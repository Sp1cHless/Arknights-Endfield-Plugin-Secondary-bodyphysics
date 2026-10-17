// Services/PresetService.cs — presets/<name>.json read/write + CRUD.
// A preset stores the FULL current state of every character (enabled, mode,
// params); bones/axis come from the character DB at runtime.
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using SecondaryMotion.Manager.Models;

namespace SecondaryMotion.Manager.Services;

public class PresetService {
    readonly string _presetsDir;   // PRIMARY: tool folder (movable/backup-able)
    readonly string _mirrorDir;    // MIRROR: game data dir (runtime reads here)

    public PresetService(string dataRoot, string managerDir) {
        _presetsDir = Path.Combine(managerDir, "presets");
        _mirrorDir = Path.Combine(dataRoot, "presets");
        // migration: user data lives in the game dir; copy anything newer
        // into the tool folder (template files there are older).  After the
        // first sync both sides match and this stops firing.
        try {
            if (Directory.Exists(_mirrorDir)) {
                Directory.CreateDirectory(_presetsDir);
                foreach (var f in Directory.GetFiles(_mirrorDir, "*.json")) {
                    string dst = Path.Combine(_presetsDir, Path.GetFileName(f));
                    if (!File.Exists(dst) ||
                        File.GetLastWriteTime(f) > File.GetLastWriteTime(dst))
                        File.Copy(f, dst, true);
                }
            }
        } catch { }
    }

    public string PathOf(string name) => Path.Combine(_presetsDir, name + ".json");

    // Keep Manager-visible names aligned with Runtime path-safety rules.
    // In particular, packaged *.template.json files are seed data, not
    // selectable presets.
    public static bool IsValidPresetName(string name) {
        if (string.IsNullOrWhiteSpace(name) || name != name.Trim()) return false;
        if (name.IndexOfAny(new[] { '.', '/', '\\', ':' }) >= 0) return false;
        return name.IndexOfAny(Path.GetInvalidFileNameChars()) < 0;
    }

    public bool IsSelectablePreset(string name) =>
        IsValidPresetName(name) && File.Exists(PathOf(name));

    public List<string> ListPresets() {
        var names = new List<string>();
        if (!Directory.Exists(_presetsDir)) return names;
        foreach (var f in Directory.GetFiles(_presetsDir, "*.json")) {
            string name = Path.GetFileNameWithoutExtension(f);
            if (IsValidPresetName(name)) names.Add(name);
        }
        names.Sort(StringComparer.OrdinalIgnoreCase);
        return names;
    }

    // Merge preset overrides into `chars` (in place).  keepPresetOnly=true
    // keeps characters that exist in the preset but not in the DB.
    public void LoadInto(string name, List<CharacterData> chars,
                         Dictionary<string, CharacterData> byId, bool keepPresetOnly) {
        string p = PathOf(name);
        if (!File.Exists(p)) return;
        var map = new Dictionary<string, Dictionary<string, object>>();
        if (!JsonMini.ParseObjectMap(File.ReadAllText(p, Encoding.UTF8), "characters", map)) return;
        foreach (var kv in map) {
            CharacterData c;
            if (!byId.TryGetValue(kv.Key, out c)) {
                if (!keepPresetOnly) continue;
                c = new CharacterData { Id = kv.Key, DisplayName = kv.Key };
                chars.Add(c);
                byId[c.Id] = c;
            }
            var node = kv.Value;
            c.Enabled = JsonMini.GetBool(node, "enabled", c.Enabled);
            string mode = JsonMini.GetStr(node, "motion_mode", "");
            if (mode.Length > 0) c.Mode = mode;
            object o;
            if (node.TryGetValue("gait", out o) && o is Dictionary<string, object> gait)
                CharacterDatabaseService.ApplyGait(gait, c);
            if (node.TryGetValue("envelope", out o) && o is Dictionary<string, object> env) {
                c.EnvAttack = JsonMini.GetNum(env, "amplitude_attack_tau_sec", c.EnvAttack);
                c.EnvFreq = JsonMini.GetNum(env, "frequency_tau_sec", c.EnvFreq);
                c.EnvIdle = JsonMini.GetNum(env, "to_idle_release_tau_sec", c.EnvIdle);
            }
            if (node.TryGetValue("jump", out o) && o is Dictionary<string, object> jump)
                CharacterDatabaseService.ApplyJump(jump, c);
            if (node.TryGetValue("native_amplify", out o) && o is Dictionary<string, object> na)
                c.NativeFactor = JsonMini.GetNum(na, "factor", c.NativeFactor);
            if (node.TryGetValue("axis", out o) && o is Dictionary<string, object> axis) {
                c.Axis = JsonMini.GetStr(axis, "name", c.Axis);
                c.AxisSign = JsonMini.GetNum(axis, "sign", 1.0) < 0 ? -1 : 1;
            }
        }
    }

