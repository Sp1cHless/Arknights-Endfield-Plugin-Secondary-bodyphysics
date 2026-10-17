using System;
using System.IO;
using System.Text.Json.Nodes;
using SecondaryMotion.Manager.Services;

string repository = Path.GetFullPath(args[0]);
string output = Path.Combine(Path.GetFullPath(args[1]), Guid.NewGuid().ToString("N"));
Directory.CreateDirectory(output);
Console.WriteLine("Artifacts: " + output);
const string Si = "chr_0036_jsspsi";
var database = Read(Path.Combine(repository, "SecondaryMotion/data/characters.default.json"));
var preset = Read(Path.Combine(repository, "SecondaryMotion/presets/Default.json"));
int passed = 0;

void Check(string name, bool condition) {
    if (!condition) throw new Exception("FAIL " + name);
    passed++;
    Console.WriteLine("PASS " + name);
}
JsonObject Read(string path) => (JsonObject)JsonNode.Parse(File.ReadAllText(path))!;
void Write(string path, JsonObject value) {
    Directory.CreateDirectory(Path.GetDirectoryName(path)!);
    File.WriteAllText(path, value.ToJsonString());
}
JsonObject Chars(JsonObject root) => (JsonObject)root["characters"]!;
(string Tool, string Game) Setup(string name, bool customized = false, bool development = false, bool legacyMarkers = true) {
    string root = Path.Combine(output, name);
    string tool = Path.Combine(root, "tool"), game = Path.Combine(root, "game");
    string suffix = development ? ".json" : ".template.json";
    Write(Path.Combine(tool, "data/characters.default" + suffix), (JsonObject)database.DeepClone());
    Write(Path.Combine(tool, "presets/Default" + suffix), (JsonObject)preset.DeepClone());
    var db = (JsonObject)database.DeepClone();
    if (customized) Chars(db)[Si] = new JsonObject { ["display_name"] = "My Si", ["user_field"] = 123 };
    else Chars(db).Remove(Si);
    Write(Path.Combine(game, "data/characters.default.json"), db);
    foreach (string file in new[] { "Default", "User", "Custom" }) {
        var node = (JsonObject)preset.DeepClone();
        if (customized) Chars(node)[Si] = new JsonObject { ["enabled"] = false, ["my_tuning"] = 987 };
        else Chars(node).Remove(Si);
        Chars(node)["user_character"] = new JsonObject { ["keep"] = true };
        Write(Path.Combine(game, "presets/" + file + ".json"), node);
    }
    if (legacyMarkers) {
        File.WriteAllText(Path.Combine(game, "data/.jump_defaults_v3"), "previously migrated");
        File.WriteAllText(Path.Combine(game, "data/.typhoeus_official_v1"), "previously migrated");
    }
    File.WriteAllText(Path.Combine(game, "presets/Ignore.template.json"), "not a working preset");
    return (tool, game);
}

Check("Si has complete packaged technical and motion data",
    Chars(database)[Si] is JsonObject && Chars(preset)[Si] is JsonObject &&
    JsonNode.DeepEquals(Chars(database)[Si]!["defaults"]!["gait"], Chars(preset)[Si]!["gait"]) &&
    JsonNode.DeepEquals(Chars(database)[Si]!["defaults"]!["jump"], Chars(preset)[Si]!["jump"]));
