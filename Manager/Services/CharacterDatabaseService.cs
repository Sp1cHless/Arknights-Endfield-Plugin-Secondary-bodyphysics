// Services/CharacterDatabaseService.cs — data/characters.default.json
// (technical facts: display name, bones/axis are shown; defaults feed the
// merged model when the preset has no override).
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using SecondaryMotion.Manager.Models;

namespace SecondaryMotion.Manager.Services;

public class CharacterDatabaseService {
    readonly string _path;        // PRIMARY: tool folder (movable/backup-able)
    readonly string _mirrorPath;  // MIRROR: game data dir (runtime reads here)
    public bool Loaded { get; private set; }

    public CharacterDatabaseService(string dataRoot, string managerDir) {
        _path = Path.Combine(managerDir, "data", "characters.default.json");
        _mirrorPath = Path.Combine(dataRoot, "data", "characters.default.json");
        // migration: user data lives in the game dir; copy it in when the
        // tool folder copy is missing or older (templates are old).
        try {
            if (File.Exists(_mirrorPath) &&
                (!File.Exists(_path) ||
                 File.GetLastWriteTime(_mirrorPath) > File.GetLastWriteTime(_path))) {
                Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
                File.Copy(_mirrorPath, _path, true);
            }
        } catch { }
    }

    public List<CharacterData> Load() {
        var list = new List<CharacterData>();
        Loaded = false;
        if (!File.Exists(_path)) return list;
        try {
            var map = new Dictionary<string, Dictionary<string, object>>();
            if (!JsonMini.ParseObjectMap(File.ReadAllText(_path, Encoding.UTF8), "characters", map))
                return list;
            foreach (var kv in map) {
                var c = new CharacterData { Id = kv.Key };
                var node = kv.Value;
                c.DisplayName = JsonMini.GetStr(node, "display_name", "");
                c.OriginalDisplayName = c.DisplayName;
                object o;
                if (node.TryGetValue("bones", out o) && o is Dictionary<string, object> bones) {
                    c.BoneRight = JsonMini.GetStr(bones, "right", "");
                    c.BoneLeft = JsonMini.GetStr(bones, "left", "");
                }
                if (node.TryGetValue("defaults", out o) && o is Dictionary<string, object> defs) {
                    if (defs.TryGetValue("gait", out o) && o is Dictionary<string, object> gait)
                        ApplyGait(gait, c);
                    if (defs.TryGetValue("envelope", out o) && o is Dictionary<string, object> env) {
                        c.EnvAttack = JsonMini.GetNum(env, "amplitude_attack_tau_sec", c.EnvAttack);
                        c.EnvFreq = JsonMini.GetNum(env, "frequency_tau_sec", c.EnvFreq);
                        c.EnvIdle = JsonMini.GetNum(env, "to_idle_release_tau_sec", c.EnvIdle);
                    }
                    if (defs.TryGetValue("jump", out o) && o is Dictionary<string, object> jump)
                        ApplyJump(jump, c);
                    if (defs.TryGetValue("native_amplify", out o) && o is Dictionary<string, object> na)
                        c.NativeFactor = JsonMini.GetNum(na, "factor", c.NativeFactor);
                }
                if (node.TryGetValue("axis", out o) && o is Dictionary<string, object> axis) {
                    c.Axis = JsonMini.GetStr(axis, "name", "");
                    c.AxisSign = JsonMini.GetNum(axis, "sign", 1.0) < 0 ? -1 : 1;
                }
                list.Add(c);
            }
            Loaded = true;
        } catch { }
        return list;
    }

    public static void ApplyGait(Dictionary<string, object> gait, CharacterData c) {
        string[] names = { "idle", "walk", "run", "sprint", "zipline" };
        for (int i = 0; i < 5; i++) {
            object o;
            if (!gait.TryGetValue(names[i], out o) || !(o is Dictionary<string, object> g)) continue;
            c.Amp[i] = JsonMini.GetNum(g, "amplitude_deg", c.Amp[i]);
            c.AmpDown[i] = JsonMini.GetNum(g, "amplitude_down_deg", 0.0);
            c.Freq[i] = JsonMini.GetNum(g, "frequency_hz", c.Freq[i]);
            c.PhaseOffset[i] = JsonMini.GetNum(g, "phase_offset_deg", c.PhaseOffset[i]);
            c.PhaseAlign[i] = JsonMini.GetBool(g, "phase_align", c.PhaseAlign[i]);
            c.AutoFreq[i] = JsonMini.GetBool(g, "auto_frequency", c.AutoFreq[i]);
            c.FreqDevThreshold[i] = JsonMini.GetNum(g, "freq_dev_threshold", c.FreqDevThreshold[i]);
        }
    }

