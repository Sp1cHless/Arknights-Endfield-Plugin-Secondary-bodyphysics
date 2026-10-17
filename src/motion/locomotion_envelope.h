#pragma once
// motion/locomotion_envelope.h — amplitude/frequency envelope with phase
// integration (baseline review #11 semantics, user-confirmed "效果非常好"):
//   desiredAngle(t) = ampEnv(t) * sin(phase(t))
//   amp env: exponential attack, exponential stop onset + finite release tail
//   freq env: exponential approach, tau 0.20s
//   phase:   phase += 2π * freqEnv * dt; fmod 2π  — NEVER reset on gait change
#include <cmath>
#include "../common/time_utils.h"
#include "../config/config_types.h"

static constexpr float kLocomotionOnsetAttackTauSec = 0.10f;
static constexpr double kStopReleaseHandoffInTau = 0.6931471805599453; // ln(2)
static constexpr double kStopReleaseDurationInTau = kStopReleaseHandoffInTau + 2.0;

// Keep the original stop down to half amplitude, then replace only its
// long tail. Both value and slope match at the handoff and vanish at end.
static inline float StopReleaseGain(double elapsedSec, double tauSec) {
  const double scaled = fmax(0.0, elapsedSec) / tauSec;
  if (scaled <= kStopReleaseHandoffInTau) return (float)exp(-scaled);
  const double remaining = fmax(0.0, 1.0 - (scaled - kStopReleaseHandoffInTau) / 2.0);
  return (float)(0.5 * remaining * remaining);
}

// PLL phase tracking: positional-pull time constant (total rate is bounded
// below, so large errors can take longer to settle) and the amplitude below
// which an initial hard snap is invisible (startup/character-switch align).
static constexpr double kPhaseLockTauSec = 0.5;
static constexpr float kPhaseSnapAmpThresholdRad = 0.02f;

// Rate-assisted PLL: integral gain (Hz per rad of phase error per second)
// and the clamp of the auxiliary rate correction.  The positional pull
// alone settles at error = (rate mismatch) x tau — while freqEnv is still
// converging (a large frequency_tau_sec) that offset lasts seconds.  The
// integral absorbs the rate mismatch independently of the frequency
// smoothing setting; freqEnv remains the free-running (long-term) authority.
static constexpr double kPhaseRateGainHzPerRad = 1.5;
static constexpr double kPhaseCorrMaxHz = 1.5;
// PLL may trim cadence, but may not halve/double it to erase phase error.
static constexpr double kPhaseCorrMaxFraction = 0.20;

// Shortest-path wrapped error to the reference in [-pi, pi).
static inline double WrapPhaseError(double phase, double ref) {
  double err = fmod(ref - phase, 2.0 * 3.14159265358979);
  if (err > 3.14159265358979) err -= 2.0 * 3.14159265358979;
  else if (err < -3.14159265358979) err += 2.0 * 3.14159265358979;
  return err;
}

// Selects an internal fast attack only for a newly accepted ground-locomotion
// epoch. State lives with ActiveCharacterRuntime so switch/reload resets it;
// this parameter is deliberately not part of the user preset/UI schema.
static float SelectLocomotionAttackTau(
    bool groundMoving, float ampNow, float downNow, float ampTarget,
    float downTarget, float normalAttackTau, bool &wasGroundMoving,
    bool &onsetActive) {
  if (!groundMoving) {
    wasGroundMoving = false;
    onsetActive = false;
    return normalAttackTau;
  }
  if (!wasGroundMoving) onsetActive = true;
  wasGroundMoving = true;
  if (onsetActive) {
    float ampTolerance = fmaxf(0.001f, fabsf(ampTarget) * 0.05f);
    float downTolerance = fmaxf(0.001f, fabsf(downTarget) * 0.05f);
    if (fabsf(ampTarget - ampNow) <= ampTolerance &&
        fabsf(downTarget - downNow) <= downTolerance)
      onsetActive = false;
  }
  return onsetActive ? kLocomotionOnsetAttackTauSec : normalAttackTau;
}

// Only a verified stop may extend its decay into an accepted idle loop.
static inline bool ReleaseTailActive(bool stopArmed, bool groundLocomotion,
                                     float ampRad, float downRad) {
  return stopArmed && !groundLocomotion &&
         (ampRad > 0.0f || downRad > 0.0f);
}

struct LocomotionEnvelope {
  float ampEnv = 0.0f;
  float downEnv = 0.0f;   // down-amplitude channel (asymmetric gait)
  float freqEnv = 1.5f;
  double phase = 0.0;
  double phaseRef = 0.0;          // PLL tracking target ([0,2pi))
  double phaseLockTauSec = 0.0;   // >0 -> bounded pull toward phaseRef
  double phaseCorrHz = 0.0;       // PLL integral rate correction (clamped)
  bool stopReleasing = false;
  double releaseElapsedSec = 0.0;
  double releaseDurationSec = 0.0;
  float releaseStartAmp = 0.0f;
  float releaseStartDown = 0.0f;
  FrameGate gate;  // frame-once advance (multi-instance safe)

