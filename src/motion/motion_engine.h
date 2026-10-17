#pragma once
// motion/motion_engine.h — the single motion compute/write orchestrator.
//
// BASELINE-CRITICAL SEMANTICS (architecture spec §0.1 / §11.2 / 禁止清单):
//   - OnPreLateTick runs the FULL read->compute->write for EVERY PreLateTick
//     callback, WITHOUT filtering by caller animator.  once-per-frame /
//     same-frame replay / main-instance-only variants all FAILED in the
//     baseline campaign — do not "clean up" this behavior.
//   - The absolute target (native × dq) is computed HERE only; every other
//     write point (LateTick / SyncCalc) idempotently REPLAYS the cached
//     target (no Get, no dq, no envelope advance).
//   - No file I/O / JSON / managed allocation inside callbacks.  Marker
//     state is cached by the worker thread and read from a plain struct.
#include "../character/active_character.h"
#include "../common/logger.h"
#include "../common/time_utils.h"
#include "../config/config_types.h"
#include "../il2cpp/il2cpp_api.h"
#include "gait_sampler.h"
#include "jump_inertial_source.h"
#include "native_amplifier.h"
#include "synthetic_motion.h"

// Marker cache updated by the worker thread (low frequency), read-only here.
struct MarkerState {
  bool spring = false;   // legacy synthetic gate (spring_test.txt)
  bool amplify = false;  // legacy amplify gate (amplify_test.txt)
};

// Diagnostics hooks executed ON THE MAIN THREAD (PreLateTick) so Unity API
// calls stay on the main thread.  All null when diagnostics are disabled —
// zero production cost (spec §19: diagnostics must not affect production).
struct DiagHooks {
  void (*axisTest)(const ActiveCharacterRuntime &) = nullptr;  // armed fixed-angle
  void (*recorder)(const ActiveCharacterRuntime &) = nullptr;  // per-frame CSV
  void (*clipInspect)(void *animator) = nullptr;               // rate-limited
  void (*boneScan)(void *root) = nullptr;                      // one-shot
  // written by the worker thread, read on the main thread -> volatile
  // (x86: atomic bool read/write; volatile prevents register caching)
  volatile bool boneScanPending = false;
};

class MotionEngine {
public:
  MotionEngine(GaitSampler &sampler, ActiveCharacterRuntime &active,
               MarkerState &markers)
      : sampler_(sampler), active_(active), markers_(markers) {}

  DiagHooks diag;

  // ---- PreLateTick: unconditional full compute per callback ----
  void OnPreLateTick(void *callerAnimator) {
    const ConfigSnapshot *cfg = ConfigAcquire();
    if (!cfg) return;  // config not installed yet — no write
    // NOTE: no early return for pluginEnabled=false here.  Character
    // identification (ObserveCallback / 500ms refresh / ForceRefresh)
    // must keep running while globally disabled, otherwise switching
    // characters during a disabled window leaves the id stale/empty.
    // The write paths (OnSyntheticCompute / ReplayTarget / OnLateTick)
    // each check cfg->pluginEnabled before writing.

    // Hot reload: revision change -> force full active refresh (profile
    // pointers point into the old snapshot).
    if (cfg->revision != lastRevision_) {
      lastRevision_ = cfg->revision;
      sampler_.ForceRefresh(active_, *cfg);
      jumpInertial_.Reset();
    }

    int frame = GetUnityFrame();

    // character switch -> reset engine-side state (amplifier base etc.).
    // Compare AFTER ObserveCallback so a same-frame switch is caught here
    // (active.animator already holds the new value).
    sampler_.ObserveCallback(callerAnimator, *cfg, active_);
    if (active_.animator != lastAnimator_) {
      lastAnimator_ = active_.animator;
      OnCharacterReset();
    }

    // FRAME DEDUP for writes: every AnimatorMono in the scene fires
    // PreLateTick and would re-apply the same offset to the main bones,
    // stacking the angle N times per frame (scene-dependent amplitude —
    // solo near NPCs looks 2-4x stronger than solo in an empty scene).
    // Only the FIRST callback of a frame writes; later ones return here.
    // Identity/switch handling above still runs for every callback.
    if (frame >= 0 && frame == lastWriteFrame_) return;
    lastWriteFrame_ = frame;

    // Diagnostics that must run even for UNKNOWN characters (fail-closed
    // gate below would skip them): bone scan + clip inspect only need the
    // animator/root transform, not a resolved profile.
    if (diag.boneScanPending && diag.boneScan && active_.rootTransform) {
      diag.boneScanPending = false;
      diag.boneScan(active_.rootTransform);
    }
    if (diag.clipInspect) diag.clipInspect(callerAnimator);

    if (!active_.valid) return;
    if (!active_.profile) return;
    if (!active_.profile->enabled) return;

    if (active_.profile->motionMode == MotionMode::Synthetic) {
      OnSyntheticCompute();
    }
    // AmplifyNative runs on LateTick (baseline timing); nothing here.
    // Off: NO WRITE (native passes through untouched).

    // Diagnostics needing resolved bones (no-op when disabled)
    if (diag.axisTest) diag.axisTest(active_);       // may override angle
    if (diag.recorder) diag.recorder(active_);
  }

