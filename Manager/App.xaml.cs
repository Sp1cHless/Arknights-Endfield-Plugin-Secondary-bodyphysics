using System;
using System.IO;
using System.Text;
using System.Windows;
using Microsoft.Win32;
using SecondaryMotion.Manager.Services;

namespace SecondaryMotion.Manager;

public partial class App : Application {
    public static AppCtx Ctx = null!;
    public static string ManagerDir = "";
    public static string CurrentLang = "en-US";

    // Merge the language string dictionary into Application resources.
    static void LoadStrings(string lang) {
        var dict = new ResourceDictionary {
            Source = new Uri("Localization/Strings." + lang + ".xaml", UriKind.Relative)
        };
        Current.Resources.MergedDictionaries.Add(dict);
    }

    // Runtime language switch: swap the strings dictionary, persist the
    // choice, let DynamicResource bindings update live.
    public static void SwitchLanguage(string lang) {
        if (lang == CurrentLang) return;
        CurrentLang = lang;
        var res = Current.Resources;
        for (int i = res.MergedDictionaries.Count - 1; i >= 0; i--)
            if (res.MergedDictionaries[i].Source != null &&
                res.MergedDictionaries[i].Source.OriginalString.Contains("Localization/Strings."))
                res.MergedDictionaries.RemoveAt(i);
        LoadStrings(lang);
        try {
            new SettingsService(ManagerDir).SaveLanguage(lang);
        } catch { }
    }

