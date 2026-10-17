#pragma once
// motion/gait_classifier.h — pure clip-name gait classification.
// V1 keeps the EXACT baseline classification semantics (architecture spec
// §9.3): priority jump > _to_ (by target) > start > stop > main loops.
#include <cstring>
#include <cmath>
#include "../config/config_types.h"

struct GaitClassification {
  int gait = GaitNone;          // Gait enum
  bool transitionToIdle = false;
  bool jumpDetected = false;
  bool landingDetected = false;
  bool loopStable = false;      // stable locomotion loop (usable phase ref)
};

static bool IsLocomotionGait(int gait) {
  return gait >= GaitWalk && gait <= GaitZipline;
}

// Fast onset is limited to accepted ground locomotion. Zipline keeps its
// existing attack smoothing; stop/to-idle and Jump never arm onset.
static bool IsGroundLocomotionOnsetEligible(int gait, bool transitionToIdle,
                                             bool jumpDetected) {
  return gait >= GaitWalk && gait <= GaitSprint && !transitionToIdle &&
         !jumpDetected;
}

// Per-sample phase-tracking eligibility (pure): any accepted locomotion clip
// that is not a to-idle release and not a jump clip (jump clips map to
// GaitRun but their normalizedTime is not a locomotion phase reference).
// Loop-stability (start/stop/_to_ exclusion) is tracked separately via
// GaitClassification::loopStable.
static inline bool IsPhaseAlignEligible(int gait, bool transitionToIdle,
                                        bool jumpDetected) {
  return IsLocomotionGait(gait) && !transitionToIdle && !jumpDetected;
}

// Symmetric (left-right alternating) locomotion loops produce TWO physical
// oscillations per animation loop (one per step): the oscillator runs at 2x
// the loop frequency, so the phase advances kLocomotionPhaseCyclesPerLoop
// cycles per normalizedTime unit.
static constexpr double kLocomotionPhaseCyclesPerLoop = 2.0;

static inline double PhaseFromNormalizedTime(float norm,
                                             double cyclesPerLoop) {
  return fmod(cyclesPerLoop * 2.0 * 3.14159265358979 * norm,
              2.0 * 3.14159265358979);
}

// Per-gait phase offset (degrees, [0,180]) from the character profile,
// applied on top of the clip-phase mapping at alignment time.  run/sprint
// typically carry 180 (inverted) for symmetric rigs; walk/zipline 0.
static inline float GaitPhaseOffsetDeg(const CharacterProfile &p, int gait) {
  switch (gait) {
    case GaitWalk: return p.walk.phaseOffsetDeg;
    case GaitRun: return p.run.phaseOffsetDeg;
    case GaitSprint: return p.sprint.phaseOffsetDeg;
    case GaitZipline: return p.zipline.phaseOffsetDeg;
    default: return 0.0f;
  }
}

// Per-gait GaitParam accessor for the new per-animation switches
// (phase_align / auto_frequency / freq_dev_threshold).
static inline const GaitParam &GaitParamRef(const CharacterProfile &p,
                                            int gait) {
  switch (gait) {
    case GaitWalk: return p.walk;
    case GaitRun: return p.run;
    case GaitSprint: return p.sprint;
    case GaitZipline: return p.zipline;
    default: return p.idle;
  }
}

static inline bool GaitPhaseAlignEnabled(const CharacterProfile &p,
                                         int gait) {
  return GaitParamRef(p, gait).phaseAlign;
}

static inline bool GaitAutoFrequencyEnabled(const CharacterProfile &p,
                                            int gait) {
  return GaitParamRef(p, gait).autoFrequency;
}

static inline float GaitFreqDevThreshold(const CharacterProfile &p,
                                         int gait) {
  return GaitParamRef(p, gait).freqDevThreshold;
}

static inline double ApplyGaitPhaseOffset(double phaseRad, float offsetDeg) {
  return fmod(phaseRad + offsetDeg * 0.01745329251994329577,
              2.0 * 3.14159265358979);
}