    public static void ApplyJump(Dictionary<string, object> jump, CharacterData c) {
        c.JumpEnabled = JsonMini.GetBool(jump, "enabled", c.JumpEnabled);
        c.JumpAmplitude = JsonMini.GetNum(jump, "amplitude_deg", c.JumpAmplitude);
        c.JumpDampingTau = JsonMini.GetNum(jump, "damping_tau_sec", c.JumpDampingTau);
        c.JumpFrequency = JsonMini.GetNum(jump, "frequency_hz", c.JumpFrequency);
        c.JumpMaxDuration = JsonMini.GetNum(jump, "max_duration_sec", c.JumpMaxDuration);
        c.JumpTakeoffDelay = JsonMini.GetNum(jump, "takeoff_delay_sec", c.JumpTakeoffDelay);
        c.JumpRisingTarget = JsonMini.GetNum(jump, "rising_target_deg", c.JumpRisingTarget);
        c.JumpApexFallingTarget = JsonMini.GetNum(jump, "apex_falling_target_deg", c.JumpApexFallingTarget);
        c.JumpAccelerationResponse = JsonMini.GetNum(jump, "acceleration_response", c.JumpAccelerationResponse);
        c.JumpAccelerationFilterTau = JsonMini.GetNum(jump, "acceleration_filter_tau_sec", c.JumpAccelerationFilterTau);
        c.JumpNaturalFrequency = JsonMini.GetNum(jump, "natural_frequency_hz", c.JumpNaturalFrequency);
        c.JumpDampingRatio = JsonMini.GetNum(jump, "damping_ratio", c.JumpDampingRatio);
        c.JumpLandingImpulseGain = JsonMini.GetNum(jump, "landing_impulse_gain", c.JumpLandingImpulseGain);
    }

    public static string JumpEntryJson(CharacterData c) {
        return "{ \"enabled\": " + (c.JumpEnabled ? "true" : "false") +
            ", \"mode\": \"" + (c.JumpEnabled ? "landing_damped" : "off") + "\"" +
            ", \"amplitude_deg\": " + JsonMini.Num(c.JumpAmplitude) +
            ", \"damping_tau_sec\": " + JsonMini.Num(c.JumpDampingTau) +
            ", \"frequency_hz\": " + JsonMini.Num(c.JumpFrequency) +
            ", \"max_duration_sec\": " + JsonMini.Num(c.JumpMaxDuration) +
            ", \"takeoff_delay_sec\": " + JsonMini.Num(c.JumpTakeoffDelay) +
            ", \"rising_target_deg\": " + JsonMini.Num(c.JumpRisingTarget) +
            ", \"apex_falling_target_deg\": " + JsonMini.Num(c.JumpApexFallingTarget) +
            ", \"acceleration_response\": " + JsonMini.Num(c.JumpAccelerationResponse) +
            ", \"acceleration_filter_tau_sec\": " + JsonMini.Num(c.JumpAccelerationFilterTau) +
            ", \"natural_frequency_hz\": " + JsonMini.Num(c.JumpNaturalFrequency) +
            ", \"damping_ratio\": " + JsonMini.Num(c.JumpDampingRatio) +
            ", \"landing_impulse_gain\": " + JsonMini.Num(c.JumpLandingImpulseGain) + " }";
    }

    // Serialize one gait entry (used by preset + DB writers).  Only
    // non-default values are emitted (absent = default), keeping files lean
    // and older runtimes backward compatible.
    public static string GaitEntryJson(string name, double amp, double down, double freq,
                                       double phaseOffset = 0.0,
                                       bool phaseAlign = true, bool autoFreq = true,
                                       double freqDevThreshold = 0.05) {
        var sb = new StringBuilder();
        sb.Append(JsonMini.Str(name))
          .Append(": { \"amplitude_deg\": ").Append(JsonMini.Num(amp));
        if (down > 0)
            sb.Append(", \"amplitude_down_deg\": ").Append(JsonMini.Num(down));
        sb.Append(", \"frequency_hz\": ").Append(JsonMini.Num(freq));
        if (phaseOffset != 0.0)
            sb.Append(", \"phase_offset_deg\": ").Append(JsonMini.Num(phaseOffset));
        if (!phaseAlign)
            sb.Append(", \"phase_align\": false");
        if (!autoFreq)
            sb.Append(", \"auto_frequency\": false");
        if (freqDevThreshold != 0.05)
            sb.Append(", \"freq_dev_threshold\": ").Append(JsonMini.Num(freqDevThreshold));
        sb.Append(" }");
        return sb.ToString();
    }

