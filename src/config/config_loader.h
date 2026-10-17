#pragma once
// config/config_loader.h — V2 config loading: merge of
//   data/characters.default.json   (technical facts: bones/axis/scale/defaults)
//   presets/<active>.json          (user overrides: enable/mode/params)
//   runtime/config.json            (revision + active preset + global)
// into an immutable ConfigSnapshot.  Supports HOT RELOAD (Phase 1): the
// service thread polls runtime/config.json, rebuilds the snapshot on
// revision change and swaps the atomic pointer.  Old snapshots are retired
// two generations later (no hook callback can still be using them).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <float.h>
#include "config_types.h"
#include "config_validator.h"
#include "json_mini.h"
#include "../common/logger.h"
#include "../runtime/runtime_paths.h"

static const char *kDefaultPreset = "Default";

// Atomic config pointer consumed by the motion core every callback.
// Defined once (single-TU plugin); swap under the service thread.
static std::atomic<const ConfigSnapshot *> g_configPtr{nullptr};
static const ConfigSnapshot *g_retired[2] = {nullptr, nullptr};
static char g_configLastError[256] = {0};

static const char *ConfigLastError() { return g_configLastError; }

static void ConfigSetLastError(const char *message) {
  snprintf(g_configLastError, sizeof(g_configLastError), "%s",
           message ? message : "config invalid");
}

static void ConfigClearLastError() { g_configLastError[0] = 0; }

static inline const ConfigSnapshot *ConfigAcquire() {
  return g_configPtr.load(std::memory_order_acquire);
}

// Retire the previous snapshot (call after a successful swap, from the
// service thread only).  Two-generation delay keeps in-flight callbacks
// safe without locks.
static void ConfigRetireOld(const ConfigSnapshot *oldSnap) {
  if (g_retired[0]) delete g_retired[0];
  g_retired[0] = g_retired[1];
  g_retired[1] = oldSnap;
}

// ---- helpers ----
static bool ParseMotionModeStrict(const std::string &s, MotionMode &out) {
  if (s == "off") {
    out = MotionMode::Off;
    return true;
  }
  if (s == "synthetic") {
    out = MotionMode::Synthetic;
    return true;
  }
  if (s == "amplify_native") {
    out = MotionMode::AmplifyNative;
    return true;
  }
  return false;
}

static bool ParseAxisStrict(const std::string &s, Axis &out) {
  if (s == "X") {
    out = Axis::X;
    return true;
  }
  if (s == "Y") {
    out = Axis::Y;
    return true;
  }
  if (s == "Z") {
    out = Axis::Z;
    return true;
  }
  return false;
}

static bool IsValidAxisSign(double sign) {
  return sign == 1.0 || sign == -1.0;
}

static bool HasCompleteBonePair(const std::string &right,
                                const std::string &left) {
  return right.empty() == left.empty();
}

static bool IsFinite(double d) { return _finite(d) != 0; }

static bool HasTypeIfPresent(const jsonmini::Value &object, const char *key,
                             jsonmini::Value::Type type) {
  const jsonmini::Value *value = object.Find(key);
  return !value || value->type == type;
}