var missing = Setup("missing");
var oldDb = Read(Path.Combine(missing.Game, "data/characters.default.json"));
var oldPreset = Read(Path.Combine(missing.Game, "presets/Custom.json"));
string result = OfficialCharactersMigration.Apply(missing.Tool, missing.Game);
Console.WriteLine(result);
Check("old installation receives a missing Si despite legacy markers", result.StartsWith("applied:"));
var afterDb = Read(Path.Combine(missing.Game, "data/characters.default.json"));
Check("technical profile matches the complete shipped object", JsonNode.DeepEquals(Chars(afterDb)[Si], Chars(database)[Si]));
foreach (string name in new[] { "Default", "User", "Custom" }) {
    var after = Read(Path.Combine(missing.Game, "presets/" + name + ".json"));
    Check(name + " gets complete default Si tuning", JsonNode.DeepEquals(Chars(after)[Si], Chars(preset)[Si]));
    Chars(after).Remove(Si);
    Check(name + " preserves every other profile", JsonNode.DeepEquals(after, oldPreset));
}
Chars(afterDb).Remove(Si);
Check("database preserves every other technical profile", JsonNode.DeepEquals(afterDb, oldDb));
Check("template-like filenames are excluded", File.ReadAllText(Path.Combine(missing.Game, "presets/Ignore.template.json")) == "not a working preset");
string hashBefore = File.ReadAllText(Path.Combine(missing.Game, "presets/User.json"));
Check("repeat startup makes no changes", OfficialCharactersMigration.Apply(missing.Tool, missing.Game) == "already applied" &&
    File.ReadAllText(Path.Combine(missing.Game, "presets/User.json")) == hashBefore);
var removed = Read(Path.Combine(missing.Game, "presets/User.json"));
Chars(removed).Remove(Si);
Write(Path.Combine(missing.Game, "presets/User.json"), removed);
OfficialCharactersMigration.Apply(missing.Tool, missing.Game);
Check("user deletion after migration remains deleted", !Chars(Read(Path.Combine(missing.Game, "presets/User.json"))).ContainsKey(Si));
var nextDb = (JsonObject)database.DeepClone();
var nextPreset = (JsonObject)preset.DeepClone();
Chars(nextDb)["chr_future"] = Chars(database)[Si]!.DeepClone();
Chars(nextPreset)["chr_future"] = Chars(preset)[Si]!.DeepClone();
Write(Path.Combine(missing.Tool, "data/characters.default.template.json"), nextDb);
Write(Path.Combine(missing.Tool, "presets/Default.template.json"), nextPreset);
OfficialCharactersMigration.Apply(missing.Tool, missing.Game);
var updatedUser = Chars(Read(Path.Combine(missing.Game, "presets/User.json")));
Check("future release automatically seeds a new official id", updatedUser.ContainsKey("chr_future"));
Check("future update does not undo previous user deletion", !updatedUser.ContainsKey(Si));

var existing = Setup("existing", true);
string existingDb = File.ReadAllText(Path.Combine(existing.Game, "data/characters.default.json"));
string existingPreset = File.ReadAllText(Path.Combine(existing.Game, "presets/Default.json"));
OfficialCharactersMigration.Apply(existing.Tool, existing.Game);
Check("existing custom Si database is byte-identical", File.ReadAllText(Path.Combine(existing.Game, "data/characters.default.json")) == existingDb);
Check("existing custom Si preset is byte-identical", File.ReadAllText(Path.Combine(existing.Game, "presets/Default.json")) == existingPreset);
var dbMissing = Setup("database-only-missing", true);
var dbMissingRoot = Read(Path.Combine(dbMissing.Game, "data/characters.default.json"));
Chars(dbMissingRoot).Remove(Si);
Write(Path.Combine(dbMissing.Game, "data/characters.default.json"), dbMissingRoot);
string retainedPreset = File.ReadAllText(Path.Combine(dbMissing.Game, "presets/Default.json"));
OfficialCharactersMigration.Apply(dbMissing.Tool, dbMissing.Game);
Check("missing DB profile is filled independently", JsonNode.DeepEquals(
    Chars(Read(Path.Combine(dbMissing.Game, "data/characters.default.json")))[Si], Chars(database)[Si]));
