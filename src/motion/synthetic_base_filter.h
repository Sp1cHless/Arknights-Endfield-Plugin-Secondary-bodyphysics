#pragma once
// motion/synthetic_base_filter.h — synthetic-only stable composition base.
// The live native pose is low-pass filtered before the independent oscillator
// is composed, preventing near-frequency native twist from beating against it.
//
// PATCH 2026-10 (self-feedback guard):
//   The "current native" rotation is read back from the very bone this filter
//   writes to.  When the animation does not refresh that bone between two
//   writes (observed after a main-character switch), the value read is our own
//   previous output, the low-pass base follows it, and the composed target
//   accumulates frame over frame — visible as the chest rotating away and only
//   springing back once the gait goes idle (baseFilter.Reset()).
//   Fix: if the freshly read rotation is numerically the value we wrote last
//   frame, treat it as read-after-write feedback and freeze the base instead of
//   following it.  Normal frames are unaffected.
#include <cmath>
#include "../common/quat.h"
#include "../common/time_utils.h"

static constexpr float kSyntheticBaseFollowTauSec = 0.30f;

// Dot-product threshold for "this is exactly what we wrote last frame"
// (1.0 == identical quaternion; this value is ~1.15 degrees of rotation).
static constexpr float kSelfFeedbackDotEps = 0.99995f;

struct SyntheticBaseFilter {
  Quat baseR = QuatIdentity();
  Quat baseL = QuatIdentity();
  Quat lastOutR = QuatIdentity();
  Quat lastOutL = QuatIdentity();
  bool hasLastOut = false;
  bool initialized = false;
  FrameGate gate;

  void Reset() {
    baseR = QuatIdentity();
    baseL = QuatIdentity();
    lastOutR = QuatIdentity();
    lastOutL = QuatIdentity();
    hasLastOut = false;
    initialized = false;
    gate = FrameGate();
  }

  void Compose(Quat currentR, Quat currentL, int axis, float angleRad,
               float dt, Quat &outR, Quat &outL) {
    // Read-after-write detection (see file header).
    const bool selfFeedback =
        hasLastOut &&
        fabsf(QuatDot(currentR, lastOutR)) > kSelfFeedbackDotEps &&
        fabsf(QuatDot(currentL, lastOutL)) > kSelfFeedbackDotEps;

    if (!initialized) {
      baseR = currentR;
      baseL = currentL;
      initialized = true;
    } else if (dt > 0.0f && !selfFeedback) {
      if (dt > 0.05f) dt = 0.05f;
      float follow = 1.0f - expf(-dt / kSyntheticBaseFollowTauSec);
      baseR = QuatSlerp(baseR, currentR, follow);
      baseL = QuatSlerp(baseL, currentL, follow);
    }

    Quat dq = QuatAxisAngle(axis, angleRad);
    outR = QuatMul(baseR, dq);
    outL = QuatMul(baseL, dq);

    lastOutR = outR;
    lastOutL = outL;
    hasLastOut = true;
  }

  void ComposeLive(Quat currentR, Quat currentL, int axis, float angleRad,
                   Quat &outR, Quat &outL) {
    Compose(currentR, currentL, axis, angleRad, (float)gate.Tick(),
            outR, outL);
  }

  void ComposeDirect(Quat currentR, Quat currentL, int axis, float angleRad,
                     Quat &outR, Quat &outL) {
    Reset();
    Quat dq = QuatAxisAngle(axis, angleRad);
    outR = QuatMul(currentR, dq);
    outL = QuatMul(currentL, dq);
  }
};