  // ---- LateTick / SyncCalc: idempotent replay of the last target ----
  void ReplayTarget() {
    const ConfigSnapshot *cfg = ConfigAcquire();
    if (!cfg) return;
    if (!cfg->pluginEnabled) return;  // global switch OFF — no write
    if (!active_.valid) return;
    if (!active_.profile || !active_.profile->enabled) return;
    if (active_.profile->motionMode != MotionMode::Synthetic) return;
    if (!active_.synthetic.targetValid) return;
    if (!active_.bones.breastR || !active_.bones.breastL) return;
    // verified-window gate (baseline: 150ms since main-char bone verify)
    if (NowMs() - active_.replay.bonesVerifiedMs >
        cfg->global.replayVerifyWindowMs)
      return;
    __try {
      SafeSetLocalRotation(active_.bones.breastR, active_.synthetic.targetR);
      SafeSetLocalRotation(active_.bones.breastL, active_.synthetic.targetL);
    } __except (1) {
    }
  }

  // ---- LateTick: amplify_native mode (baseline Mode 1 timing) ----
  void OnLateTick() {
    const ConfigSnapshot *cfg = ConfigAcquire();
    if (!cfg) return;
    if (!cfg->pluginEnabled) return;  // global switch OFF — no write
    if (!active_.valid) return;
    if (!active_.profile || !active_.profile->enabled) return;
    if (active_.profile->motionMode == MotionMode::AmplifyNative) {
      if (WriteGateAmplify()) {
        amplifier_.enabled = true;
        amplifier_.Apply(active_, active_.profile->nativeAmplify.factor);
      } else {
        amplifier_.enabled = false;
      }
    } else {
      amplifier_.enabled = false;
    }
  }

  void OnCharacterReset() {
    active_.synthetic = SyntheticRuntime();
    active_.jump = JumpRuntime();
    jumpInertial_.Reset();
    active_.replay = ReplayRuntime();
    amplifier_.Reset();
  }

  // ---- gates (V2 default: config-driven; legacy markers only when
  // legacy_marker_mode=true) ----
  bool WriteGateSynthetic() {
    const ConfigSnapshot *cfg = ConfigAcquire();
    if (!cfg) return false;
    if (!cfg->global.legacyMarkerMode) return true;
    return markers_.spring;
  }

