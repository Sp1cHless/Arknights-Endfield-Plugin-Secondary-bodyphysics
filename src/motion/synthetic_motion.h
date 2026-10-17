#pragma once
// motion/synthetic_motion.h — synthetic gait oscillation computation
// (baseline Mode 2 / ProbeSpringAdvance math, review #11 semantics):
//   angle = ampEnv(t) * sin(phase(t))          [or jump curve]
//   target = lowPass(currentNative) × dq(axis, angle)
// The caller (MotionEngine) owns read/write timing; this module is pure math
// + envelope state so the "unconditional full compute per callback" rule
// stays intact without duplicating logic.
#include "../character/active_character.h"
#include "../common/logger.h"
#include "../config/config_types.h"
#include "gait_classifier.h"
#include "jump_controller.h"
#include "locomotion_envelope.h"

struct SyntheticMotion {
  LocomotionEnvelope envelope;
  JumpController jump;

  // Compute the output angle (radians, axis-sign applied) for this callback.
  // Advances envelope at most once per frame (multi-instance safe).
  float ComputeAngle(ActiveCharacterRuntime &c, const CharacterProfile &p) {
    // PHASE TRACKING (PLL): feed the stable-loop clip phase reference to the
    // envelope, which pulls its continuous phase toward it with a bounded
    // rate each frame (no hard snap).  On fresh engagement with negligible
    // amplitude (startup / character switch / idle->walk onset) an initial
    // snap is invisible and aligns immediately; at non-zero amplitude
    // (gait->gait switch) the pull converges smoothly instead.
    const bool wasTracking = envelope.PhaseTrackingActive();
    envelope.SetPhaseTracking(c.synthetic.phaseRefValid,
                              c.synthetic.phaseRefRad);
    if (c.synthetic.phaseRefValid && !wasTracking &&
        envelope.Amplitude() < kPhaseSnapAmpThresholdRad &&
        envelope.DownAmplitude() < kPhaseSnapAmpThresholdRad)
      envelope.SetPhase(c.synthetic.phaseRefRad);
    int gait = c.currentGait;
    // baseline kGaitAmp is RADIANS; config stores degrees -> convert here
    bool validGait = gait >= GaitIdle && gait <= GaitZipline;
    float ampTarget = validGait ? DegToRad(GaitAmplitude(p, gait)) : 0.0f;
    float downTarget = validGait ? DegToRad(GaitDownAmplitude(p, gait)) : 0.0f;
    float freqTarget = validGait ? GaitFrequency(p, gait) : 1.5f;
    // A fit belongs to exactly one gait. Never borrow Run/Sprint frequency
    // during Walk start while its loop measurement is still paused.
    if (validGait)
      freqTarget = FreqLockTargetHz(c.synthetic.freq, GaitParamRef(p, gait), gait);

    // Jump animations (any clip containing "jump", including landings) are
    // classified as Run and would write large run amplitudes during the
    // jump.  When the jump feature is OFF, keep jump clips fully native:
    // zero the targets so the envelope eases out and nothing is written.
    if (c.jumpDetected && !p.jump.enabled) {
      ampTarget = 0.0f;
      downTarget = 0.0f;
    }

    // Release tail: a stop must decay through the WHOLE stop window — the
    // stop/_to_idle clip AND the following accepted idle-loop window —
    // otherwise a slow to_idle_release_tau_sec truncates the motion
    // mid-swing the moment the idle clip takes over.  (See also the
    // matching write-gate bypass in MotionEngine::OnSyntheticCompute.)
    const bool groundLoco = validGait && gait >= GaitWalk;
    const bool releaseTail =
        ReleaseTailActive(c.synthetic.stopRelease.armed, groundLoco,
                          envelope.Amplitude(), envelope.DownAmplitude());
    if (!c.transitionToIdle && !releaseTail)
      c.synthetic.stopRelease.armed = false;
    const bool stopReleasing = c.transitionToIdle || releaseTail;
    if (stopReleasing) {
      ampTarget = 0.0f;   // to-idle fast release
      downTarget = 0.0f;
      // Freeze the frequency at the oscillator's CURRENT value for the
      // whole release.  The last gait's cadence is the motion the player
      // saw; letting freqTarget follow the stop/idle/GaitNone clip
      // classification (walk freq -> idle freq -> 1.5 default, plus
      // per-gait auto-frequency fits) reads as a sudden frequency change
      // mid-decay.  Amplitude decays to zero, frequency does not move.
      freqTarget = envelope.Frequency();
    }

    bool groundMoving = IsGroundLocomotionOnsetEligible(
        gait, c.transitionToIdle, c.jumpDetected);
    float attackTau = SelectLocomotionAttackTau(
        groundMoving, envelope.Amplitude(), envelope.DownAmplitude(),
        ampTarget, downTarget, p.envelope.amplitudeAttackTauSec,
        c.synthetic.wasGroundLocomotionMoving,
        c.synthetic.locomotionOnsetActive);
    const float frequencyTau = SelectGaitFrequencyTau(
        c.synthetic.freqTransition, gait,
        IsPhaseAlignEligible(gait, c.transitionToIdle, c.jumpDetected),
        envelope.Frequency(), freqTarget, p.envelope.frequencyTauSec);

    envelope.Advance(ampTarget, downTarget, freqTarget,
                     stopReleasing, attackTau,
                     p.envelope.toIdleReleaseTauSec,
                     frequencyTau);

    float angle;
    if (c.jumpActive) {
      // landing-only damped settle (baseline simplified jump).
      angle = jump.Tick(true, p.jump);
    } else {
      jump.Tick(false, p.jump);
      float amp = envelope.Amplitude();
      float down = envelope.DownAmplitude();
      float ph = envelope.Phase();
      // asymmetric gait: upper half-cycle swings with `amp`, lower
      // half-cycle with `down` (symmetric when down config is 0)
      if (amp > 0.001f || down > 0.001f || stopReleasing)
        angle = ph < 3.14159265358979f ? amp * sinf(ph) : down * sinf(ph);
      else
        angle = 0.0f;
    }
    angle *= c.axisSign;
    // Publish a read-only snapshot for the transform recorder.  Motion still
    // uses the envelope above directly; these fields never feed computation.
    c.synthetic.ampEnv = envelope.Amplitude();
    c.synthetic.downEnv = envelope.DownAmplitude();
    c.synthetic.freqEnv = envelope.Frequency();
    c.synthetic.phase = envelope.Phase();
    c.synthetic.releaseTail = releaseTail;
    return angle;
  }