    // Update only the display_name of an existing entry (keeps bones/axis/
    // defaults untouched).  Backup + atomic write.
    public void SaveDisplayName(string id, string newName) {
        Dictionary<string, object> root;
        if (!File.Exists(_path)) return;
        JsonMini.ParseTopLevel(File.ReadAllText(_path, Encoding.UTF8), out root);
        if (!root.ContainsKey("characters")) return;
        var chars = (Dictionary<string, object>)root["characters"];
        if (!chars.ContainsKey(id)) return;
        var entry = (Dictionary<string, object>)chars[id];
        entry["display_name"] = newName;
        File.Copy(_path, _path + ".bak", true);
        PresetService.AtomicWrite(_path, SerializeCharacters(chars));
    }

    static string SerializeCharacters(Dictionary<string, object> chars) {
        var sb = new StringBuilder();
        sb.Append("{\r\n  \"schema_version\": 1,\r\n  \"characters\": {\r\n");
        int i = 0;
        foreach (var kv in chars) {
            sb.Append("    ").Append(JsonMini.Str(kv.Key)).Append(": ")
              .Append(WriteEntry((Dictionary<string, object>)kv.Value))
              .Append(i < chars.Count - 1 ? "," : "").Append("\r\n");
            i++;
        }
        sb.Append("  }\r\n}\r\n");
        return sb.ToString();
    }

    // Write/update one character entry in characters.default.json
    // (backup .bak -> atomic tmp+rename).  Used by the Developer Wizard.
    public void SaveCharacter(CharacterData c, string displayName,
                              string rightBone, string leftBone) {
        Dictionary<string, object> root;
        if (File.Exists(_path))
            JsonMini.ParseTopLevel(File.ReadAllText(_path, Encoding.UTF8), out root);
        else
            root = new Dictionary<string, object>();
        if (!root.ContainsKey("characters"))
            root["characters"] = new Dictionary<string, object>();
        var chars = (Dictionary<string, object>)root["characters"];

        // Technical animation rules are DB-owned. Re-scanning an existing
        // character may replace bones/defaults, but must not erase its
        // official special-movement whitelist.
        Dictionary<string, object>? existingAnimationRules = null;
        if (chars.TryGetValue(c.Id, out object? existingObj) &&
            existingObj is Dictionary<string, object> existingEntry &&
            existingEntry.TryGetValue("animation_rules", out object? rulesObj) &&
            rulesObj is Dictionary<string, object> rules)
            existingAnimationRules = rules;

        var entry = new Dictionary<string, object>();
        entry["display_name"] = displayName;
        var bones = new Dictionary<string, object>();
        bones["right"] = rightBone;
        bones["left"] = leftBone;
        bones["allow_fallback_candidates"] = true;
        entry["bones"] = bones;
        if (c.Axis.Length > 0) {
            var axis = new Dictionary<string, object>();
            axis["name"] = c.Axis;
            axis["sign"] = c.AxisSign < 0 ? -1.0 : 1.0;
            entry["axis"] = axis;
        }
        var defaults = new Dictionary<string, object>();
        var gait = new Dictionary<string, object>();
        string[] gnames = { "idle", "walk", "run", "sprint", "zipline" };
        for (int i = 0; i < 5; i++) {
            var g = new Dictionary<string, object>();
            g["amplitude_deg"] = c.Amp[i];
            if (c.AmpDown[i] > 0) g["amplitude_down_deg"] = c.AmpDown[i];
            g["frequency_hz"] = c.Freq[i];
            g["phase_offset_deg"] = c.PhaseOffset[i];
            g["phase_align"] = c.PhaseAlign[i];
            g["auto_frequency"] = c.AutoFreq[i];
            g["freq_dev_threshold"] = c.FreqDevThreshold[i];
            gait[gnames[i]] = g;
        }
        defaults["gait"] = gait;
        var env = new Dictionary<string, object>();
        env["amplitude_attack_tau_sec"] = c.EnvAttack;
        env["frequency_tau_sec"] = c.EnvFreq;
        env["to_idle_release_tau_sec"] = c.EnvIdle;
        defaults["envelope"] = env;
        var jump = new Dictionary<string, object>();
        jump["enabled"] = c.JumpEnabled;
        jump["mode"] = c.JumpEnabled ? "landing_damped" : "off";
        jump["amplitude_deg"] = c.JumpAmplitude;
        jump["damping_tau_sec"] = c.JumpDampingTau;
        jump["frequency_hz"] = c.JumpFrequency;
        jump["max_duration_sec"] = c.JumpMaxDuration;
        jump["takeoff_delay_sec"] = c.JumpTakeoffDelay;
        jump["rising_target_deg"] = c.JumpRisingTarget;
        jump["apex_falling_target_deg"] = c.JumpApexFallingTarget;
        jump["acceleration_response"] = c.JumpAccelerationResponse;
        jump["acceleration_filter_tau_sec"] = c.JumpAccelerationFilterTau;
        jump["natural_frequency_hz"] = c.JumpNaturalFrequency;
        jump["damping_ratio"] = c.JumpDampingRatio;
        jump["landing_impulse_gain"] = c.JumpLandingImpulseGain;
        defaults["jump"] = jump;
        var na = new Dictionary<string, object>();
        na["factor"] = c.NativeFactor;
        defaults["native_amplify"] = na;
        entry["defaults"] = defaults;
        if (existingAnimationRules != null)
            entry["animation_rules"] = existingAnimationRules;
        chars[c.Id] = entry;
        var sb = new StringBuilder();
        sb.Append("{\r\n  \"schema_version\": 1,\r\n  \"characters\": {\r\n");
        int i2 = 0;
        foreach (var kv in chars) {
            sb.Append("    ").Append(JsonMini.Str(kv.Key)).Append(": ")
              .Append(WriteEntry((Dictionary<string, object>)kv.Value))
              .Append(i2 < chars.Count - 1 ? "," : "").Append("\r\n");
            i2++;
        }
        sb.Append("  }\r\n}\r\n");

        if (File.Exists(_path))
            File.Copy(_path, _path + ".bak", true);
        PresetService.AtomicWrite(_path, sb.ToString());
        // mirror to the game dir so the runtime sees the change
        try {
            PresetService.AtomicWrite(_mirrorPath, sb.ToString());
        } catch { }
    }