  bool WriteGateAmplify() {
    const ConfigSnapshot *cfg = ConfigAcquire();
    if (!cfg) return false;
    if (!cfg->global.legacyMarkerMode) return true;
    return markers_.amplify;
  }

private:
  // Baseline Mode 2 core (ProbeSpringAdvance): full read->compute->write.
  void OnSyntheticCompute() {
    const ConfigSnapshot *cfg = ConfigAcquire();
    if (!cfg) return;
    if (!cfg->pluginEnabled) return;  // global switch OFF — no write
    if (!active_.bones.breastR || !active_.bones.breastL) return;
    __try {
      float angle = synthetic_.ComputeAngle(active_, *active_.profile);
      const bool liveJumpConfigured =
          active_.profile->jump.enabled &&
          active_.profile->jump.mode == "landing_damped";
      JumpInertialOutput jumpOut = jumpInertial_.Tick(
          NowMs(), active_.jumpSignal, active_.profile->jump,
          liveJumpConfigured);

      // An inertial Jump epoch owns the angle until its bounded landing tail
      // settles. A Jump clip without an accepted exact-start epoch (notably a
      // direct fall/heavy landing) remains completely native.
      const bool jumpOwnsIntent =
          liveJumpConfigured &&
          (active_.jumpDetected || jumpOut.valid || jumpOut.directLanding);
      if (jumpOwnsIntent) {
        if (!jumpOut.valid) {
          active_.synthetic.baseFilter.Reset();
          active_.synthetic.targetValid = false;
          return;
        }
        angle = jumpOut.angleRad * active_.axisSign *
                AutomaticJumpDirection(active_.axis);
      }
      active_.synthetic.outAngleRad = angle;

      if (jumpOut.acceptedStart)
        ProbeLog("[JUMP-E] epoch=%u Rising accepted\n", jumpOut.eventEpoch);
      if (jumpOut.acceptedLanding)
        ProbeLog("[JUMP-E] epoch=%u LandingTail accepted\n",
                 jumpOut.eventEpoch);
      if (jumpOut.directLanding)
        ProbeLog("[JUMP-E] direct landing -> native-only\n");

      // Unsupported/idle clips and disabled Jump still advance the envelope
      // toward zero, but must never compose, write, or replay a synthetic
      // target. This is the fail-closed write boundary.
      // A verified stop may finish decaying into an accepted idle loop.
      // Unsupported clips cancel it, including blends retaining a stop clip.
      const bool releaseTail =
          !active_.jumpDetected && active_.synthetic.releaseTail;
      const bool blockedStop = active_.transitionToIdle &&
                               active_.synthetic.stopRelease.blocked;
      if (!jumpOut.valid && (blockedStop ||
          !CanWriteSynthetic(active_.currentGait, active_.jumpDetected,
                             active_.profile->jump.enabled) &&
          !releaseTail)) {
        active_.synthetic.baseFilter.Reset();
        active_.synthetic.targetValid = false;
        return;
      }

      Quat targetR, targetL;
      const bool filterNativeOscillation =
          !jumpOwnsIntent && !active_.transitionToIdle &&
          !active_.synthetic.releaseTail;
      SyntheticMotion::ComposeTargets(active_, angle, filterNativeOscillation,
                                      targetR, targetL);

      active_.synthetic.targetR = targetR;
      active_.synthetic.targetL = targetL;
      active_.synthetic.targetValid = true;

      if (WriteGateSynthetic()) {
        SafeSetLocalRotation(active_.bones.breastR, targetR);
        SafeSetLocalRotation(active_.bones.breastL, targetL);
        active_.replay.bonesVerifiedMs = NowMs();
      }

      static DWORD s_preLogT = 0;
      if (RateLimit(s_preLogT, 2000))
        ProbeLog("[PRE] gait=%d ang=%.1fdeg\n",
                 active_.currentGait, RadToDeg(angle));
    } __except (1) {
    }
  }

  // Unity Time.frameCount via reflection (baseline diagnostic + squad
  // grouping source; QPC is NOT a frame boundary for 4-instance squads).
  static int GetUnityFrame() {
    static void *s_frameCountMethod = nullptr;
    if (!s_frameCountMethod) {
      void *domain = il2cpp_domain_get();
      if (domain) {
        size_t ac = 0;
        void **asms = il2cpp_domain_get_assemblies(domain, &ac);
        void *tClass = FindClass("UnityEngine", "Time", asms, ac);
        if (tClass)
          s_frameCountMethod = FindMethod(tClass, "get_frameCount", 0);
      }
    }
    if (!s_frameCountMethod) return -1;
    void *r = Invoke(s_frameCountMethod, nullptr);
    return r ? UnboxInt32(r) : -1;
  }

  GaitSampler &sampler_;
  ActiveCharacterRuntime &active_;
  MarkerState &markers_;
  SyntheticMotion synthetic_;
  JumpInertialSource jumpInertial_;
  NativeAmplifier amplifier_;
  void *lastAnimator_ = nullptr;
  int lastRevision_ = -1;
  int lastWriteFrame_ = -1;  // frame dedup: first PreLateTick per frame writes
};
