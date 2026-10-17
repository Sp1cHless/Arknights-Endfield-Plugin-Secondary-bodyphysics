#pragma once
#include <cstring>
#include "gait_classifier.h"

static bool IsAttackClip(const char *name) {
  if (!name) return false;
  return strstr(name, "attack") || strstr(name, "atk") ||
         strstr(name, "skill") || strstr(name, "combo") ||
         strstr(name, "ult") || strstr(name, "_air_");
}

// Inspect every contributing clip: an unsupported blend must cancel a stop
// even when a lower-weight locomotion stop remains in the layer.
struct StopReleaseSample {
  bool stop = false;
  bool idle = false;
  bool moving = false;
  bool blocked = false;

  void Observe(const char *name, const GaitClassification &cls, float weight) {
    if (!(weight > 0.0f)) return;
    if (!name || IsAttackClip(name) || cls.jumpDetected) {
      blocked = true;
    } else if (IsLocomotionGait(cls.gait)) {
      if (cls.transitionToIdle) stop = true;
      else moving = true;
    } else if (cls.gait == GaitIdle ||
               strcmp(name, "idle") == 0 ||
               strcmp(name, "idle_loop") == 0 ||
               (strstr(name, "_idle_loop") &&
                !strstr(name, "_to_") && !strstr(name, "special") &&
                !strstr(name, "dash"))) {
      // Idle is continuation-only; it never arms motion from rest.
      idle = true;
    } else {
      blocked = true;
    }
  }
};

struct StopReleaseState {
  bool armed = false;
  bool blocked = false;
  bool wasStop = false;

  void Observe(const StopReleaseSample &sample) {
    blocked = sample.blocked;
    const bool enteredStop = sample.stop && !wasStop;
    const bool continuing = sample.stop || (sample.idle && !sample.moving);
    armed = !blocked && (enteredStop || (armed && continuing));
    wasStop = sample.stop;
  }
};
