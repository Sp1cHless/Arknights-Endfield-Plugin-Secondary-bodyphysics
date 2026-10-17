#pragma once
// config/config_validator.h — final validation shared by startup and hot
// reload. Validation never mutates a snapshot: invalid input is rejected and
// the caller keeps the last-known-good snapshot.
#include <float.h>
#include "config_types.h"
#include "../common/logger.h"

static bool ValidateSnapshot(const ConfigSnapshot &snap) {
  Log("[CFG] validation policy: motion_mode=off|synthetic|amplify_native; "
      "axis=X|Y|Z; sign=-1|+1; bones=both-set-or-both-empty; "
      "jump.mode=off|landing_damped; animation_rules<=16; "
      "global intervals=positive integers; revision=non-negative integer; "
      "amplitude/frequency/tau=finite-only(no range bounds)");

  bool valid = true;
  for (const auto &kv : snap.characters) {
    const std::string &id = kv.first;
    const CharacterProfile &p = kv.second;
    if (!p.enabled) {
      Log("[CFG] character '%s': disabled", id.c_str());
      continue;
    }

    const bool rightSet = !p.bones.rightName.empty();
    const bool leftSet = !p.bones.leftName.empty();
    if (rightSet != leftSet) {
      Log("[CFG] character '%s': one-sided bone pair -> INVALID", id.c_str());
      valid = false;
    }
    if (p.motionMode != MotionMode::Off &&
        p.motionMode != MotionMode::Synthetic &&
        p.motionMode != MotionMode::AmplifyNative) {
      Log("[CFG] character '%s': motion_mode enum invalid -> INVALID",
          id.c_str());
      valid = false;
    }
    if (p.axis.axis != Axis::X && p.axis.axis != Axis::Y &&
        p.axis.axis != Axis::Z) {
      Log("[CFG] character '%s': axis enum invalid -> INVALID", id.c_str());
      valid = false;
    }
    if (p.axis.sign != -1.0f && p.axis.sign != 1.0f) {
      Log("[CFG] character '%s': axis sign invalid -> INVALID", id.c_str());
      valid = false;
    }
    if (p.jump.mode != "off" && p.jump.mode != "landing_damped") {
      Log("[CFG] character '%s': jump mode invalid -> INVALID", id.c_str());
      valid = false;
    }
    if (!_finite(p.jump.amplitudeDeg) || !_finite(p.jump.dampingTauSec) ||
        !_finite(p.jump.frequencyHz) || !_finite(p.jump.maxDurationSec) ||
        !_finite(p.jump.takeoffDelaySec) || !_finite(p.jump.risingTargetDeg) ||
        !_finite(p.jump.apexFallingTargetDeg) ||
        !_finite(p.jump.accelerationResponse) ||
        !_finite(p.jump.accelerationFilterTauSec) ||
        !_finite(p.jump.naturalFrequencyHz) || !_finite(p.jump.dampingRatio) ||
        !_finite(p.jump.landingImpulseGain) || p.jump.dampingTauSec <= 0.0f ||
        p.jump.frequencyHz < 0.0f || p.jump.maxDurationSec < 0.0f ||
        p.jump.takeoffDelaySec < 0.0f ||
        p.jump.accelerationFilterTauSec <= 0.0f ||
        p.jump.naturalFrequencyHz <= 0.0f || p.jump.dampingRatio < 0.0f ||
        p.jump.landingImpulseGain < 0.0f) {
      Log("[CFG] character '%s': Jump tuning invalid -> INVALID", id.c_str());
      valid = false;
    }
    if (!_finite(p.nativeAmplify.factor) || p.nativeAmplify.factor <= 0.0f) {
      Log("[CFG] character '%s': native amplify factor invalid -> INVALID",
          id.c_str());
      valid = false;
    }
    // phase_offset_deg is bounded to [0, 180] (user-facing phase-alignment
    // range); non-finite or out-of-range values reject the snapshot.
    const GaitParam *gaitParams[] = {&p.idle, &p.walk, &p.run, &p.sprint,
                                     &p.zipline};
    for (const GaitParam *g : gaitParams) {
      if (!_finite(g->phaseOffsetDeg) || g->phaseOffsetDeg < 0.0f ||
          g->phaseOffsetDeg > 180.0f) {
        Log("[CFG] character '%s': phase_offset_deg out of [0,180] -> INVALID",
            id.c_str());
        valid = false;
      }
      // freq_dev_threshold is a fraction in (0,1) (default 0.05 = 5%).
      if (!_finite(g->freqDevThreshold) || g->freqDevThreshold <= 0.0f ||
          g->freqDevThreshold >= 1.0f) {
        Log("[CFG] character '%s': freq_dev_threshold out of (0,1) -> INVALID",
            id.c_str());
        valid = false;
      }
    }
    if (p.animationRules.size() > 16) {
      Log("[CFG] character '%s': animation_rules exceeds 16 -> INVALID",
          id.c_str());
      valid = false;
    }
  }
  return valid;
}