static bool ParseGaitParam(const jsonmini::Value *v, GaitParam &out,
                           float defAmp, float defFreq) {
  if (!v) {
    out.amplitudeDeg = defAmp;
    out.amplitudeDownDeg = 0.0f;  // 0 = symmetric (= up)
    out.frequencyHz = defFreq;
    return true;
  }
  if (v->type != jsonmini::Value::Object ||
      !HasTypeIfPresent(*v, "amplitude_deg", jsonmini::Value::Number) ||
      !HasTypeIfPresent(*v, "amplitude_down_deg", jsonmini::Value::Number) ||
      !HasTypeIfPresent(*v, "frequency_hz", jsonmini::Value::Number) ||
      !HasTypeIfPresent(*v, "phase_offset_deg", jsonmini::Value::Number) ||
      !HasTypeIfPresent(*v, "phase_align", jsonmini::Value::Bool) ||
      !HasTypeIfPresent(*v, "auto_frequency", jsonmini::Value::Bool) ||
      !HasTypeIfPresent(*v, "freq_dev_threshold", jsonmini::Value::Number))
    return false;
  out.amplitudeDeg = (float)v->GetNumber("amplitude_deg", defAmp);
  // down amplitude: explicit only; 0 / absent = symmetric
  out.amplitudeDownDeg = (float)v->GetNumber("amplitude_down_deg", 0.0);
  out.frequencyHz = (float)v->GetNumber("frequency_hz", defFreq);
  out.phaseOffsetDeg = (float)v->GetNumber("phase_offset_deg", 0.0);
  out.phaseAlign = v->GetBool("phase_align", true);
  out.autoFrequency = v->GetBool("auto_frequency", true);
  out.freqDevThreshold = (float)v->GetNumber("freq_dev_threshold", 0.05);
  if (!IsFinite(out.amplitudeDeg) || !IsFinite(out.amplitudeDownDeg) ||
      !IsFinite(out.frequencyHz) || !IsFinite(out.phaseOffsetDeg) ||
      !IsFinite(out.freqDevThreshold))
    return false;
  return true;
}