    // Full preset write (all characters, current state) — atomic.
    public void Write(string name, List<CharacterData> chars) {
        var sb = new StringBuilder();
        sb.Append("{\r\n  \"schema_version\": 1,\r\n  \"name\": ").Append(JsonMini.Str(name))
          .Append(",\r\n  \"characters\": {\r\n");
        for (int i = 0; i < chars.Count; i++) {
            var c = chars[i];
            sb.Append("    ").Append(JsonMini.Str(c.Id)).Append(": {\r\n");
            sb.Append("      \"enabled\": ").Append(c.Enabled ? "true" : "false").Append(",\r\n");
            sb.Append("      \"motion_mode\": ").Append(JsonMini.Str(c.Mode)).Append(",").Append(Environment.NewLine);
            if (c.Axis.Length > 0)
                sb.Append("      \"axis\": { \"name\": ").Append(JsonMini.Str(c.Axis))
                  .Append(", \"sign\": ").Append(c.AxisSign < 0 ? "-1" : "1").Append(" },\r\n");
            sb.Append("      \"gait\": {\r\n");
            string[] gnames = { "idle", "walk", "run", "sprint", "zipline" };
            for (int g = 0; g < 5; g++)
                sb.Append("        ")
                  .Append(CharacterDatabaseService.GaitEntryJson(gnames[g], c.Amp[g], c.AmpDown[g], c.Freq[g], c.PhaseOffset[g],
                                                                 c.PhaseAlign[g], c.AutoFreq[g], c.FreqDevThreshold[g]))
                  .Append(g < 4 ? "," : "").Append("\r\n");
            sb.Append("      },\r\n");
            sb.Append("      \"envelope\": {\r\n")
              .Append("        \"amplitude_attack_tau_sec\": ").Append(JsonMini.Num(c.EnvAttack)).Append(",\r\n")
              .Append("        \"frequency_tau_sec\": ").Append(JsonMini.Num(c.EnvFreq)).Append(",\r\n")
              .Append("        \"to_idle_release_tau_sec\": ").Append(JsonMini.Num(c.EnvIdle)).Append("\r\n")
              .Append("      },\r\n");
            sb.Append("      \"jump\": ")
              .Append(CharacterDatabaseService.JumpEntryJson(c))
              .Append(",").Append(Environment.NewLine);
            sb.Append("      \"native_amplify\": { \"factor\": ").Append(JsonMini.Num(c.NativeFactor)).Append(" }\r\n");
            sb.Append("    }").Append(i < chars.Count - 1 ? "," : "").Append("\r\n");
        }
        sb.Append("  }\r\n}\r\n");
        AtomicWrite(PathOf(name), sb.ToString());
    }

    // Mirror one preset file from the tool folder to the game dir.
    public void SyncMirror(string name) {
        try {
            Directory.CreateDirectory(_mirrorDir);
            string s = PathOf(name);
            if (File.Exists(s))
                File.Copy(s, Path.Combine(_mirrorDir, name + ".json"), true);
        } catch { }
    }

    public void Delete(string name) {
        string p = PathOf(name);
        if (File.Exists(p)) File.Delete(p);
        try {
            var m = Path.Combine(_mirrorDir, name + ".json");
            if (File.Exists(m)) File.Delete(m);
        } catch { }
    }

    // Remove one character's override entry from a preset file.  The
    // character then falls back to its DB defaults on next load.
    public void RemoveOverride(string name, string characterId) {
        string p = PathOf(name);
        if (!File.Exists(p)) return;
        var chars = new List<CharacterData>();
        var byId = new Dictionary<string, CharacterData>();
        LoadInto(name, chars, byId, true);
        if (!byId.ContainsKey(characterId)) return;
        byId.Remove(characterId);
        chars.RemoveAll(c => c.Id == characterId);
        Write(name, chars);
    }

    public static void AtomicWrite(string path, string content) {
        string tmp = path + ".tmp";
        File.WriteAllText(tmp, content, new UTF8Encoding(false));
        if (File.Exists(path)) File.Delete(path);
        File.Move(tmp, path);
    }
}