    protected override void OnStartup(StartupEventArgs e) {
        base.OnStartup(e);
        try {
            ManagerDir = AppDomain.CurrentDomain.BaseDirectory;
            var settings = new SettingsService(ManagerDir);
            // language: explicit setting wins, then the package's
            // default_lang.txt (set by the build), then follow the system
            // display language (zh -> Simplified Chinese, anything else -> EN)
            CurrentLang = settings.Language;
            if (CurrentLang.Length == 0) {
                try {
                    var dl = Path.Combine(ManagerDir, "default_lang.txt");
                    if (File.Exists(dl))
                        CurrentLang = File.ReadAllText(dl).Trim();
                } catch { }
            }
            if (CurrentLang.Length == 0)
                CurrentLang = System.Globalization.CultureInfo.InstalledUICulture
                    .TwoLetterISOLanguageName == "zh" ? "zh-CN" : "en-US";
            LoadStrings(CurrentLang);
            var diag = new StringBuilder();
            diag.AppendLine("manager_dir: " + ManagerDir);
            diag.AppendLine("settings.game_data_dir: " + (settings.GameDataDir ?? "(none)"));
            diag.AppendLine("language: " + CurrentLang);

            // Data root: settings.game_data_dir (recommended — Manager lives
            // in its own folder).  When the setting is missing/invalid the
            // FIRST-RUN flow starts: pick the game folder and auto-install.
            // (No fallback to the Manager folder — it carries only release
            // templates, not working data; the old V1 self-contained-in-game
            // layout is obsolete.)
            string dataRoot;
            if (settings.GameDataDir != null &&
                Directory.Exists(Path.Combine(settings.GameDataDir, "data"))) {
                dataRoot = settings.GameDataDir;
            } else {
                // FIRST RUN: ask for the GAME ROOT folder, then auto-install:
                // create SecondaryMotion\ + data dirs, copy template files,
                // and deploy the plugin dll into the game's plugin\ folder.
                var dlg = new OpenFolderDialog {
                    Title = "Select the game folder (the one containing Endfield.exe, " +
                            "e.g. ...\\Endfield Game)"
                };
                if (dlg.ShowDialog() != true) {
                    MessageBox.Show(L10n.Get("Msg_GameFolderRequired"),
                                    "Secondary Motion", MessageBoxButton.OK,
                                    MessageBoxImage.Warning);
                    Shutdown(1);
                    return;
                }
                string gameRoot = dlg.FolderName;
                var setupMsg = FirstRunSetup(gameRoot);
                if (setupMsg != null) {
                    MessageBox.Show(setupMsg, L10n.Get("Msg_SetupFailedTitle"),
                                    MessageBoxButton.OK, MessageBoxImage.Error);
                    Shutdown(1);
                    return;
                }
                dataRoot = Path.Combine(gameRoot, "SecondaryMotion");
                settings.Save(dataRoot, CurrentLang);
                MessageBox.Show(
                    L10n.Get("Msg_Installed", dataRoot, Path.Combine(gameRoot, "plugin")),
                    "Secondary Motion", MessageBoxButton.OK, MessageBoxImage.Information);
            }
            diag.AppendLine("data_root: " + dataRoot);
            // keep game-root proxy loaders in sync on every start (updates
            // skip the first-run install flow, so this is the only hook).
            try {
                var gameRootDir = Path.GetDirectoryName(dataRoot) ?? "";
                string[] proxyPairs = {
                    "d3dcompiler_47.dll", "d3dcompiler_47.dll",
                    "vulkan-1.dll", "vulkan-1.dll",
                };
                for (int i = 0; i < proxyPairs.Length; i += 2) {
                    var psrc = Path.Combine(ManagerDir, "plugin", proxyPairs[i]);
                    var pdst = Path.Combine(gameRootDir, proxyPairs[i + 1]);
                    if (File.Exists(psrc) &&
                        (!File.Exists(pdst) || !FilesEqual(psrc, pdst))) {
                        if (File.Exists(pdst)) File.Copy(pdst, pdst + ".bak", true);
                        File.Copy(psrc, pdst, true);
                    }
                }
                diag.AppendLine("proxy_sync: done");
            } catch (Exception ex) {
                diag.AppendLine("proxy_sync: " + ex.Message);
            }
            var stPath = Path.Combine(dataRoot, "runtime", "runtime_status.json");
            diag.AppendLine("status_path: " + stPath);
            diag.AppendLine("status_exists: " + File.Exists(stPath));
            if (File.Exists(stPath))
                diag.AppendLine("status_age_s: " +
                    (DateTime.Now - File.GetLastWriteTime(stPath)).TotalSeconds.ToString("0.0"));

            // Add the official Typhoeus profile to older installations once.
            // Existing user-recorded DB/preset objects are never replaced.
            diag.AppendLine("typhoeus_official_v1: " +
                TyphoeusOfficialMigration.Apply(ManagerDir, dataRoot));
            // New official ids (including Si) are added once without replacing
            // user profiles. Run before legacy Jump migration can create a
            // Jump-only preset entry, and before loading the mirrored model.
            diag.AppendLine("official_characters_v1: " +
                OfficialCharactersMigration.Apply(ManagerDir, dataRoot));
            // v3 update: replace only the official Jump blocks once. Existing
            // gait tuning, bones, names, custom characters and other data stay.
            diag.AppendLine("jump_defaults_v3: " +
                JumpDefaultsMigration.Apply(ManagerDir, dataRoot));
            // v3.0.1 only: correct the accidentally packaged intermediate
            // Default Jump table, but only for untouched exact 3.0.0 values.
            diag.AppendLine("jump_defaults_v3_0_1_hotfix: " +
                JumpDefaultsV301Hotfix.Apply(ManagerDir, dataRoot));

            Ctx = new AppCtx(dataRoot, ManagerDir);
            Ctx.Load();
            ChangeLog.Init(ManagerDir);
            ChangeLog.Append("[Startup] data_root=" + dataRoot +
                             " preset=" + Ctx.ActivePreset +
                             " chars=" + Ctx.Characters.Count);
            diag.AppendLine("chars_loaded: " + Ctx.Characters.Count);
            diag.AppendLine("preset: " + Ctx.ActivePreset + " rev=" + Ctx.Config.Revision);
            File.WriteAllText(Path.Combine(ManagerDir, "manager_startup.log"), diag.ToString());

            var win = new MainWindow();
            MainWindow = win;
            win.Show();
        } catch (Exception ex) {
            try {
                File.WriteAllText(Path.Combine(ManagerDir, "manager_crash.log"), ex.ToString());
            } catch { }
            throw;
        }
    }