// Parse the shared "params" shape used by both the default DB (as defaults)
// and user presets (as overrides): gait/envelope/jump/native_amplify.
static bool ParseParamBlock(const jsonmini::Value &v, CharacterProfile &p) {
  if (v.type != jsonmini::Value::Object) return false;
  if (const jsonmini::Value *g = v.Find("gait")) {
    if (g->type != jsonmini::Value::Object) return false;
    if (!ParseGaitParam(g->Find("idle"), p.idle, 0.0f, 1.2f)) return false;
    if (!ParseGaitParam(g->Find("walk"), p.walk, 3.6f, 1.5f)) return false;
    if (!ParseGaitParam(g->Find("run"), p.run, 8.5f, 1.7f)) return false;
    if (!ParseGaitParam(g->Find("sprint"), p.sprint, 12.0f, 2.0f))
      return false;
    if (!ParseGaitParam(g->Find("zipline"), p.zipline, 8.5f, 1.7f))
      return false;  // defaults = run-family values
  }
  if (const jsonmini::Value *env = v.Find("envelope")) {
    if (env->type != jsonmini::Value::Object ||
        !HasTypeIfPresent(*env, "amplitude_attack_tau_sec",
                          jsonmini::Value::Number) ||
        !HasTypeIfPresent(*env, "frequency_tau_sec", jsonmini::Value::Number) ||
        !HasTypeIfPresent(*env, "to_idle_release_tau_sec",
                          jsonmini::Value::Number))
      return false;
    p.envelope.amplitudeAttackTauSec =
        (float)env->GetNumber("amplitude_attack_tau_sec", 0.15);
    p.envelope.frequencyTauSec =
        (float)env->GetNumber("frequency_tau_sec", 0.20);
    p.envelope.toIdleReleaseTauSec =
        (float)env->GetNumber("to_idle_release_tau_sec", 0.015);
  }
  if (!IsFinite(p.envelope.amplitudeAttackTauSec) ||
      !IsFinite(p.envelope.frequencyTauSec) ||
      !IsFinite(p.envelope.toIdleReleaseTauSec))
    return false;
  if (const jsonmini::Value *j = v.Find("jump")) {
    if (j->type != jsonmini::Value::Object ||
        !HasTypeIfPresent(*j, "enabled", jsonmini::Value::Bool) ||
        !HasTypeIfPresent(*j, "mode", jsonmini::Value::String) ||
        !HasTypeIfPresent(*j, "amplitude_deg", jsonmini::Value::Number) ||
        !HasTypeIfPresent(*j, "damping_tau_sec", jsonmini::Value::Number) ||
        !HasTypeIfPresent(*j, "frequency_hz", jsonmini::Value::Number) ||
        !HasTypeIfPresent(*j, "max_duration_sec", jsonmini::Value::Number) ||
        !HasTypeIfPresent(*j, "takeoff_delay_sec", jsonmini::Value::Number) ||
        !HasTypeIfPresent(*j, "rising_target_deg", jsonmini::Value::Number) ||
        !HasTypeIfPresent(*j, "apex_falling_target_deg", jsonmini::Value::Number) ||
        !HasTypeIfPresent(*j, "acceleration_response", jsonmini::Value::Number) ||
        !HasTypeIfPresent(*j, "acceleration_filter_tau_sec", jsonmini::Value::Number) ||
        !HasTypeIfPresent(*j, "natural_frequency_hz", jsonmini::Value::Number) ||
        !HasTypeIfPresent(*j, "damping_ratio", jsonmini::Value::Number) ||
        !HasTypeIfPresent(*j, "landing_impulse_gain", jsonmini::Value::Number))
      return false;
    p.jump.enabled = j->GetBool("enabled", false);  // V2: default OFF
    p.jump.mode = j->GetString("mode", "off");
    p.jump.amplitudeDeg = (float)j->GetNumber("amplitude_deg", p.jump.amplitudeDeg);
    p.jump.dampingTauSec = (float)j->GetNumber("damping_tau_sec", p.jump.dampingTauSec);
    p.jump.frequencyHz = (float)j->GetNumber("frequency_hz", p.jump.frequencyHz);
    p.jump.maxDurationSec = (float)j->GetNumber("max_duration_sec", p.jump.maxDurationSec);
    p.jump.takeoffDelaySec = (float)j->GetNumber("takeoff_delay_sec", p.jump.takeoffDelaySec);
    p.jump.risingTargetDeg = (float)j->GetNumber("rising_target_deg", p.jump.risingTargetDeg);
    p.jump.apexFallingTargetDeg = (float)j->GetNumber("apex_falling_target_deg", p.jump.apexFallingTargetDeg);
    p.jump.accelerationResponse = (float)j->GetNumber("acceleration_response", p.jump.accelerationResponse);
    p.jump.accelerationFilterTauSec = (float)j->GetNumber("acceleration_filter_tau_sec", p.jump.accelerationFilterTauSec);
    p.jump.naturalFrequencyHz = (float)j->GetNumber("natural_frequency_hz", p.jump.naturalFrequencyHz);
    p.jump.dampingRatio = (float)j->GetNumber("damping_ratio", p.jump.dampingRatio);
    p.jump.landingImpulseGain = (float)j->GetNumber("landing_impulse_gain", p.jump.landingImpulseGain);
  }
  if (p.jump.mode != "off" && p.jump.mode != "landing_damped") return false;
  if (!IsFinite(p.jump.amplitudeDeg) || !IsFinite(p.jump.dampingTauSec) ||
      !IsFinite(p.jump.frequencyHz) || !IsFinite(p.jump.maxDurationSec) ||
      !IsFinite(p.jump.takeoffDelaySec) || !IsFinite(p.jump.risingTargetDeg) ||
      !IsFinite(p.jump.apexFallingTargetDeg) ||
      !IsFinite(p.jump.accelerationResponse) ||
      !IsFinite(p.jump.accelerationFilterTauSec) ||
      !IsFinite(p.jump.naturalFrequencyHz) ||
      !IsFinite(p.jump.dampingRatio) ||
      !IsFinite(p.jump.landingImpulseGain))
    return false;
  if (const jsonmini::Value *na = v.Find("native_amplify")) {
    if (na->type != jsonmini::Value::Object ||
        !HasTypeIfPresent(*na, "factor", jsonmini::Value::Number))
      return false;
    p.nativeAmplify.factor = (float)na->GetNumber("factor", 2.0f);
  }
  if (!IsFinite(p.nativeAmplify.factor) || p.nativeAmplify.factor <= 0.0f)
    return false;
  return true;
}

static int ParseAnimationRuleGait(const std::string &name) {
  if (name == "walk") return GaitWalk;
  if (name == "run") return GaitRun;
  if (name == "sprint") return GaitSprint;
  if (name == "zipline") return GaitZipline;
  return GaitNone;
}