  // Compose the absolute synthetic target on a slow native pose base.  The
  // current pose is still sampled fresh, but its locomotion-rate oscillation
  // is filtered before applying the independent synthetic oscillator.
  static void ComposeTargets(ActiveCharacterRuntime &c, float angleRad,
                             bool filterNativeOscillation,
                             Quat &outR, Quat &outL) {
    Quat curR = SafeGetLocalRotation(c.bones.breastR);
    Quat curL = SafeGetLocalRotation(c.bones.breastL);
    c.synthetic.lastNativeR = curR;
    c.synthetic.lastNativeL = curL;
    if (filterNativeOscillation)
      c.synthetic.baseFilter.ComposeLive(curR, curL, (int)c.axis, angleRad,
                                         outR, outL);
    else
      c.synthetic.baseFilter.ComposeDirect(curR, curL, (int)c.axis, angleRad,
                                           outR, outL);
  }

  static float GaitAmplitude(const CharacterProfile &p, int gait) {
    switch (gait) {
      case GaitIdle: return p.idle.amplitudeDeg;
      case GaitWalk: return p.walk.amplitudeDeg;
      case GaitRun: return p.run.amplitudeDeg;
      case GaitSprint: return p.sprint.amplitudeDeg;
      case GaitZipline: return p.zipline.amplitudeDeg;
      default: return 0.0f;
    }
  }
  // Down (-direction) amplitude; 0 in config = symmetric (= up value).
  static float GaitDownAmplitude(const CharacterProfile &p, int gait) {
    float d = 0.0f;
    switch (gait) {
      case GaitIdle: d = p.idle.amplitudeDownDeg; break;
      case GaitWalk: d = p.walk.amplitudeDownDeg; break;
      case GaitRun: d = p.run.amplitudeDownDeg; break;
      case GaitSprint: d = p.sprint.amplitudeDownDeg; break;
      case GaitZipline: d = p.zipline.amplitudeDownDeg; break;
      default: return 0.0f;
    }
    return d > 0.0f ? d : GaitAmplitude(p, gait);
  }
  static float GaitFrequency(const CharacterProfile &p, int gait) {
    switch (gait) {
      case GaitIdle: return p.idle.frequencyHz;
      case GaitWalk: return p.walk.frequencyHz;
      case GaitRun: return p.run.frequencyHz;
      case GaitSprint: return p.sprint.frequencyHz;
      case GaitZipline: return p.zipline.frequencyHz;
      default: return 1.5f;
    }
  }
};