static bool CanWriteSynthetic(int gait, bool jumpDetected,
                              bool jumpEnabled) {
  if (!IsLocomotionGait(gait)) return false;
  if (jumpDetected && !jumpEnabled) return false;
  return true;
}

// Global locomotion whitelist.  Unknown clips fail closed (GaitNone); there
// is deliberately no permanent blacklist for "special", "dash", etc. so
// character-specific animation rules can opt those clips in later.
static int ClassifyGlobalLocomotionClip(const char *name) {
  if (!name) return GaitNone;
  // Default policy: special/dash clips are not generic locomotion. They are
  // still opt-in capable because character rules run before this function.
  if (strstr(name, "special") || strstr(name, "dash")) return GaitNone;
  if (strstr(name, "zipline")) return GaitZipline;
  if (strstr(name, "jump")) return GaitRun;  // existing Jump gate semantics

  if (strstr(name, "_to_")) {
    const char *target = strstr(name, "_to_") + 4;
    if (strstr(target, "sprint")) return GaitSprint;
    if (strstr(target, "run")) return GaitRun;
    if (strstr(target, "walk") || strstr(target, "move")) return GaitWalk;
    if (strstr(target, "idle")) {
      // Only a locomotion -> idle transition is eligible for fast release.
      if (strstr(name, "sprint") || strstr(name, "run") ||
          strstr(name, "walk") || strstr(name, "move"))
        return GaitWalk;
    }
    return GaitNone;
  }

  if (strstr(name, "start")) {
    if (strstr(name, "sprint")) return GaitSprint;
    if (strstr(name, "run")) return GaitRun;
    if (strstr(name, "walk") || strstr(name, "move")) return GaitWalk;
    return GaitNone;
  }

  if (strstr(name, "stop")) {
    if (strstr(name, "sprint") || strstr(name, "run") ||
        strstr(name, "walk") || strstr(name, "move"))
      return GaitWalk;
    return GaitNone;
  }

  if (strstr(name, "sprint")) return GaitSprint;
  if (strstr(name, "run")) return GaitRun;
  if (strstr(name, "walk") || strstr(name, "move")) return GaitWalk;
  return GaitNone;
}

static int ClassifyClipName(const char *name,
                            const CharacterProfile *profile = nullptr) {
  if (!name) return GaitNone;
  if (profile) {
    const AnimationRule *bestRule = nullptr;
    size_t bestLength = 0;
    for (const AnimationRule &rule : profile->animationRules) {
      if (!rule.contains.empty() && rule.contains.size() > bestLength &&
          strstr(name, rule.contains.c_str())) {
        bestRule = &rule;
        bestLength = rule.contains.size();
      }
    }
    if (bestRule) return bestRule->gait;
  }
  return ClassifyGlobalLocomotionClip(name);
}

static GaitClassification ClassifyClipNameFull(
    const char *name, const CharacterProfile *profile = nullptr) {
  GaitClassification out;
  out.gait = ClassifyClipName(name, profile);
  if (!name) return out;
  // Fast release is only meaningful for an accepted locomotion transition.
  if (out.gait != GaitNone &&
      (strstr(name, "stop") || strstr(name, "_to_idle")))
    out.transitionToIdle = true;
  if (out.gait != GaitNone && strstr(name, "jump")) {
    out.jumpDetected = true;
    // landing-only simplified jump (baseline: only the LAND clip drives it)
    if (strstr(name, "land")) out.landingDetected = true;
  }
  // A stable locomotion loop (usable phase reference) is any accepted
  // locomotion clip that is not a start/stop/_to_ transition, not a jump,
  // and not idle.  Zipline traversal (interact_zipline_sp_02) counts;
  // zipline start/stop do not.  Character-rule opt-in clips follow the same
  // name heuristic.
  if (out.gait >= GaitWalk && out.gait <= GaitZipline &&
      !strstr(name, "start") && !strstr(name, "stop") &&
      !strstr(name, "_to_") && !strstr(name, "jump"))
    out.loopStable = true;
  return out;
}