static void ParseAnimationRules(const jsonmini::Value &character,
                                CharacterProfile &profile) {
  const jsonmini::Value *rules = character.Find("animation_rules");
  if (!rules || rules->type != jsonmini::Value::Object) return;
  // Keep the 20Hz hot-path bounded. Reject oversized rule sets instead of
  // silently loading only a prefix that appears valid in the Manager.
  constexpr size_t kMaxAnimationRules = 16;
  if (rules->obj.size() > kMaxAnimationRules) {
    Log("[CFG] animation_rules has %zu entries (max %zu) -> ignored",
        rules->obj.size(), kMaxAnimationRules);
    return;
  }
  for (const auto &kv : rules->obj) {
    if (kv.first.empty() || kv.second.type != jsonmini::Value::String) continue;
    int gait = ParseAnimationRuleGait(kv.second.str);
    if (gait == GaitNone) continue;
    AnimationRule rule;
    rule.contains = kv.first;
    rule.gait = gait;
    profile.animationRules.push_back(rule);
  }
}

// ---- characters.default.json (technical facts) ----
// Each entry: display_name, bones{right,left,allow_fallback_candidates},
// axis{name,sign} (optional -> auto),
// animation_rules{"clip_fragment":"walk|run|sprint|zipline"}
// (optional special movement whitelist), defaults{ gait, envelope, jump,
// native_amplify }.
static bool LoadCharacterDatabase(const char *path, ConfigSnapshot &snap) {
  jsonmini::Value root;
  if (!jsonmini::LoadJsonFile(path, root)) {
    Log("[CFG] characters.default.json missing/unreadable: %s", path);
    return false;
  }
  if ((uint32_t)root.GetNumber("schema_version", 1) != 1) {
    Log("[CFG] characters.default.json schema unsupported");
    return false;
  }
  const jsonmini::Value *chars = root.Find("characters");
  if (!chars || chars->type != jsonmini::Value::Object) {
    Log("[CFG] characters.default.json has no characters object");
    return false;
  }
  for (auto &kv : chars->obj) {
    const std::string &id = kv.first;
    const jsonmini::Value &c = kv.second;
    if (c.type != jsonmini::Value::Object) {
      Log("[CFG] db character '%s': entry must be an object -> INVALID",
          id.c_str());
      return false;
    }
    CharacterProfile p;
    p.enabled = true;  // DB chars are supported; preset decides enable
    p.motionMode = MotionMode::Synthetic;
    if (const jsonmini::Value *b = c.Find("bones")) {
      if (b->type != jsonmini::Value::Object) {
        Log("[CFG] db character '%s': bones must be an object -> INVALID",
            id.c_str());
        return false;
      }
      p.bones.rightName = b->GetString("right", "");
      p.bones.leftName = b->GetString("left", "");
      p.bones.allowFallbackCandidates =
          b->GetBool("allow_fallback_candidates", true);
      if (!HasCompleteBonePair(p.bones.rightName, p.bones.leftName)) {
        Log("[CFG] db character '%s': bones right/left must both be set or both empty -> INVALID",
            id.c_str());
        return false;
      }
    }
    if (const jsonmini::Value *axis = c.Find("axis")) {
      if (axis->type != jsonmini::Value::Object ||
          !ParseAxisStrict(axis->GetString("name", ""), p.axis.axis)) {
        Log("[CFG] db character '%s': axis.name must be X/Y/Z -> INVALID",
            id.c_str());
        return false;
      }
      double sign = axis->GetNumber("sign", 1.0);
      if (!IsValidAxisSign(sign)) {
        Log("[CFG] db character '%s': axis.sign must be -1 or +1 -> INVALID",
            id.c_str());
        return false;
      }
      p.axis.sign = (float)sign;
      p.axisExplicit = true;
    }
    if (const jsonmini::Value *def = c.Find("defaults")) {
      if (!ParseParamBlock(*def, p)) {
        Log("[CFG] db character '%s' defaults invalid -> skipped", id.c_str());
        continue;
      }
    }
    ParseAnimationRules(c, p);
    snap.characters[id] = p;
  }
  Log("[CFG] character db loaded: %zu entries", snap.characters.size());
  return true;
}

