#pragma once
// motion/gait_sampler.h — main-character gait sampling (baseline
// ProbeSampleGait semantics):
//   - entity refresh every 500ms (squad-swap following)
//   - ONLY the main animator is sampled (PreLateTick fires for every member)
//   - 20Hz clip sampling via AnimatorClipReader, weight-max selection
//   - character switch -> reset active runtime + re-find bones
//   - bone discovery on demand
#include <cstring>
#include "../character/active_character.h"
#include "../character/bone_resolver.h"
#include "../common/logger.h"
#include "../config/config_types.h"
#include "../diagnostics/jump_phase_a_probe.h"
#include "../diagnostics/movement_signal_probe.h"
#include "../il2cpp/animator_clip_reader.h"
#include "freq_lock.h"
#include "gait_classifier.h"

static JumpPhaseAObservation JumpPhaseABaseObservation(
    const ActiveCharacterRuntime &active, const ConfigSnapshot &cfg,
    DWORD now, const ClipReadStatus &status,
    const MovementSignalSample &movement) {
  JumpPhaseAObservation observation;
  observation.sampleMs = now;
  observation.pluginEnabled = cfg.pluginEnabled;
  observation.readOk = status.readOk;
  observation.readFailure = static_cast<uint32_t>(status.failure);
  observation.reportedCount = status.reportedCount;
  observation.parsedCount = status.parsedCount;
  observation.truncated = status.truncated;
  snprintf(observation.characterId, sizeof(observation.characterId), "%s",
           active.characterId);
  observation.animator = active.animator;
  observation.cachedGait = active.currentGait;
  observation.cachedTransitionToIdle = active.transitionToIdle;
  observation.cachedJump = active.jumpDetected;
  observation.cachedLanding = active.jumpActive;
  observation.phaseDActive = !cfg.pluginEnabled;
  observation.movement = movement;
  return observation;
}