    // Remove one character entirely from the DB (back to "unknown" so the
    // Developer wizard can re-scan and re-record it).  Backup + atomic write.
    public void DeleteCharacter(string id) {
        Dictionary<string, object> root;
        if (!File.Exists(_path)) return;
        if (!JsonMini.ParseTopLevel(File.ReadAllText(_path, Encoding.UTF8), out root))
            return;
        if (!root.ContainsKey("characters")) return;
        var chars = (Dictionary<string, object>)root["characters"];
        if (!chars.ContainsKey(id)) return;
        chars.Remove(id);
        var sb = new StringBuilder();
        sb.Append("{\r\n  \"schema_version\": 1,\r\n  \"characters\": {\r\n");
        int i2 = 0;
        foreach (var kv in chars) {
            sb.Append("    ").Append(JsonMini.Str(kv.Key)).Append(": ")
              .Append(WriteEntry((Dictionary<string, object>)kv.Value))
              .Append(i2 < chars.Count - 1 ? "," : "").Append("\r\n");
            i2++;
        }
        sb.Append("  }\r\n}\r\n");
        File.Copy(_path, _path + ".bak", true);
        PresetService.AtomicWrite(_path, sb.ToString());
    }

    static string WriteEntry(Dictionary<string, object> e) {
        var sb = new StringBuilder();
        sb.Append("{\r\n      \"display_name\": ").Append(JsonMini.Str((string)e["display_name"])).Append(",\r\n");
        var bones = (Dictionary<string, object>)e["bones"];
        sb.Append("      \"bones\": { \"right\": ").Append(JsonMini.Str((string)bones["right"]))
          .Append(", \"left\": ").Append(JsonMini.Str((string)bones["left"]))
          .Append(", \"allow_fallback_candidates\": true }");
        if (e.ContainsKey("axis")) {
            var axis = (Dictionary<string, object>)e["axis"];
            sb.Append(",\r\n      \"axis\": { \"name\": ").Append(JsonMini.Str((string)axis["name"]))
              .Append(", \"sign\": ").Append(JsonMini.Num((double)axis["sign"])).Append(" }");
        }
        if (e.TryGetValue("animation_rules", out object? rulesObj) &&
            rulesObj is Dictionary<string, object> rules) {
            sb.Append(",\r\n      \"animation_rules\": { ");
            bool firstRule = true;
            foreach (var rule in rules) {
                if (rule.Value is not string gaitName) continue;
                if (!firstRule) sb.Append(", ");
                sb.Append(JsonMini.Str(rule.Key)).Append(": ").Append(JsonMini.Str(gaitName));
                firstRule = false;
            }
            sb.Append(" }");
        }
        var d = (Dictionary<string, object>)e["defaults"];
        var gait = (Dictionary<string, object>)d["gait"];
        sb.Append(",\r\n      \"defaults\": {\r\n        \"gait\": {\r\n");
        string[] gnames2 = { "idle", "walk", "run", "sprint", "zipline" };
        // DB defaults are intentionally partial: the runtime tolerates a
        // missing gait sub-block (e.g. zipline) by falling back to defaults,
        // and the Manager reader skips absent keys.  Mirror that here so a
        // save/rewrite never throws "The given key 'zipline' was not
        // present in the dictionary."  Emit only the gait entries present.
        bool wroteGait = false;
        for (int i = 0; i < 5; i++) {
            if (!gait.TryGetValue(gnames2[i], out object? gObj) ||
                !(gObj is Dictionary<string, object> g)) continue;
            if (wroteGait) sb.Append(",");
            sb.Append(wroteGait ? "\r\n          " : "          ")
              .Append(CharacterDatabaseService.GaitEntryJson(
                gnames2[i],
                JsonMini.GetNum(g, "amplitude_deg", 0),
                JsonMini.GetNum(g, "amplitude_down_deg", 0),
                JsonMini.GetNum(g, "frequency_hz", 1.5),
                JsonMini.GetNum(g, "phase_offset_deg", 0),
                JsonMini.GetBool(g, "phase_align", true),
                JsonMini.GetBool(g, "auto_frequency", true),
                JsonMini.GetNum(g, "freq_dev_threshold", 0.05)));
            wroteGait = true;
        }
        sb.Append("\r\n");
        var env = (Dictionary<string, object>)d["envelope"];
        sb.Append("        },\r\n        \"envelope\": {\r\n")
          .Append("          \"amplitude_attack_tau_sec\": ").Append(JsonMini.Num((double)env["amplitude_attack_tau_sec"])).Append(",\r\n")
          .Append("          \"frequency_tau_sec\": ").Append(JsonMini.Num((double)env["frequency_tau_sec"])).Append(",\r\n")
          .Append("          \"to_idle_release_tau_sec\": ").Append(JsonMini.Num((double)env["to_idle_release_tau_sec"])).Append("\r\n")
          .Append("        },\r\n");
        var jump = (Dictionary<string, object>)d["jump"];
        var jumpData = new CharacterData {
            JumpEnabled = JsonMini.GetBool(jump, "enabled", false),
            JumpAmplitude = JsonMini.GetNum(jump, "amplitude_deg", 23.333),
            JumpDampingTau = JsonMini.GetNum(jump, "damping_tau_sec", 0.35),
            JumpFrequency = JsonMini.GetNum(jump, "frequency_hz", 3.0),
            JumpMaxDuration = JsonMini.GetNum(jump, "max_duration_sec", 1.20),
            JumpTakeoffDelay = JsonMini.GetNum(jump, "takeoff_delay_sec", 0.08),
            JumpRisingTarget = JsonMini.GetNum(jump, "rising_target_deg", -23.333),
            JumpApexFallingTarget = JsonMini.GetNum(jump, "apex_falling_target_deg", 23.333),
            JumpAccelerationResponse = JsonMini.GetNum(jump, "acceleration_response", 0.1667),
            JumpAccelerationFilterTau = JsonMini.GetNum(jump, "acceleration_filter_tau_sec", 0.08),
            JumpNaturalFrequency = JsonMini.GetNum(jump, "natural_frequency_hz", 2.2),
            JumpDampingRatio = JsonMini.GetNum(jump, "damping_ratio", 0.52),
            JumpLandingImpulseGain = JsonMini.GetNum(jump, "landing_impulse_gain", 6.667)
        };
        sb.Append("        \"jump\": ").Append(JumpEntryJson(jumpData))
          .Append(",").Append(Environment.NewLine);
        var na = (Dictionary<string, object>)d["native_amplify"];
        sb.Append("        \"native_amplify\": { \"factor\": ").Append(JsonMini.Num((double)na["factor"])).Append(" }\r\n")
          .Append("      }\r\n    }");
        return sb.ToString();
    }
}