// Apply one preset entry to a temporary profile. The caller commits the copy
// only after every field validates, preventing half-applied overrides.
static bool ApplyCharacterOverride(const std::string &id,
                                   const jsonmini::Value &v,
                                   CharacterProfile &p) {
  if (v.type != jsonmini::Value::Object) {
    Log("[CFG] preset character '%s': entry must be an object -> INVALID",
        id.c_str());
    return false;
  }
  if (const jsonmini::Value *enabled = v.Find("enabled")) {
    if (enabled->type != jsonmini::Value::Bool) {
      Log("[CFG] preset character '%s': enabled must be boolean -> INVALID",
          id.c_str());
      return false;
    }
    p.enabled = enabled->b;
  }
  if (const jsonmini::Value *mode = v.Find("motion_mode")) {
    if (mode->type != jsonmini::Value::String ||
        !ParseMotionModeStrict(mode->str, p.motionMode)) {
      Log("[CFG] preset character '%s': motion_mode must be off/synthetic/amplify_native -> INVALID",
          id.c_str());
      return false;
    }
  }
  if (const jsonmini::Value *bones = v.Find("bones")) {
    if (bones->type != jsonmini::Value::Object) {
      Log("[CFG] preset character '%s': bones must be an object -> INVALID",
          id.c_str());
      return false;
    }
    if (const jsonmini::Value *right = bones->Find("right")) {
      if (right->type != jsonmini::Value::String) {
        Log("[CFG] preset character '%s': bones.right must be string -> INVALID",
            id.c_str());
        return false;
      }
      p.bones.rightName = right->str;
    }
    if (const jsonmini::Value *left = bones->Find("left")) {
      if (left->type != jsonmini::Value::String) {
        Log("[CFG] preset character '%s': bones.left must be string -> INVALID",
            id.c_str());
        return false;
      }
      p.bones.leftName = left->str;
    }
    if (const jsonmini::Value *fallback =
            bones->Find("allow_fallback_candidates")) {
      if (fallback->type != jsonmini::Value::Bool) {
        Log("[CFG] preset character '%s': allow_fallback_candidates must be boolean -> INVALID",
            id.c_str());
        return false;
      }
      p.bones.allowFallbackCandidates = fallback->b;
    }
    if (!HasCompleteBonePair(p.bones.rightName, p.bones.leftName)) {
      Log("[CFG] preset character '%s': bones right/left must both be set or both empty -> INVALID",
          id.c_str());
      return false;
    }
  }
  if (const jsonmini::Value *axis = v.Find("axis")) {
    if (axis->type != jsonmini::Value::Object) {
      Log("[CFG] preset character '%s': axis must be an object -> INVALID",
          id.c_str());
      return false;
    }
    if (const jsonmini::Value *name = axis->Find("name")) {
      if (name->type != jsonmini::Value::String ||
          !ParseAxisStrict(name->str, p.axis.axis)) {
        Log("[CFG] preset character '%s': axis.name must be X/Y/Z -> INVALID",
            id.c_str());
        return false;
      }
    }
    if (const jsonmini::Value *sign = axis->Find("sign")) {
      if (sign->type != jsonmini::Value::Number ||
          !IsValidAxisSign(sign->num)) {
        Log("[CFG] preset character '%s': axis.sign must be -1 or +1 -> INVALID",
            id.c_str());
        return false;
      }
      p.axis.sign = (float)sign->num;
    }
    p.axisExplicit = true;
  }
  if (!ParseParamBlock(v, p)) {
    Log("[CFG] preset character '%s': parameter block invalid -> INVALID",
        id.c_str());
    return false;
  }
  return true;
}

// ---- user preset (overrides) ----
// Entries may be partial: only written fields override the DB.
static bool ApplyUserPreset(const char *path, ConfigSnapshot &snap) {
  jsonmini::Value root;
  if (!jsonmini::LoadJsonFile(path, root)) {
    Log("[CFG] preset missing/unreadable: %s", path);
    return false;
  }
  if ((uint32_t)root.GetNumber("schema_version", 1) != 1) {
    Log("[CFG] preset schema unsupported: %s", path);
    return false;
  }
  const jsonmini::Value *chars = root.Find("characters");
  if (!chars || chars->type != jsonmini::Value::Object) {
    Log("[CFG] preset has no characters object: %s", path);
    return false;
  }
  int overridden = 0;
  for (auto &kv : chars->obj) {
    const std::string &id = kv.first;
    const jsonmini::Value &v = kv.second;
    auto it = snap.characters.find(id);
    CharacterProfile candidate;
    if (it != snap.characters.end())
      candidate = it->second;
    else {
      candidate.enabled = true;
      candidate.motionMode = MotionMode::Synthetic;
    }
    if (!ApplyCharacterOverride(id, v, candidate)) return false;
    snap.characters[id] = candidate;
    overridden++;
  }
  Log("[CFG] preset applied: %d character overrides", overridden);
  return true;
}

