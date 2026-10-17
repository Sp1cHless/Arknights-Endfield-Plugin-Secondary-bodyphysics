using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace SecondaryMotion.Manager.Services;

// Each newly shipped official id is considered once. Missing DB/preset
// objects receive the complete packaged defaults; existing objects belong
// to the user. Remembering ids also respects deletions after migration.
public static class OfficialCharactersMigration {
    const string MarkerName = ".official_characters_v1.json";
    static readonly JsonSerializerOptions Options = new(JsonSerializerOptions.Default) {
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping
    };

    public static string Apply(string managerDir, string dataRoot) {
        try {
            string marker = Path.Combine(dataRoot, "data", MarkerName);
            var seen = new HashSet<string>(StringComparer.Ordinal);
            if (File.Exists(marker)) {
                var saved = JsonNode.Parse(File.ReadAllText(marker)) as JsonObject;
                if (saved?["processed_ids"] is not JsonArray ids)
                    return "invalid migration marker";
                foreach (var id in ids) seen.Add(id!.GetValue<string>());
            }
            string dbTemplate = Template(managerDir, "data", "characters.default");
            string presetTemplate = Template(managerDir, "presets", "Default");
            if (dbTemplate.Length == 0 || presetTemplate.Length == 0)
                return "official templates missing";
            var officialDb = Read(dbTemplate)["characters"] as JsonObject;
            var officialPreset = Read(presetTemplate)["characters"] as JsonObject;
            if (officialDb == null || officialPreset == null)
                return "official templates invalid";
            var pending = officialDb.Select(pair => pair.Key)
                .Where(id => !seen.Contains(id)).ToArray();
            if (pending.Length == 0) return "already applied";
            foreach (string id in pending)
                if (officialDb[id] is not JsonObject || officialPreset[id] is not JsonObject)
                    return "official profile incomplete: " + id;

            string dbPath = Path.Combine(dataRoot, "data", "characters.default.json");
            string presetsDir = Path.Combine(dataRoot, "presets");
            if (!Directory.Exists(presetsDir)) return "presets directory missing";
            // Parse all destinations first: malformed user data must not
            // result in partial writes or a completed migration marker.
            var destinations = new List<(string Path, JsonObject Root, JsonObject Seeds)> {
                (dbPath, Read(dbPath), officialDb)
            };
            foreach (string path in Directory.GetFiles(presetsDir, "*.json"))
                if (PresetService.IsValidPresetName(Path.GetFileNameWithoutExtension(path)))
                    destinations.Add((path, Read(path), officialPreset));
            foreach (var destination in destinations)
                if (destination.Root["characters"] is not JsonObject)
                    return "characters map missing: " + Path.GetFileName(destination.Path);

            int added = 0;
            foreach (var destination in destinations) {
                var characters = (JsonObject)destination.Root["characters"]!;
                bool changed = false;
                foreach (string id in pending) {
                    if (characters.ContainsKey(id)) continue;
                    characters[id] = destination.Seeds[id]!.DeepClone();
                    changed = true;
                    added++;
                }
                if (changed) BackupAndWrite(destination.Path, destination.Root);
            }
            foreach (string id in pending) seen.Add(id);
            var processed = new JsonArray();
            foreach (string id in seen.OrderBy(id => id, StringComparer.Ordinal)) processed.Add(id);
            var record = new JsonObject {
                ["schema_version"] = 1, ["processed_ids"] = processed
            };
            PresetService.AtomicWrite(marker, record.ToJsonString(Options) + Environment.NewLine);
            return "applied: ids=" + pending.Length + " objects=" + added;
        } catch (Exception ex) {
            return "failed: " + ex.Message;
        }
    }

    static string Template(string directory, string subdirectory, string name) {
        string packaged = Path.Combine(directory, subdirectory, name + ".template.json");
        if (File.Exists(packaged)) return packaged;
        string development = Path.Combine(directory, subdirectory, name + ".json");
        return File.Exists(development) ? development : "";
    }

    static JsonObject Read(string path) =>
        JsonNode.Parse(File.ReadAllText(path)) as JsonObject ??
        throw new InvalidDataException("Invalid JSON object: " + Path.GetFileName(path));

    static void BackupAndWrite(string path, JsonObject root) {
        string baseName = path + ".pre_official_characters_v1";
        string backup = baseName;
        for (int i = 2; File.Exists(backup); i++) backup = baseName + "." + i;
        File.Copy(path, backup);
        PresetService.AtomicWrite(path, root.ToJsonString(Options) + Environment.NewLine);
    }
}