Check("DB-only recovery preserves existing Si tuning", File.ReadAllText(Path.Combine(dbMissing.Game, "presets/Default.json")) == retainedPreset);
var presetMissing = Setup("preset-only-missing", true);
var presetMissingRoot = Read(Path.Combine(presetMissing.Game, "presets/Default.json"));
Chars(presetMissingRoot).Remove(Si);
Write(Path.Combine(presetMissing.Game, "presets/Default.json"), presetMissingRoot);
string retainedDb = File.ReadAllText(Path.Combine(presetMissing.Game, "data/characters.default.json"));
OfficialCharactersMigration.Apply(presetMissing.Tool, presetMissing.Game);
Check("missing preset profile is filled independently", JsonNode.DeepEquals(
    Chars(Read(Path.Combine(presetMissing.Game, "presets/Default.json")))[Si], Chars(preset)[Si]));
Check("preset-only recovery preserves existing Si technical data", File.ReadAllText(Path.Combine(presetMissing.Game, "data/characters.default.json")) == retainedDb);
var development = Setup("development", development: true);
Check("development source fallback works", OfficialCharactersMigration.Apply(development.Tool, development.Game).StartsWith("applied:"));
var invalid = Setup("invalid");
File.WriteAllText(Path.Combine(invalid.Game, "presets/Custom.json"), "malformed");
string invalidDb = File.ReadAllText(Path.Combine(invalid.Game, "data/characters.default.json"));
string invalidPreset = File.ReadAllText(Path.Combine(invalid.Game, "presets/Default.json"));
Check("invalid destination reports failure", OfficialCharactersMigration.Apply(invalid.Tool, invalid.Game).StartsWith("failed:"));
Check("invalid destination causes no partial database or preset writes",
    File.ReadAllText(Path.Combine(invalid.Game, "data/characters.default.json")) == invalidDb &&
    File.ReadAllText(Path.Combine(invalid.Game, "presets/Default.json")) == invalidPreset);
Check("failed migration does not record completion", !File.Exists(Path.Combine(invalid.Game, "data/.official_characters_v1.json")));

// Exercise the real startup order, including old users who never ran the
// legacy migrations, then load through the same mirroring services as AppCtx.
var firstUpdate = Setup("full-startup-chain", legacyMarkers: false);
File.Copy(Path.Combine(repository, "SecondaryMotion/presets/Default.v3.0.0-bad.template.json"),
    Path.Combine(firstUpdate.Tool, "presets/Default.v3.0.0-bad.template.json"));
Check("startup Typhoeus migration succeeds", TyphoeusOfficialMigration.Apply(firstUpdate.Tool, firstUpdate.Game).StartsWith("applied:"));
Check("startup official-character migration succeeds", OfficialCharactersMigration.Apply(firstUpdate.Tool, firstUpdate.Game).StartsWith("applied:"));
Check("startup legacy Jump migration succeeds", JumpDefaultsMigration.Apply(firstUpdate.Tool, firstUpdate.Game).StartsWith("applied:"));
Check("startup legacy hotfix succeeds", JumpDefaultsV301Hotfix.Apply(firstUpdate.Tool, firstUpdate.Game).StartsWith("applied:"));
Check("complete Si survives the entire startup chain", JsonNode.DeepEquals(
    Chars(Read(Path.Combine(firstUpdate.Game, "presets/Default.json")))[Si], Chars(preset)[Si]));
var dbService = new CharacterDatabaseService(firstUpdate.Game, firstUpdate.Tool);
var models = dbService.Load();
var byId = models.ToDictionary(model => model.Id);
var presetsService = new PresetService(firstUpdate.Game, firstUpdate.Tool);
presetsService.LoadInto("Default", models, byId, false);
Check("Manager model loads the migrated Si with bones and axis", dbService.Loaded && byId.TryGetValue(Si, out var si) &&
    si.BoneRight == "R_breast_01_jnt" && si.BoneLeft == "L_breast_01_jnt" && si.Axis == "Z" && si.AxisSign == 1);
Check("Manager model retains the shipped Si motion parameters", byId[Si].Freq[1] == 1.7 &&
    byId[Si].Freq[2] == 2.7 && byId[Si].Freq[3] == 3.4 && byId[Si].Amp[3] == 45 &&
    byId[Si].JumpAmplitude == 40 && byId[Si].EnvIdle == 0.7);
Console.WriteLine($"RESULT: {passed} passed");