// ---- runtime/config.json ----
struct RuntimeConfigFile {
  int revision = 0;
  bool enabled = true;
  std::string activePreset = kDefaultPreset;
  bool hasGlobal = false;
  jsonmini::Value global;
};

static bool IsNonNegativeInt(double value) {
  return IsFinite(value) && value >= 0.0 && value <= 2147483647.0 &&
         value == (double)(int)value;
}

static bool IsPositiveUint(double value) {
  return IsFinite(value) && value >= 1.0 && value <= 4294967295.0 &&
         value == (double)(uint32_t)value;
}

static bool ParseRuntimeConfigValue(const jsonmini::Value &root,
                                    RuntimeConfigFile &out) {
  if (root.type != jsonmini::Value::Object) {
    Log("[CFG] runtime/config.json: root must be an object -> INVALID");
    return false;
  }
  if (const jsonmini::Value *revision = root.Find("revision")) {
    if (revision->type != jsonmini::Value::Number ||
        !IsNonNegativeInt(revision->num)) {
      Log("[CFG] runtime/config.json: revision must be a non-negative integer -> INVALID");
      return false;
    }
    out.revision = (int)revision->num;
  }
  if (const jsonmini::Value *enabled = root.Find("enabled")) {
    if (enabled->type != jsonmini::Value::Bool) {
      Log("[CFG] runtime/config.json: enabled must be boolean -> INVALID");
      return false;
    }
    out.enabled = enabled->b;
  }
  if (const jsonmini::Value *preset = root.Find("active_preset")) {
    if (preset->type != jsonmini::Value::String) {
      Log("[CFG] runtime/config.json: active_preset must be string -> INVALID");
      return false;
    }
    if (!preset->str.empty()) out.activePreset = preset->str;
  }
  if (const jsonmini::Value *global = root.Find("global")) {
    if (global->type != jsonmini::Value::Object) {
      Log("[CFG] runtime/config.json: global must be an object -> INVALID");
      return false;
    }
    const char *intervals[] = {
        "gait_sample_interval_ms", "entity_refresh_interval_ms",
        "replay_verify_window_ms"};
    for (const char *key : intervals) {
      if (const jsonmini::Value *value = global->Find(key)) {
        if (value->type != jsonmini::Value::Number ||
            !IsPositiveUint(value->num)) {
          Log("[CFG] runtime/config.json: global.%s must be a positive integer -> INVALID",
              key);
          return false;
        }
      }
    }
    if (!HasTypeIfPresent(*global, "legacy_marker_mode",
                          jsonmini::Value::Bool)) {
      Log("[CFG] runtime/config.json: global.legacy_marker_mode must be boolean -> INVALID");
      return false;
    }
    out.hasGlobal = true;
    out.global = *global;
  }
  return true;
}

static bool LoadRuntimeConfigFile(RuntimeConfigFile &out) {
  char path[512];
  RuntimePath(path, sizeof(path), "runtime\\config.json");
  jsonmini::Value root;
  if (!jsonmini::LoadJsonFile(path, root)) {
    Log("[CFG] runtime/config.json missing/unreadable: %s", path);
    ConfigSetLastError("config invalid: runtime/config.json missing or unreadable");
    return false;
  }
  if (!ParseRuntimeConfigValue(root, out)) {
    ConfigSetLastError("config invalid: runtime/config.json field rejected; see log");
    return false;
  }
  return true;
}