  // PLL phase tracking: while valid, every Advance pulls the continuous
  // phase toward `ref` with an exponential time constant (shortest wrapped
  // path).  The phase is never snapped while tracking — only a smooth,
  // bounded convergence, so amplitude/frequency/phase all transition
  // smoothly between gaits.
  void SetPhaseTracking(bool valid, double ref) {
    phaseRef = ref;
    if (!valid) phaseCorrHz = 0.0;  // re-engage locks fresh
    phaseLockTauSec = valid ? kPhaseLockTauSec : 0.0;
  }
  bool PhaseTrackingActive() const { return phaseLockTauSec > 0.0; }

  // Advance the envelope.  Returns true when a frame actually advanced.
  bool Advance(float ampTarget, float downTarget, float freqTarget,
               bool toIdle, float attackTauSec, float toIdleTauSec,
               float freqTauSec) {
    return AdvanceDt((float)gate.Tick(), ampTarget, downTarget, freqTarget,
                     toIdle, attackTauSec, toIdleTauSec, freqTauSec);
  }

  // Same motion core with an explicit interval for deterministic replay.
  bool AdvanceDt(float dt, float ampTarget, float downTarget, float freqTarget,
                 bool toIdle, float attackTauSec, float toIdleTauSec,
                 float freqTauSec) {
    if (!std::isfinite(dt) || dt <= 0.0f) return false;
    if (dt > 0.05f) dt = 0.05f;
    float kF = 1.0f - expf(-dt / freqTauSec);
    if (toIdle && ampTarget == 0.0f && downTarget == 0.0f) {
      if (!stopReleasing) {
        stopReleasing = true;
        releaseElapsedSec = 0.0;
        releaseDurationSec = kStopReleaseDurationInTau * fmax(0.001, (double)toIdleTauSec);
        releaseStartAmp = ampEnv;
        releaseStartDown = downEnv;
      }
      releaseElapsedSec = fmin(releaseDurationSec, releaseElapsedSec + dt);
      // Snapshot once per stop; entering idle must not restart this clock.
      const float gain = releaseElapsedSec >= releaseDurationSec ? 0.0f :
          StopReleaseGain(releaseElapsedSec, releaseDurationSec / kStopReleaseDurationInTau);
      ampEnv = releaseStartAmp * gain;
      downEnv = releaseStartDown * gain;
    } else {
      stopReleasing = false;
      float ampTau = toIdle ? toIdleTauSec : attackTauSec;
      float kA = 1.0f - expf(-dt / ampTau);
      ampEnv += (ampTarget - ampEnv) * kA;
      downEnv += (downTarget - downEnv) * kA;
    }
    freqEnv += (freqTarget - freqEnv) * kF;
    phase += 2.0 * 3.14159265358979 * freqEnv * dt;
    if (phaseLockTauSec > 0.0) {
      const double error = WrapPhaseError(phase, phaseRef);
      const double limit = fmin(kPhaseCorrMaxHz,
                               fabs((double)freqEnv) * kPhaseCorrMaxFraction);
      phaseCorrHz = fmax(-limit, fmin(limit, phaseCorrHz));
      const double positionalHz = error * (1.0 - exp(-dt / phaseLockTauSec)) /
                                  (6.28318530717959 * dt);
      const double candidate = fmax(-limit, fmin(limit,
          phaseCorrHz + error * kPhaseRateGainHzPerRad * dt));
      const double requested = positionalHz + candidate;
      // Do not wind up the integral while the same-direction request is
      // saturated; allow it to unwind when it opposes the current error.
      if (fabs(requested) <= limit || requested * error < 0.0)
        phaseCorrHz = candidate;
      const double correctionHz = fmax(-limit, fmin(limit,
          positionalHz + phaseCorrHz));
      phase += 6.28318530717959 * correctionHz * dt;
    } else {
      phaseCorrHz = 0.0;
    }
    phase = fmod(phase, 2.0 * 3.14159265358979);
    if (phase < 0.0) phase += 2.0 * 3.14159265358979;
    return true;
  }

  float Amplitude() const { return ampEnv; }
  float DownAmplitude() const { return downEnv; }
  float Frequency() const { return freqEnv; }
  float Phase() const { return (float)phase; }

  // Anchor the oscillator phase to an external reference (locomotion-clip
  // normalizedTime derived phase).  Caller must snap at ampEnv ~ 0 onset so
  // the phase jump is invisible.  Keeps the phase in [0, 2pi) invariant.
  void SetPhase(double ph) {
    phase = fmod(ph, 2.0 * 3.14159265358979);
    if (phase < 0.0) phase += 2.0 * 3.14159265358979;
    phaseCorrHz = 0.0;  // fresh anchor: no stale rate
  }

  void Reset() {
    ampEnv = 0.0f;
    downEnv = 0.0f;
    freqEnv = 1.5f;
    phase = 0.0;
    phaseRef = 0.0;
    phaseLockTauSec = 0.0;
    phaseCorrHz = 0.0;
    stopReleasing = false;
    releaseElapsedSec = 0.0;
    releaseDurationSec = 0.0;
    releaseStartAmp = 0.0f;
    releaseStartDown = 0.0f;
    gate = FrameGate();
  }
};