static bool PhaseDEntityAnimatorPairValid(
    const ActiveCharacterRuntime &active) {
  if (!g_mainCharEntity || !active.animator ||
      active.animator != g_cachedAnimator)
    return false;
  __try {
    const int entityAnimOffset =
        SafeOff(OFF_entityComplexAnim, "entityComplexAnim");
    const int animatorOffset =
        SafeOff(OFF_complexAnimAnimator, "complexAnimAnimator");
    if (entityAnimOffset < 0 || animatorOffset < 0) return false;
    void *complexAnim = *reinterpret_cast<void **>(
        static_cast<char *>(g_mainCharEntity) + entityAnimOffset);
    if (!complexAnim) return false;
    void *animator = *reinterpret_cast<void **>(
        static_cast<char *>(complexAnim) + animatorOffset);
    return animator == active.animator;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

static void PublishJumpLiveSignal(ActiveCharacterRuntime &active, DWORD now,
                                  const MovementSignalSample &movement,
                                  bool clipReadValid, bool jumpStartActive,
                                  bool landingActive, bool attackActive,
                                  bool ziplineActive) {
  uint32_t next = active.jumpSignal.serial + 1u;
  if (next == 0) next = 1;
  active.jumpSignal.serial = next;
  active.jumpSignal.sampleMs = now;
  active.jumpSignal.entity = movement.entity;
  active.jumpSignal.identityValid = movement.identityValid;
  active.jumpSignal.clipReadValid = clipReadValid;
  active.jumpSignal.fallingSpeedValid =
      movement.fallingSpeedValid && std::isfinite(movement.fallingSpeed);
  active.jumpSignal.fallingSpeed = movement.fallingSpeed;
  active.jumpSignal.teleported =
      movement.teleportedValid && movement.teleportedThisFrame;
  active.jumpSignal.jumpStartActive = jumpStartActive;
  active.jumpSignal.landingActive = landingActive;
  active.jumpSignal.attackActive = attackActive;
  active.jumpSignal.ziplineActive = ziplineActive;
}

static void JumpPhaseAObserveReadFailure(
    const ActiveCharacterRuntime &active, const ConfigSnapshot &cfg,
    DWORD now, const ClipReadStatus &status,
    const MovementSignalSample &movement) {
  JumpPhaseAObservation observation =
      JumpPhaseABaseObservation(active, cfg, now, status, movement);
  g_jumpPhaseATimeline.Observe(observation);
}

static void JumpPhaseAObserveClips(
    const ActiveCharacterRuntime &active, const ConfigSnapshot &cfg,
    DWORD now, const ClipReadStatus &status, const ClipSample *clips,
    size_t count, bool sampleJump, bool sampleLanding,
    const MovementSignalSample &movement) {
  JumpPhaseAObservation observation =
      JumpPhaseABaseObservation(active, cfg, now, status, movement);
  size_t copied = count < kJumpPhaseAMaxClips ? count : kJumpPhaseAMaxClips;
  observation.parsedCount = count;
  observation.sampleJumpEvidence = sampleJump;
  observation.sampleLandingEvidence = sampleLanding;
  for (size_t i = 0; i < copied; ++i) {
    snprintf(observation.clips[i].name, sizeof(observation.clips[i].name),
             "%s", clips[i].name);
    observation.clips[i].weight = clips[i].weight;
    observation.clips[i].durationSec = clips[i].durationSec;
  }
  g_jumpPhaseATimeline.Observe(observation);
}

// ---- auto-frequency alignment ----  pure logic + state live in
// motion/freq_lock.h (unit-testable); the 20Hz sampling layer below only
// feeds it the per-gait params, the clip name, normalizedTime and now.
class GaitSampler {
public:
  GaitSampler(AnimatorClipReader &reader) : reader_(reader) {}

  // PATCH 2026-10: switch hysteresis.
  // The game's controlled-entity chain can flip between two characters for
  // several consecutive frames (observed after quests that pull a second
  // character model into the scene).  Rebinding on every flip rebuilt the
  // synthetic base and the bone pointers constantly and produced
  // cross-character writes (chest rotating away).  Require the candidate to
  // stay the main animator for kSwitchSettleMs before rebinding.
  static constexpr DWORD kSwitchSettleMs = 150;
  void *switchCandidate_ = nullptr;
  DWORD switchCandidateSinceMs_ = 0;

  // Hot-reload / forced refresh: reset the active runtime but keep the
  // animator, then re-identify + re-bind profile from the NEW snapshot.
  // Runs on the main thread (PreLateTick).
  void ForceRefresh(ActiveCharacterRuntime &active, const ConfigSnapshot &cfg) {
    void *animator = active.animator;
    ProbeLog("[CFG] hot reload -> refreshing active character\n");
    ResetActiveCharacter(active);
    active.animator = animator;
    if (animator) {
      ResolveCharacterId(active);
      active.rootTransform = SafeGetComponentTransform(animator);
      BindProfile(active, cfg);
    }
    active.valid = animator != nullptr;
  }

  // Called from PreLateTick (per instance).  Internally filters to the main
  // character only.  (The per-frame squad call counter lives in
  // MotionEngine — it must count EVERY callback, not just the main one.)
  void ObserveCallback(void *callerAnimator, const ConfigSnapshot &cfg,
                       ActiveCharacterRuntime &active) {
    DWORD now = NowMs();

    // 500ms entity refresh (baseline)
    if (now - active.lastIdentityRefreshMs >=
        cfg.global.entityRefreshIntervalMs) {
      active.lastIdentityRefreshMs = now;
      RefreshMainCharacterAnimator();
    }

    // ONLY the main character (baseline filter)
    if (!g_cachedAnimator || callerAnimator != g_cachedAnimator) return;

    // Character switch detection: reset everything, re-find bones
    if (callerAnimator != active.animator) {
      // PATCH: settle the candidate first (see kSwitchSettleMs).
      if (switchCandidate_ != callerAnimator) {
        switchCandidate_ = callerAnimator;
        switchCandidateSinceMs_ = now;
        ProbeLog("[SWITCH-PENDING] animator %p -> %p (settling %ums)\n",
                 (void *)active.animator, (void *)callerAnimator,
                 (unsigned)kSwitchSettleMs);
      }
      if (now - switchCandidateSinceMs_ < kSwitchSettleMs) {
        // Not settled yet: keep the previous binding untouched.  The identity
        // gate in the motion engine suppresses writes while the chain is
        // ambiguous, so no wrong-character write can happen in the meantime.
        return;
      }
      switchCandidate_ = nullptr;
      bool isSwitch = (active.animator != nullptr);
      ProbeLog("[SWITCH] animator %p -> %p%s\n", (void *)active.animator,
               (void *)callerAnimator, isSwitch ? " (re-finding)" : "");
      ResetActiveCharacter(active);
      active.animator = callerAnimator;
      ResolveCharacterId(active);
      active.rootTransform = SafeGetComponentTransform(callerAnimator);
      BindProfile(active, cfg);
      active.valid = true;
    } else {
      switchCandidate_ = nullptr;  // PATCH: stable again
    }

    // Bone discovery when missing (main character only)
    if (!active.bones.found && active.profile && active.profile->enabled &&
        active.rootTransform) {
      if (!ResolveBreastBonesWithProfile(active.rootTransform,
                                         *active.profile, active.bones)) {
        ProbeLog("[BONE] not found yet (animator=%p)\n",
                 (void *)active.animator);
      } else {
        // Axis: explicit preset value wins; otherwise the bone-family
        // default discovered from the actual bone name (spec §8.2 — family
        // defaults are the profile-generation rule, explicit config the
        // final authority).
        if (active.profile->axisExplicit) {
          active.axis = active.profile->axis.axis;
          active.axisSign = active.profile->axis.sign;
        } else {
          active.axis = active.bones.axis;
          active.axisSign = 1.0f;
        }
        ProbeLog("[BONE] R=%s L=%s axis=%d%s sign=%+.0f\n",
                 active.bones.rightName, active.bones.leftName,
                 (int)active.axis, active.profile->axisExplicit ? "" : "(auto)",
                 active.axisSign);
      }
    }

    // 20Hz sampling (baseline)
    if (now - active.lastGaitSampleMs < cfg.global.gaitSampleIntervalMs)
      return;
    active.lastGaitSampleMs = now;

    ClipSample clips[8];
    size_t count = 0;
    ClipReadStatus readStatus;
    MovementSignalSample movement;
    const bool liveJumpSignalNeeded =
        cfg.pluginEnabled && active.profile && active.profile->jump.enabled &&
        active.profile->jump.mode == "landing_damped";
    if (!cfg.pluginEnabled || liveJumpSignalNeeded) {
      const bool identityValid = PhaseDEntityAnimatorPairValid(active);
      movement = g_movementSignalProbe.Sample(g_mainCharEntity, identityValid);
    }
    active.synthetic.clipSampleMs = now;
    active.synthetic.clipReadOk = false;
    active.synthetic.dominantClip[0] = 0;
    active.synthetic.releaseBlockedClip[0] = 0;
    if (!reader_.ReadLayer0(callerAnimator, clips, 8, count, &readStatus)) {
      active.synthetic.stopRelease.armed = false;
      active.synthetic.stopRelease.blocked = true;
      active.synthetic.phaseRefValid = false;
      FreqLockPause(active.synthetic.freq);
      PublishJumpLiveSignal(active, now, movement, false, false, false, false,
                            false);
      JumpPhaseAObserveReadFailure(active, cfg, now, readStatus, movement);
      return;
    }

    active.synthetic.clipReadOk = true;
    float dominantWeight = -1.0f;
    int bestGait = GaitNone;
    GaitClipSelection bestClip;
    bool transToIdle = false;
    bool landing = false;
    bool jumpStart = false;
    bool attackActive = false;
    bool ziplineActive = false;
    bool bestLoopStable = false;
    char bestClipName[128] = {0};
    StopReleaseSample releaseSample;
    active.jumpDetected = false;  // recomputed every sample
    for (size_t i = 0; i < count; i++) {
      GaitClassification cls =
          ClassifyClipNameFull(clips[i].name, active.profile);
      releaseSample.Observe(clips[i].name, cls, clips[i].weight);
      const float clipRank = SafeClipBlendWeight(clips[i].weight);
      if (clipRank > dominantWeight) {
        dominantWeight = clipRank;
        snprintf(active.synthetic.dominantClip,
                 sizeof(active.synthetic.dominantClip), "%s", clips[i].name);
      }
      if (releaseSample.blocked && !active.synthetic.releaseBlockedClip[0])
        snprintf(active.synthetic.releaseBlockedClip,
                 sizeof(active.synthetic.releaseBlockedClip), "%s",
                 clips[i].name[0] ? clips[i].name : "<unnamed>");
      if (bestClip.Observe(cls, clips[i].weight)) {
        bestGait = cls.gait;
        bestLoopStable = cls.loopStable;
        snprintf(bestClipName, sizeof(bestClipName), "%s", clips[i].name);
      }
      if (cls.transitionToIdle) transToIdle = true;
      if (cls.landingDetected) landing = true;
      if (cls.jumpDetected && strstr(clips[i].name, "jump_start"))
        jumpStart = true;
      if (IsAttackClip(clips[i].name)) attackActive = true;
      if (strstr(clips[i].name, "zipline")) ziplineActive = true;
      if (cls.jumpDetected) active.jumpDetected = true;
    }
    active.currentGait = bestGait;
    active.transitionToIdle = transToIdle;
    active.jumpActive = landing;
    active.synthetic.stopRelease.Observe(releaseSample);

    // PHASE TRACKING (PLL): publish the stable-loop clip's real phase
    // (m_NormalizedTime, 2x symmetric-loop mapping + per-gait config offset)
    // as a bounded tracking target.  The envelope pulls its continuous phase
    // toward this reference every frame (no hard snap), so amplitude /
    // frequency / phase all transition smoothly between gaits.  start/stop/
    // _to_ transition clips and jump clips (loopStable=false) have no usable
    // locomotion phase and disable tracking.  The per-gait phase_align
    // switch disables the reference entirely (free integration).
    const bool frequencyEligible =
        active.profile && active.profile->enabled &&
        active.profile->motionMode == MotionMode::Synthetic &&
        IsPhaseAlignEligible(bestGait, transToIdle, active.jumpDetected);
    if (frequencyEligible)
      FreqLockPrepare(active.synthetic.freq, GaitParamRef(*active.profile, bestGait),
                      bestGait, freqCache_, active.characterId);
    const bool trackEligible = bestLoopStable && frequencyEligible;
    active.synthetic.phaseRefValid = false;
    if (trackEligible) {
      float norm = reader_.ReadNormalizedTime(callerAnimator);
      if (std::isfinite(norm) && norm >= 0.0f) {
        // auto-frequency alignment runs regardless of the phase-align switch
        const GaitParam &gp = GaitParamRef(*active.profile, bestGait);
        FreqLockTick(active.synthetic.freq, gp, bestClipName, norm, now,
                     bestGait, freqCache_, active.characterId);
        static DWORD s_freqLogT = 0;
        if (RateLimit(s_freqLogT, 2000) && active.synthetic.freq.measValid)
          ProbeLog("[FREQ] gait=%d cfg=%.2f meas=%.2f use=%.2f%s\n",
                   bestGait, gp.frequencyHz, active.synthetic.freq.measHz,
                   active.synthetic.freq.useHz,
                   active.synthetic.freq.correcting ? " (correcting)" : "");
        if (GaitPhaseAlignEnabled(*active.profile, bestGait)) {
          active.synthetic.phaseRefRad = ApplyGaitPhaseOffset(
              PhaseFromNormalizedTime(norm, kLocomotionPhaseCyclesPerLoop),
              GaitPhaseOffsetDeg(*active.profile, bestGait));
          active.synthetic.phaseRefValid = true;
          static DWORD s_phaseLogT = 0;
          if (RateLimit(s_phaseLogT, 2000))
            ProbeLog("[PHASE] track gait=%d norm=%.3f ref=%.1fdeg\n", bestGait,
                     norm, RadToDeg((float)active.synthetic.phaseRefRad));
        } else {
          active.synthetic.phaseRefValid = false;
        }
      } else {
        FreqLockPause(active.synthetic.freq);
      }
    } else {
      active.synthetic.phaseRefValid = false;
      FreqLockPause(active.synthetic.freq);
    }

    PublishJumpLiveSignal(active, now, movement, true, jumpStart, landing,
                          attackActive, ziplineActive);
    JumpPhaseAObserveClips(active, cfg, now, readStatus, clips, count,
                           active.jumpDetected, landing, movement);

    // Diagnostic: dump clip names (helps onboarding new gaits such as
    // zipline/slide).  Rate-limited to ~2s.
    static DWORD s_gaitLogT = 0;
    static DWORD s_clipLogT = 0;
    if (RateLimit(s_gaitLogT, 2000))
      ProbeLog("[GAIT] gait=%d count=%zu\n", bestGait, count);
    if (RateLimit(s_clipLogT, 2000)) {
      float spd = reader_.ReadAnimatorSpeed(callerAnimator);
      // actual loop frequency from normalizedTime delta (includes
      // state-speed multipliers that Animator.speed misses)
      static float s_lastNorm = -1.0f;
      static DWORD s_lastNormT = 0;
      float cyc = -1.0f;
      float norm = reader_.ReadNormalizedTime(callerAnimator);
      if (norm >= 0.0f && s_lastNorm >= 0.0f && s_lastNormT) {
        float dNorm = norm - s_lastNorm;
        if (dNorm < 0.0f) dNorm += 1.0f;  // wrapped
        float dt = (float)(NowMs() - s_lastNormT) / 1000.0f;
        if (dt > 0.1f) cyc = dNorm / dt;
      }
      s_lastNorm = norm;
      s_lastNormT = NowMs();
      static DWORD s_normDiagT = 0;
      if (RateLimit(s_normDiagT, 5000))
        ProbeLog("[NORM] norm=%.3f off=%d stateinfo=%p\n",
                 norm, (int)reader_.NormOffset(), (void *)reader_.StateInfoMethod());
      for (size_t i = 0; i < count && i < 8; i++)
        ProbeLog("[CLIP] %s dur=%.3fs spd=%.2f eff=%.3fs cyc=%.2f/s w=%.2f\n",
                 clips[i].name[0] ? clips[i].name : "(none)",
                 clips[i].durationSec, spd,
                 clips[i].durationSec > 0.001f
                     ? clips[i].durationSec / spd
                     : 0.0f,
                 cyc >= 0.0f ? cyc : -1.0f,
                 clips[i].weight);
    }
  }

private:
  // Bind the active character's profile from the current snapshot
  // (used on switch AND on hot reload).
  static void BindProfile(ActiveCharacterRuntime &active,
                          const ConfigSnapshot &cfg) {
    active.profile = nullptr;
    active.profileFound = false;
    if (cfg.characters.count(active.characterId) > 0) {
      active.profile = &cfg.characters.at(active.characterId);
      active.profileFound = true;
      if (!active.profile->enabled) {
        ProbeLog("[CHAR] id=%s profile=DISABLED -> native only\n",
                 active.characterId);
      } else {
        ProbeLog("[CHAR] id=%s profile=FOUND mode=%d\n",
                 active.characterId, (int)active.profile->motionMode);
      }
    } else {
      ProbeLog("[CHAR] id=%s UNSUPPORTED -> native only\n",
               active.characterId);
    }
  }

  AnimatorClipReader &reader_;
  // Session-level fitted-frequency cache (learned per character+gait).
  // Fixed-size, zero allocation; survives character switches so re-entering
  // a character reuses its fitted frequency instead of re-learning it.
  FreqCache freqCache_;
};
