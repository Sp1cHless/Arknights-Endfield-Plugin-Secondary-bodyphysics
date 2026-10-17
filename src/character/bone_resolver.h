#pragma once
// character/bone_resolver.h — breast bone discovery from the Animator root
// and per-bone-family axis selection (baseline candidate table +
// SetBreastAxisByName semantics).  Architecture spec §8.
//
// PATCH 2026-10 (breast_base family):
//   1) The candidate table had no entry for the "breast_base_*_a_01_jnt"
//      family (used by e.g. chr_0034_typhoea).  Because the family was also
//      missing from ApplyBoneFamilyAxis(), any character relying on pattern
//      matching for this family would silently get axis Z (lateral sway)
//      instead of Y.  The family is now listed in both places.
//   2) The explicit (profile) path now logs the axis it applies, matching the
//      pattern path, so a missing/empty axis in the character database becomes
//      visible in the probe log.
#include <cstring>
#include "../common/logger.h"
#include "../common/safe_unity.h"
#include "character_identity.h"

// Candidate table in priority order (baseline verified list).
static const char *kBreastCandidates[][2] = {
    {"breast_R_01_jnt", "breast_L_01_jnt"},
    {"R_breast_01_jnt", "L_breast_01_jnt"},
    {"xiong_R_0_skin_jnt", "xiong_L_0_skin_jnt"},
    {"breast_R_01", "breast_L_01"},
    {"R_breast_01", "L_breast_01"},
    {"xiong_R_0_skin", "xiong_L_0_skin"},
    // PATCH: family seen on chr_0034_typhoea (breast_base_*_a_01_jnt).
    {"breast_base_R_a_01_jnt", "breast_base_L_a_01_jnt"},
};
static const int kBreastCandidateCount =
    (int)(sizeof(kBreastCandidates) / sizeof(kBreastCandidates[0]));

struct BoneResolution {
  void *breastR = nullptr;
  void *breastL = nullptr;
  char rightName[128] = {0};
  char leftName[128] = {0};
  Axis axis = Axis::Z;      // per-family (baseline SetBreastAxisByName)
  bool found = false;
};

// Axis by bone family:
//   xiong_*       -> Y front-back sway
//   breast_base_* -> Y front-back sway (PATCH; verified on chr_0034_typhoea:
//                    Z produces lateral sway, Y is the natural up/down motion)
//   others        -> Z
static void ApplyBoneFamilyAxis(BoneResolution &res, const char *rightName) {
  if (!rightName) {
    res.axis = Axis::Z;
    return;
  }
  if (strstr(rightName, "xiong")) {
    res.axis = Axis::Y;
    return;
  }
  if (strstr(rightName, "breast_base")) {
    res.axis = Axis::Y;
    return;
  }
  res.axis = Axis::Z;
}

// Search the whole candidate table under root.  Returns false when no pair
// is found (caller decides fail-closed behavior).
static bool ResolveBreastBones(void *rootTransform, BoneResolution &out,
                               bool logFound) {
  if (!rootTransform) return false;
  for (int i = 0; i < kBreastCandidateCount; i++) {
    void *r = SafeFindChildRecursive(rootTransform, kBreastCandidates[i][0], 40);
    void *l = SafeFindChildRecursive(rootTransform, kBreastCandidates[i][1], 40);
    if (r && l) {
      out.breastR = r;
      out.breastL = l;
      SafeGetObjectName(r, out.rightName, sizeof(out.rightName));
      SafeGetObjectName(l, out.leftName, sizeof(out.leftName));
      ApplyBoneFamilyAxis(out, out.rightName);
      out.found = true;
      if (logFound)
        ProbeLog("[BONE] FOUND pattern[%d] R=\"%s\" L=\"%s\" axis=%d\n",
                 i, out.rightName, out.leftName, (int)out.axis);
      return true;
    }
  }
  out.found = false;
  return false;
}

// Resolve using explicit profile bone names first, then candidates.
// (Architecture spec §8.1 order.)
static bool ResolveBreastBonesWithProfile(void *rootTransform,
                                          const CharacterProfile &profile,
                                          BoneResolution &out) {
  if (rootTransform && !profile.bones.rightName.empty() &&
      !profile.bones.leftName.empty()) {
    void *r = SafeFindChildRecursive(rootTransform, profile.bones.rightName.c_str(), 40);
    void *l = SafeFindChildRecursive(rootTransform, profile.bones.leftName.c_str(), 40);
    if (r && l) {
      out.breastR = r;
      out.breastL = l;
      SafeGetObjectName(r, out.rightName, sizeof(out.rightName));
      SafeGetObjectName(l, out.leftName, sizeof(out.leftName));
      out.axis = profile.axis.axis;
      out.found = true;
      // PATCH: log the applied axis here too (was missing, which made an
      // unverified/empty axis in the character database invisible).
      ProbeLog("[BONE] FOUND explicit R=\"%s\" L=\"%s\" axis=%d\n",
               out.rightName, out.leftName, (int)out.axis);
      return true;
    }
  }
  if (!profile.bones.allowFallbackCandidates) return false;
  return ResolveBreastBones(rootTransform, out, true);
}
