#pragma once
// config/config_types.h — immutable config snapshots. Startup and hot reload
// both build a fresh snapshot; runtime readers only observe atomic swaps.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

enum class MotionMode {
  Off = 0,
  Synthetic = 1,
  AmplifyNative = 2
};

enum class Axis {
  X = 0,
  Y = 1,
  Z = 2
};

// Gait enum: -1 none, 0 idle, 1 walk, 2 run, 3 sprint, 4 zipline.
enum Gait {
  GaitNone = -1,
  GaitIdle = 0,
  GaitWalk = 1,
  GaitRun = 2,
  GaitSprint = 3,
  GaitZipline = 4
};

struct GaitParam {
  float amplitudeDeg = 0.0f;      // up (+direction) amplitude
  float amplitudeDownDeg = 0.0f;  // down (-direction); 0 = symmetric (=up)
  float frequencyHz = 1.0f;
  float phaseOffsetDeg = 0.0f;    // phase alignment offset, [0, 180]
  bool phaseAlign = true;         // PLL phase tracking switch (default on)
  bool autoFrequency = true;      // auto frequency alignment switch (default on)
  float freqDevThreshold = 0.05f; // dev threshold (fraction, 5%) that
                                  // triggers correction toward measured freq
};

struct BoneConfig {
  std::string rightName;   // explicit override; empty = use candidates
  std::string leftName;
  bool allowFallbackCandidates = true;
};

struct AxisConfig {
  Axis axis = Axis::Z;
  float sign = 1.0f;       // +1 or -1
};

struct EnvelopeConfig {
  float amplitudeAttackTauSec = 0.15f;
  float frequencyTauSec = 0.20f;
  float toIdleReleaseTauSec = 0.015f;
};

struct JumpConfig {
  bool enabled = false;
  std::string mode = "off";      // off | landing_damped
  // Phase-E visual tuning. These defaults are the user-validated Liino
  // source-space range; final visible deformation is character dependent.
  float amplitudeDeg = 23.333f;  // landing shake amplitude
  float dampingTauSec = 0.35f;   // landing shake exponential decay tau
  float frequencyHz = 3.0f;      // landing shake carrier frequency
  float maxDurationSec = 1.20f;  // landing shake duration
  float takeoffDelaySec = 0.08f;
  float risingTargetDeg = -23.333f;
  float apexFallingTargetDeg = 23.333f;
  float accelerationResponse = 0.1667f;
  float accelerationFilterTauSec = 0.08f;
  float naturalFrequencyHz = 2.2f;
  float dampingRatio = 0.52f;
  float landingImpulseGain = 6.667f;
};

struct NativeAmplifyConfig {
  float factor = 2.0f;           // K exponent (baseline g_ampK)
};

// Optional per-character locomotion whitelist extension. `contains` is a
// clip-name fragment loaded once into the immutable config snapshot; runtime
// classification only performs an in-memory comparison at the existing 20Hz
// gait sample. Unknown clips remain native unless a character opts them in.
struct AnimationRule {
  std::string contains;
  int gait = GaitNone;
};

// axisExplicit=true  -> use AxisConfig as-is (preset overrides family)
// axisExplicit=false -> auto: axis/sign come from the bone-family default
//                       discovered at runtime (girl/lady->Z, xiong->Y)
struct CharacterProfile {
  bool enabled = false;
  MotionMode motionMode = MotionMode::Off;

  BoneConfig bones;
  AxisConfig axis;
  bool axisExplicit = false;   // set by ConfigLoader when "axis" key present

  GaitParam idle;    // amplitudeDeg 0.0  / 1.2 Hz
  GaitParam walk;    // 3.6 / 1.5
  GaitParam run;     // 8.5 / 1.7
  GaitParam sprint;  // 12.0 / 2.0
  GaitParam zipline{8.5f, 0.0f, 1.7f};  // zipline slide (interact_zipline_*)
                                         // defaults = run-family values

  EnvelopeConfig envelope;
  JumpConfig jump;
  NativeAmplifyConfig nativeAmplify;
  std::vector<AnimationRule> animationRules;  // DB-owned special movement clips
};

struct GlobalConfig {
  uint32_t gaitSampleIntervalMs = 50;    // 20 Hz
  uint32_t entityRefreshIntervalMs = 500;
  uint32_t replayVerifyWindowMs = 150;
  // Marker compatibility: V2 default OFF (config-driven).  When true, the
  // spring_test.txt / amplify_test.txt markers gate writes (V1 behavior).
  bool legacyMarkerMode = false;
};

struct ConfigSnapshot {
  std::string presetName;
  uint32_t schemaVersion = 1;
  int revision = 0;  // runtime/config.json revision (hot-apply ACK)
  // Global plugin switch: config.json "enabled"=false -> NO WRITE for any
  // character (clean native game), config still loads for instant re-enable.
  bool pluginEnabled = true;

  GlobalConfig global;
  std::unordered_map<std::string, CharacterProfile> characters;

  uint64_t contentHash = 0;
  bool valid = false;
};
