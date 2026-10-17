#pragma once
// motion/freq_lock.h — auto-frequency alignment (pure logic, no Unity/IL2CPP
// deps so verify_tests.cpp can unit-test it directly).
//
// Semantics (user task): start from the CONFIG frequency; correct toward the
// measured animation-loop frequency only when the deviation stays above the
// per-gait threshold for several consecutive 20Hz samples.  Hysteresis:
// correction triggers at `threshold`, freezes below threshold/2.
// Physical frequency = kLocomotionPhaseCyclesPerLoop x loop frequency (same
// semantics as config frequency_hz).
#include <cmath>
#include <cstring>
#include "../config/config_types.h"

static constexpr float kFreqEmaAlpha = 0.3f;         // measurement EMA
static constexpr float kFreqCorrectionStep = 0.2f;   // per-sample pull toward meas
static constexpr int   kFreqDevSamples = 5;          // samples before correction (~0.25s)
static constexpr float kFreqMeasMaxDeltaNorm = 0.5f; // reject > half-loop per 50ms
static constexpr float kFreqMeasMinDeltaNorm = 1e-6f; // reject stalled/roundoff deltas
static constexpr float kFreqMeasMaxGapSec = 0.25f;  // do not fit across stalls
static constexpr double kFreqLockCyclesPerLoop = 2.0;  // 1:2 loop->oscillator mapping
static constexpr int   kFreqCacheMax = 32;           // fitted-frequency cache slots

// Session-level fitted-frequency cache, keyed by (characterId, gait).
// A completed correction persists its final frequency here so character or
// gait switches REUSE the learned value instead of restarting from config.
// Fixed-size array -> zero allocation (safe on the 20Hz callback path).
struct FreqCacheEntry {
  char charId[128] = {0};
  int gait = GaitNone;
  float hz = 0.0f;
  float cfgHz = 0.0f;  // config used to learn this fit; survives runtime reset
};

struct FreqCache {
  FreqCacheEntry entries[kFreqCacheMax];
  int count = 0;
  int nextSlot = 0;  // ring cursor for replacement when full

  float *Find(const char *charId, int gait) {
    if (!charId || !charId[0]) return nullptr;
    for (int i = 0; i < count; i++)
      if (entries[i].gait == gait &&
          strcmp(entries[i].charId, charId) == 0)
        return &entries[i].hz;
    return nullptr;
  }

  float *FindForConfig(const char *charId, int gait, float cfgHz) {
    if (!charId || !charId[0]) return nullptr;
    for (int i = 0; i < count; i++) {
      if (entries[i].gait == gait && strcmp(entries[i].charId, charId) == 0) {
        if (entries[i].cfgHz == cfgHz) return &entries[i].hz;
        Invalidate(charId, gait);
        return nullptr;
      }
    }
    return nullptr;
  }

  void Put(const char *charId, int gait, float hz, float cfgHz = 0.0f) {
    if (!charId || !charId[0] || hz <= 0.0f) return;
    for (int i = 0; i < count; i++) {
      if (entries[i].gait == gait && strcmp(entries[i].charId, charId) == 0) {
        entries[i].hz = hz;
        entries[i].cfgHz = cfgHz;
        return;
      }
    }
    int slot = count < kFreqCacheMax ? count++ : nextSlot;
    if (count == kFreqCacheMax) nextSlot = (nextSlot + 1) % kFreqCacheMax;
    FreqCacheEntry &en = entries[slot];
    snprintf(en.charId, sizeof(en.charId), "%s", charId);
    en.gait = gait;
    en.hz = hz;
    en.cfgHz = cfgHz;
  }

  // Drop one (character, gait) entry (e.g. the user manually changed the
  // config frequency -> their input is authoritative, re-learn from it).
  void Invalidate(const char *charId, int gait) {
    for (int i = 0; i < count; i++) {
      if (entries[i].gait == gait &&
          strcmp(entries[i].charId, charId) == 0) {
        for (int j = i; j < count - 1; j++) entries[j] = entries[j + 1];
        count--;
        return;
      }
    }
  }
};

struct FreqLockState {
  float useHz = 0.0f;       // current oscillator frequency (cfg or corrected)
  float cfgHz = 0.0f;       // config frequency of the last sample (change -> reset)
  float measHz = 0.0f;      // EMA-smoothed measured physical frequency
  bool measValid = false;   // measurement published
  bool correcting = false;  // currently pulling useHz toward measHz
  int devCount = 0;         // consecutive over-threshold samples
  int lastGait = GaitNone;  // gait of the last sample (cfg-change detection
                            // is per-gait: a gait switch changes cfgHz too,
                            // but that is NOT a user config edit)
  // measurement scratch (20Hz, reset on clip switch)
  float lastNorm = -1.0f;
  DWORD lastNormMs = 0;
  bool havePrev = false;
  char lastClip[128] = {0};
};

static inline void FreqLockPause(FreqLockState &f) {
  f.havePrev = false;
  f.lastNorm = -1.0f;
  f.lastNormMs = 0;
  f.lastClip[0] = 0;
  f.measValid = false;
  f.correcting = false;
  f.devCount = 0;
}

// Select the current gait before start/transition clips, without learning
// their non-loop timing. Measurement and cache reuse are separate decisions.
static inline void FreqLockPrepare(FreqLockState &f, const GaitParam &g,
                                   int gait, FreqCache &cache,
                                   const char *charId) {
  if (f.lastGait == gait && f.cfgHz == g.frequencyHz) return;
  f = FreqLockState();
  f.lastGait = gait;
  f.cfgHz = g.frequencyHz;
  const float *cached = cache.FindForConfig(charId, gait, g.frequencyHz);
  f.useHz = g.autoFrequency && cached ? *cached : g.frequencyHz;
}