    // First-run auto-install.  Returns an error message on failure, null on
    // success.  Dumb and explicit — no cleverness.
    static string? FirstRunSetup(string gameRoot) {
        try {
            // sanity: the picked folder should be the game root.  Note the
            // game ships a Qt "plugins\" (plural) folder — our mod folder is
            // "plugin\" (singular, Unity loads it) and may not exist yet on
            // a fresh install, so we create it below.
            string pluginDir = Path.Combine(gameRoot, "plugin");
            bool looksLikeGame =
                Directory.Exists(Path.Combine(gameRoot, "plugins")) ||
                Directory.Exists(Path.Combine(gameRoot, "Plugins")) ||
                File.Exists(Path.Combine(gameRoot, "Endfield.exe")) ||
                File.Exists(Path.Combine(gameRoot, "UnityPlayer.dll"));
            if (!looksLikeGame) {
                return L10n.Get("Msg_SetupNotGameFolder");
            }
            Directory.CreateDirectory(pluginDir);

            // 1. data skeleton under <gameRoot>\SecondaryMotion\
            string sm = Path.Combine(gameRoot, "SecondaryMotion");
            foreach (var sub in new[] { "data", "presets", "runtime", "developer", "logs" })
                Directory.CreateDirectory(Path.Combine(sm, sub));

            // 2. template files (only if missing — never clobber user data)
            CopyIfMissing(Path.Combine(ManagerDir, "data", "characters.default.template.json"),
                          Path.Combine(sm, "data", "characters.default.json"));
            CopyIfMissing(Path.Combine(ManagerDir, "presets", "Default.template.json"),
                          Path.Combine(sm, "presets", "Default.json"));
            CopyIfMissing(Path.Combine(ManagerDir, "presets", "User.template.json"),
                          Path.Combine(sm, "presets", "User.json"));
            CopyIfMissing(Path.Combine(ManagerDir, "runtime", "config.json"),
                          Path.Combine(sm, "runtime", "config.json"));

            // 3. plugin dlls (release layout: plugin\ beside the exe).
            // sbm.dll is the runtime itself; d3dcompiler_47.dll / vulkan-1.dll
            // are PROXY loaders that must sit in the GAME ROOT (the game
            // loads them instead of the system originals, then they load
            // plugin\*.dll).  DLLs are code - always update, keep .bak.
            var srcDll = Path.Combine(ManagerDir, "plugin", "sbm.dll");
            var dstDll = Path.Combine(pluginDir, "sbm.dll");
            if (File.Exists(srcDll)) {
                if (File.Exists(dstDll) && !FilesEqual(srcDll, dstDll))
                    File.Copy(dstDll, dstDll + ".pre_auto", true);
                File.Copy(srcDll, dstDll, true);
            }
            string[] proxyPairs = {
                "d3dcompiler_47.dll", "d3dcompiler_47.dll",
                "vulkan-1.dll", "vulkan-1.dll",
            };
            for (int i = 0; i < proxyPairs.Length; i += 2) {
                var psrc = Path.Combine(ManagerDir, "plugin", proxyPairs[i]);
                var pdst = Path.Combine(gameRoot, proxyPairs[i + 1]);
                if (File.Exists(psrc)) {
                    if (File.Exists(pdst) && !FilesEqual(psrc, pdst))
                        File.Copy(pdst, pdst + ".bak", true);
                    File.Copy(psrc, pdst, true);
                }
            }
            // remove legacy eiem.dll so Unity does not double-inject
            if (File.Exists(Path.Combine(pluginDir, "eiem.dll")))
                File.Delete(Path.Combine(pluginDir, "eiem.dll"));
            foreach (var f in Directory.GetFiles(pluginDir, "eiem.dll.*"))
                File.Delete(f);

            return null;
        } catch (Exception ex) {
            return L10n.Get("Msg_SetupFailed", ex.Message);
        }
    }

    static void CopyIfMissing(string src, string dst) {
        if (File.Exists(src) && !File.Exists(dst))
            File.Copy(src, dst);
    }

    static bool FilesEqual(string a, string b) {
        try {
            var fa = new FileInfo(a);
            var fb = new FileInfo(b);
            if (fa.Length != fb.Length) return false;
            using var sa = File.OpenRead(a);
            using var sb = File.OpenRead(b);
            var ba = new byte[65536];
            var bb = new byte[65536];
            int n;
            while ((n = sa.Read(ba, 0, ba.Length)) > 0) {
                int m = sb.Read(bb, 0, n);
                if (m != n) return false;
                for (int i = 0; i < n; i++)
                    if (ba[i] != bb[i]) return false;
            }
            return true;
        } catch {
            return false;
        }
    }
}
