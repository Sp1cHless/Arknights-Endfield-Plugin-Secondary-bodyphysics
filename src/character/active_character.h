#pragma once
// character/active_character.h — the single active-character runtime context
// (architecture spec §4.2).  One character is written at a time; switching
// resets everything so no stale targets leak across characters (§8.3).
#include <cstring>
#include "../common/logger.h"
#include "../config/config_loader.h"  // ConfigSnapshot (profile binding)
#include "../motion/freq_lock.h"
#include "../motion/jump_live_signal.h"
#include "../motion/locomotion_release.h"
#include "../motion/synthetic_base_filter.h"
#include "bone_resolver.h"
#include "character_identity.h"

struct SyntheticRuntime {
  float ampEnv = 0.0f;
  float downEnv = 0.0f;
  float freqEnv = 1.5f;
  double phase = 0.0;
  double lastAdvanceTime = 0.0;
  bool wasGroundLocomotionMoving = false;
  bool locomotionOnsetActive = false;
  // Phase-tracking handshake (PLL): the 20Hz sampling layer publishes the
  // stable-loop clip's phase (phaseRefRad) + validity (phaseRefValid) on
  // every accepted-locomotion sample; SyntheticMotion feeds them to the
  // envelope each frame, which pulls its continuous phase toward the ref
  // with a bounded rate (no hard snap except at negligible amplitude).
  bool phaseRefValid = false;
  double phaseRefRad = 0.0;  // tracking target = 2pi*cycles*2*norm + offset
  // Auto-frequency alignment (20Hz sampling layer): measured loop frequency
  // (physical = 2x loop, same semantics as config frequency_hz) + a
  // deviation-triggered lock that starts from the CONFIG frequency and only
  // corrects toward the measurement when |dev| stays above the per-gait
  // threshold for several samples (hysteresis).  Pure logic lives in
  // motion/freq_lock.h so verify_tests can unit-test it.
  FreqLockState freq;
  float outAngleRad = 0.0f;
  Quat lastNativeR = QuatIdentity();
  Quat lastNativeL = QuatIdentity();
  SyntheticBaseFilter baseFilter;
  Quat targetR = QuatIdentity();
  Quat targetL = QuatIdentity();
  bool targetValid = false;
  StopReleaseState stopRelease;
  // Release-tail write-through flag for the engine's synthetic write gate
  // (derived fresh by SyntheticMotion every frame; never persists).
  bool releaseTail = false;
  // Read-only trace context, refreshed at the existing 20Hz clip sample.
  DWORD clipSampleMs = 0;
  bool clipReadOk = false;
  char dominantClip[128] = {0};
  char releaseBlockedClip[128] = {0};
};

struct JumpRuntime {
  bool active = false;
  float t = -1.0f;        // seconds, -1 = not jumping
  DWORD lastTick = 0;
};

struct ReplayRuntime {
  // bones-verified timestamp gate (baseline g_bonesVerifiedT)
  DWORD bonesVerifiedMs = 0;
};

struct ActiveCharacterRuntime {
  bool valid = false;

  void *animator = nullptr;       // current main-character Animator
  void *rootTransform = nullptr;  // Animator -> transform

  char characterId[128] = {0};    // canonical chr_* id
  const CharacterProfile *profile = nullptr;  // points into ConfigSnapshot
  bool profileFound = false;

  BoneResolution bones;
  Axis axis = Axis::Z;
  float axisSign = 1.0f;

  int currentGait = -1;           // Gait enum
  bool transitionToIdle = false;
  bool jumpActive = false;
  bool jumpDetected = false;      // any jump clip active this frame

  DWORD lastIdentityRefreshMs = 0;
  DWORD lastGaitSampleMs = 0;

  SyntheticRuntime synthetic;
  JumpRuntime jump;
  JumpLiveSignal jumpSignal;
  ReplayRuntime replay;
};

// Full reset on character switch (spec §8.3): clear bones, targets, jump,
// gait, envelope — everything.
static void ResetActiveCharacter(ActiveCharacterRuntime &c) {
  c.valid = false;
  c.animator = nullptr;
  c.rootTransform = nullptr;
  c.characterId[0] = 0;
  c.profile = nullptr;
  c.profileFound = false;
  c.bones = BoneResolution();
  c.axis = Axis::Z;
  c.axisSign = 1.0f;
  c.currentGait = -1;
  c.transitionToIdle = false;
  c.jumpActive = false;
  c.lastIdentityRefreshMs = 0;
  c.lastGaitSampleMs = 0;
  c.synthetic = SyntheticRuntime();
  c.jump = JumpRuntime();
  c.jumpSignal = JumpLiveSignal();
  c.replay = ReplayRuntime();
}

// Refresh the main-character animator via the player-control chain
// (baseline SafeRefreshEntity / RefreshEntityAnimator, called ~500ms).
static void RefreshMainCharacterAnimator() {
  if (!g_playerController) return;
  __try {
    int pcOff = SafeOff(OFF_pcEntity, "pcEntity");
    if (pcOff < 0) return;  // strict: chain unavailable -> no refresh
    void *entity = *(void **)((char *)g_playerController + pcOff);
    if (entity) {
      g_mainCharEntity = entity;
      int ecOff = SafeOff(OFF_entityComplexAnim, "entityComplexAnim");
      if (ecOff < 0) return;  // strict
      void *complexAnimCom = *(void **)((char *)entity + ecOff);
      if (complexAnimCom) {
        // retry lazy resolve here: component is initialized by refresh time
        ResolveComplexAnimatorOffset(entity, complexAnimCom);
        int caOff = SafeOff(OFF_complexAnimAnimator, "complexAnimAnimator");
        if (caOff < 0) return;  // strict
        void *animator = *(void **)((char *)complexAnimCom + caOff);
        if (animator) g_cachedAnimator = animator;
      }
    }
  } __except (1) {
  }
}

// Resolve the canonical character id from the active Animator's root
// GameObject name (spec §7.1).  Returns "" when unavailable.
static void ResolveCharacterId(ActiveCharacterRuntime &c) {
  char idBuf[192] = {0};
  char goName[192] = {0};
  __try {
    void *go = SafeGetComponentGameObject(c.animator);
    if (go) {
      SafeGetObjectName(go, goName, sizeof(goName));
      if (goName[0]) {
        NormalizeCharacterId(goName, idBuf, sizeof(idBuf));
        KnownCharacterNote(goName);  // collector: raw name incl. _postmodel
        ProbeLog("[CHAR] animator=%p GO=\"%s\" id=\"%s\"\n",
                 c.animator, goName, idBuf);
      }
    }
  } __except (1) {
  }
  snprintf(c.characterId, sizeof(c.characterId), "%s", idBuf);
}