static void ApplyGlobalOverride(ConfigSnapshot &snap,
                                const RuntimeConfigFile &rc) {
  if (!rc.hasGlobal) return;
  const jsonmini::Value &g = rc.global;
  snap.global.gaitSampleIntervalMs =
      (uint32_t)g.GetNumber("gait_sample_interval_ms", 50);
  snap.global.entityRefreshIntervalMs =
      (uint32_t)g.GetNumber("entity_refresh_interval_ms", 500);
  snap.global.replayVerifyWindowMs =
      (uint32_t)g.GetNumber("replay_verify_window_ms", 150);
  snap.global.legacyMarkerMode = g.GetBool("legacy_marker_mode", false);
}

// ---- main entry (startup + hot reload) ----
// Builds a full snapshot from the three files.  On failure returns an
// invalid snapshot (caller keeps the last good one).
static ConfigSnapshot LoadConfigSnapshot(int revision) {
  ConfigClearLastError();
  ConfigSnapshot snap;
  snap.revision = revision;

  if (!RuntimePathsInit()) {
    Log("[CFG] runtime root unavailable -> INVALID");
    ConfigSetLastError("config invalid: runtime root unavailable");
    return snap;
  }

  char dbPath[512];
  RuntimePath(dbPath, sizeof(dbPath), "data\\characters.default.json");

  // runtime/config.json first (knows the active preset)
  RuntimeConfigFile rc;
  if (!LoadRuntimeConfigFile(rc)) {
    ConfigSetLastError("config invalid: runtime/config.json unreadable or malformed");
    return snap;
  }
  snap.presetName = rc.activePreset;
  // Global switch: enabled=false keeps loading config but marks the snapshot
  // disabled (MotionEngine -> no write). Re-enabling is instant on the next
  // revision bump.
  snap.pluginEnabled = rc.enabled;

  // preset name sanity (no path traversal)
  for (char ch : snap.presetName) {
    if (ch == '.' || ch == '/' || ch == '\\' || ch == ':') {
      Log("[CFG] invalid preset name '%s' -> INVALID", snap.presetName.c_str());
      ConfigSetLastError("config invalid: active_preset contains forbidden path characters");
      return snap;
    }
  }

  if (!LoadCharacterDatabase(dbPath, snap)) {
    ConfigSetLastError("config invalid: character database rejected; see log");
    return snap;
  }
  char presetPath[512];
  snprintf(presetPath, sizeof(presetPath), "%s\\presets\\%s.json",
           g_runtimeRoot, snap.presetName.c_str());
  if (!ApplyUserPreset(presetPath, snap)) {
    ConfigSetLastError("config invalid: active preset rejected; see log");
    return snap;
  }
  ApplyGlobalOverride(snap, rc);

  snap.schemaVersion = 1;
  if (!ValidateSnapshot(snap)) {
    ConfigSetLastError("config invalid: final snapshot validation failed; see log");
    return snap;
  }
  snap.valid = true;
  ConfigClearLastError();
  Log("[CFG] snapshot ready preset=%s revision=%d chars=%zu", 
      snap.presetName.c_str(), revision, snap.characters.size());
  return snap;
}

// Hot-reload entry: called from the service thread on revision change.
// Returns true when a new valid snapshot was installed.
static bool ConfigReloadIfChanged(int newRevision) {
  const ConfigSnapshot *cur = ConfigAcquire();
  if (cur && cur->revision == newRevision) return false;
  ConfigSnapshot fresh = LoadConfigSnapshot(newRevision);
  if (!fresh.valid) {
    Log("[CFG] reload revision=%d INVALID -> keeping last good (rev=%d)",
        newRevision, cur ? cur->revision : -1);
    return false;
  }
  ConfigSnapshot *installed = new ConfigSnapshot(std::move(fresh));
  const ConfigSnapshot *old = g_configPtr.exchange(installed,
                                                   std::memory_order_acq_rel);
  ConfigRetireOld(old);
  Log("[CFG] reload installed revision=%d", newRevision);
  return true;
}

// Startup entry: installs the initial snapshot (valid required).
static bool ConfigInstallInitial(int revision) {
  ConfigSnapshot fresh = LoadConfigSnapshot(revision);
  if (!fresh.valid) return false;
  ConfigSnapshot *installed = new ConfigSnapshot(std::move(fresh));
  g_configPtr.store(installed, std::memory_order_release);
  return true;
}