static inline float FreqLockTargetHz(const FreqLockState &f,
                                     const GaitParam &g, int gait) {
  return g.autoFrequency && f.lastGait == gait && f.cfgHz == g.frequencyHz &&
                 std::isfinite(f.useHz) && f.useHz > 0.0f
             ? f.useHz : g.frequencyHz;
}

// Cadence may respond quickly to a newly accepted gait without changing
// what the frequency fitter learns, or speeding up subsequent same-gait
// adjustments. Pauses/Jump/stops rearm this for the next locomotion entry.
struct FreqTransitionState {
  int gait = GaitNone;
  bool active = false;
  float toleranceHz = 0.0f;
};

static constexpr float kGaitFrequencyTransitionTauSec = 0.10f;

static inline float SelectGaitFrequencyTau(
    FreqTransitionState &state, int gait, bool moving, float currentHz,
    float targetHz, float configuredTauSec) {
  if (!moving) {
    state = FreqTransitionState();
    return configuredTauSec;
  }
  if (state.gait != gait) {
    state.gait = gait;
    state.active = true;
    state.toleranceHz = fmaxf(0.005f, fabsf(targetHz - currentHz) * 0.05f);
  }
  if (state.active && fabsf(targetHz - currentHz) <= state.toleranceHz)
    state.active = false;
  return state.active ? fminf(configuredTauSec, kGaitFrequencyTransitionTauSec)
                      : configuredTauSec;
}

// One 20Hz sample.  `g` is the per-gait GaitParam of the current clip;
// `clipName` change restarts measurement; `cache` holds the learned
// (characterId, gait) frequencies so switches reuse them instead of falling
// back to the config value. Returns whether this sample can publish a phase
// reference too; an invalid timing interval must disable both consumers.
static inline bool FreqLockTick(FreqLockState &f, const GaitParam &g,
                                const char *clipName, float norm, DWORD now,
                                int gait, FreqCache &cache,
                                const char *charId) {
  const float cfgHz = g.frequencyHz;
  FreqLockPrepare(f, g, gait, cache, charId);
  if (!std::isfinite(norm) || norm < 0.0f) {
    FreqLockPause(f);
    return false;
  }

  // A different clip or a paused measurement needs a fresh first sample.
  // The cache validates its own config identity, including after hot reload.
  if (strcmp(f.lastClip, clipName) != 0) {
    snprintf(f.lastClip, sizeof(f.lastClip), "%s", clipName);
    f.havePrev = false;
    f.lastNorm = -1.0f;
    f.lastNormMs = 0;
    const float *cached = cache.FindForConfig(charId, gait, cfgHz);
    f.useHz = g.autoFrequency && cached ? *cached : cfgHz;
    f.measValid = false;
    f.correcting = false;
    f.devCount = 0;
    return true;  // finite phase is usable; no frequency delta yet
  }

  // measurement: loop frequency from the normalizedTime delta (already
  // includes state-speed multipliers that Animator.speed misses)
  if (f.havePrev && f.lastNormMs) {
    float dNorm = norm - f.lastNorm;
    if (dNorm < 0.0f) dNorm += 1.0f;  // wrapped a full loop
    float dt = (float)(now - f.lastNormMs) / 1000.0f;
    if (dt <= 0.01f || dt > kFreqMeasMaxGapSec ||
        dNorm <= kFreqMeasMinDeltaNorm || dNorm >= kFreqMeasMaxDeltaNorm) {
      // A previous good measurement does not make this sample usable.
      // Break the consecutive-deviation streak and never advance a fit
      // using stale EMA during a frozen/restarted/undersampled clock.
      f.measValid = false;
      f.correcting = false;
      f.devCount = 0;
      // Retain this clip and rebase to the current finite clock sample.
      // A persistent stall must remain invalid on every following sample,
      // rather than repeatedly masquerading as a fresh clip engagement.
      f.havePrev = true;
      f.lastNorm = norm;
      f.lastNormMs = now;
      return false;
    }
    {
      const float fMeas =
          (float)(kFreqLockCyclesPerLoop * (dNorm / dt));
      if (!f.measValid) {
        f.measHz = fMeas;
        f.measValid = true;
      } else {
        f.measHz += kFreqEmaAlpha * (fMeas - f.measHz);
      }
    }
  }
  f.havePrev = true;
  f.lastNorm = norm;
  f.lastNormMs = now;

  // deviation-triggered correction (hysteresis: trigger at the threshold,
  // keep correcting through the band until the deviation drops below
  // threshold/2, then freeze)
  if (!g.autoFrequency || !f.measValid || f.measHz < 0.1f) return true;
  const float th = g.freqDevThreshold;
  const float dev = fabsf(f.useHz - f.measHz) / f.measHz;
  if (dev > th) {
    f.devCount++;
    if (f.devCount >= kFreqDevSamples && !f.correcting) f.correcting = true;
  } else {
    f.devCount = 0;
  }
  // The correction step is gated by the correcting flag alone (not by
  // dev > th): once engaged it converges through the hysteresis band down to
  // threshold/2 so the final frequency MATCHES the measurement instead of
  // freezing at the trigger edge (4.7% for a 5% threshold).
  if (f.correcting) {
    f.useHz += (f.measHz - f.useHz) * kFreqCorrectionStep;
    if (fabsf(f.useHz - f.measHz) / f.measHz < th * 0.5f) {
      f.correcting = false;
      cache.Put(charId, gait, f.useHz, cfgHz);  // persist fit and config identity
    }
  }
  return true;
}
