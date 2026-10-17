// ViewModels/CharactersViewModel.cs — full per-character editor.
using System;
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Windows.Threading;
using SecondaryMotion.Manager.Models;
using SecondaryMotion.Manager.Services;

namespace SecondaryMotion.Manager.ViewModels;

public class CharacterItem : ViewModelBase {

    // record every user edit so "my change had no effect" can be traced
    void LogEdit(string field, object v) =>
        Services.ChangeLog.Append("[Edit] " + Data.Id + " " + field + " = " + v);
    public CharacterData Data { get; }
    public CharacterItem(CharacterData d) { Data = d; }

    // shown name only — the raw id (chr_xxx) is a developer detail
    public string Display => Data.DisplayName.Length > 0 ? Data.DisplayName : Data.Id;

    public string DisplayName {
        get => Data.DisplayName;
        set { Data.DisplayName = value; OnPropertyChanged(); OnPropertyChanged(nameof(Display)); }
    }

    public bool Enabled {
        get => Data.Enabled;
        set { Data.Enabled = value; OnPropertyChanged(); }
    }

    public int ModeIndex {
        get => Data.Mode == "off" ? 0 : Data.Mode == "amplify_native" ? 2 : 1;
        set {
            Data.Mode = value == 0 ? "off" : value == 2 ? "amplify_native" : "synthetic";
            OnPropertyChanged();
        }
    }

    public double IdleAmp { get => Data.Amp[0]; set { Data.Amp[0] = value; LogEdit("IdleAmp", value); OnPropertyChanged(); } }
    public double WalkAmp { get => Data.Amp[1]; set { Data.Amp[1] = value; LogEdit("WalkAmp", value); OnPropertyChanged(); } }
    public double RunAmp { get => Data.Amp[2]; set { Data.Amp[2] = value; LogEdit("RunAmp", value); OnPropertyChanged(); } }
    public double SprintAmp { get => Data.Amp[3]; set { Data.Amp[3] = value; LogEdit("SprintAmp", value); OnPropertyChanged(); } }
    public double ZiplineAmp { get => Data.Amp[4]; set { Data.Amp[4] = value; LogEdit("ZiplineAmp", value); OnPropertyChanged(); } }

    // down amplitudes.  RUNTIME SEMANTICS (verified in-game): amplitude_deg
    // (Amp, first half-cycle) = visual UP swing; amplitude_down_deg (AmpDown,
    // second half-cycle) = visual DOWN swing.  UI columns bind directly:
    // Up column -> Amp, Down column -> AmpDown.  A down value of 0 means
    // symmetric (= up value, runtime falls back to Amp).
    public double IdleDown { get => Data.AmpDown[0] > 0 ? Data.AmpDown[0] : Data.Amp[0]; set { Data.AmpDown[0] = value; LogEdit("IdleDown", value); OnPropertyChanged(); } }
    public double WalkDown { get => Data.AmpDown[1] > 0 ? Data.AmpDown[1] : Data.Amp[1]; set { Data.AmpDown[1] = value; LogEdit("WalkDown", value); OnPropertyChanged(); } }
    public double RunDown { get => Data.AmpDown[2] > 0 ? Data.AmpDown[2] : Data.Amp[2]; set { Data.AmpDown[2] = value; LogEdit("RunDown", value); OnPropertyChanged(); } }
    public double SprintDown { get => Data.AmpDown[3] > 0 ? Data.AmpDown[3] : Data.Amp[3]; set { Data.AmpDown[3] = value; LogEdit("SprintDown", value); OnPropertyChanged(); } }
    public double ZiplineDown { get => Data.AmpDown[4] > 0 ? Data.AmpDown[4] : Data.Amp[4]; set { Data.AmpDown[4] = value; LogEdit("ZiplineDown", value); OnPropertyChanged(); } }

    public double IdleFreq { get => Data.Freq[0]; set { Data.Freq[0] = value; LogEdit("IdleFreq", value); OnPropertyChanged(); } }
    public double WalkFreq { get => Data.Freq[1]; set { Data.Freq[1] = value; LogEdit("WalkFreq", value); OnPropertyChanged(); } }
    public double RunFreq { get => Data.Freq[2]; set { Data.Freq[2] = value; LogEdit("RunFreq", value); OnPropertyChanged(); } }
    public double SprintFreq { get => Data.Freq[3]; set { Data.Freq[3] = value; LogEdit("SprintFreq", value); OnPropertyChanged(); } }
    public double ZiplineFreq { get => Data.Freq[4]; set { Data.Freq[4] = value; LogEdit("ZiplineFreq", value); OnPropertyChanged(); } }

    // per-gait phase alignment offset (deg, 0-180)
    public double WalkPhaseOffset { get => Data.PhaseOffset[1]; set { Data.PhaseOffset[1] = value; LogEdit("WalkPhaseOffset", value); OnPropertyChanged(); } }
    public double RunPhaseOffset { get => Data.PhaseOffset[2]; set { Data.PhaseOffset[2] = value; LogEdit("RunPhaseOffset", value); OnPropertyChanged(); } }
    public double SprintPhaseOffset { get => Data.PhaseOffset[3]; set { Data.PhaseOffset[3] = value; LogEdit("SprintPhaseOffset", value); OnPropertyChanged(); } }
    public double ZiplinePhaseOffset { get => Data.PhaseOffset[4]; set { Data.PhaseOffset[4] = value; LogEdit("ZiplinePhaseOffset", value); OnPropertyChanged(); } }

    // per-gait phase alignment switch (PLL tracking on/off)
    public bool WalkPhaseAlign { get => Data.PhaseAlign[1]; set { Data.PhaseAlign[1] = value; LogEdit("WalkPhaseAlign", value); OnPropertyChanged(); } }
    public bool RunPhaseAlign { get => Data.PhaseAlign[2]; set { Data.PhaseAlign[2] = value; LogEdit("RunPhaseAlign", value); OnPropertyChanged(); } }
    public bool SprintPhaseAlign { get => Data.PhaseAlign[3]; set { Data.PhaseAlign[3] = value; LogEdit("SprintPhaseAlign", value); OnPropertyChanged(); } }
    public bool ZiplinePhaseAlign { get => Data.PhaseAlign[4]; set { Data.PhaseAlign[4] = value; LogEdit("ZiplinePhaseAlign", value); OnPropertyChanged(); } }

    // per-gait auto frequency alignment switch
    public bool WalkAutoFreq { get => Data.AutoFreq[1]; set { Data.AutoFreq[1] = value; LogEdit("WalkAutoFreq", value); OnPropertyChanged(); } }
    public bool RunAutoFreq { get => Data.AutoFreq[2]; set { Data.AutoFreq[2] = value; LogEdit("RunAutoFreq", value); OnPropertyChanged(); } }
    public bool SprintAutoFreq { get => Data.AutoFreq[3]; set { Data.AutoFreq[3] = value; LogEdit("SprintAutoFreq", value); OnPropertyChanged(); } }
    public bool ZiplineAutoFreq { get => Data.AutoFreq[4]; set { Data.AutoFreq[4] = value; LogEdit("ZiplineAutoFreq", value); OnPropertyChanged(); } }

    // per-gait frequency deviation threshold (percent in UI, fraction in storage)
    public double WalkFreqDevThreshold { get => Data.FreqDevThreshold[1] * 100.0; set { Data.FreqDevThreshold[1] = value / 100.0; LogEdit("WalkFreqDevThreshold", Data.FreqDevThreshold[1]); OnPropertyChanged(); } }
    public double RunFreqDevThreshold { get => Data.FreqDevThreshold[2] * 100.0; set { Data.FreqDevThreshold[2] = value / 100.0; LogEdit("RunFreqDevThreshold", Data.FreqDevThreshold[2]); OnPropertyChanged(); } }
    public double SprintFreqDevThreshold { get => Data.FreqDevThreshold[3] * 100.0; set { Data.FreqDevThreshold[3] = value / 100.0; LogEdit("SprintFreqDevThreshold", Data.FreqDevThreshold[3]); OnPropertyChanged(); } }
    public double ZiplineFreqDevThreshold { get => Data.FreqDevThreshold[4] * 100.0; set { Data.FreqDevThreshold[4] = value / 100.0; LogEdit("ZiplineFreqDevThreshold", Data.FreqDevThreshold[4]); OnPropertyChanged(); } }

    public int AxisIndex {
        get => Data.Axis.Length == 0 ? 0 : Array.IndexOf(new[] { "X", "Y", "Z" }, Data.Axis) + 1;
        set {
            Data.Axis = value == 0 ? "" : new[] { "X", "Y", "Z" }[value - 1];
            OnPropertyChanged();
        }
    }

    public int SignIndex { get => Data.AxisSign < 0 ? 1 : 0; set { Data.AxisSign = value == 0 ? 1 : -1; LogEdit("SignIndex", value); OnPropertyChanged(); } }

    public double EnvAttack { get => Data.EnvAttack; set { Data.EnvAttack = value; LogEdit("EnvAttack", value); OnPropertyChanged(); } }
    public double EnvFreq { get => Data.EnvFreq; set { Data.EnvFreq = value; LogEdit("EnvFreq", value); OnPropertyChanged(); } }
    public double EnvIdle { get => Data.EnvIdle; set { Data.EnvIdle = value; LogEdit("EnvIdle", value); OnPropertyChanged(); } }
    public double NativeFactor { get => Data.NativeFactor; set { Data.NativeFactor = value; LogEdit("NativeFactor", value); OnPropertyChanged(); } }
    public bool JumpEnabled { get => Data.JumpEnabled; set { Data.JumpEnabled = value; LogEdit("JumpEnabled", value); OnPropertyChanged(); } }
    public double JumpAmplitude { get => Data.JumpAmplitude; set { Data.JumpAmplitude = value; LogEdit("JumpAmplitude", value); OnPropertyChanged(); } }
    public double JumpDampingTau { get => Data.JumpDampingTau; set { Data.JumpDampingTau = value; LogEdit("JumpDampingTau", value); OnPropertyChanged(); } }
    public double JumpFrequency { get => Data.JumpFrequency; set { Data.JumpFrequency = value; LogEdit("JumpFrequency", value); OnPropertyChanged(); } }
    public double JumpMaxDuration { get => Data.JumpMaxDuration; set { Data.JumpMaxDuration = value; LogEdit("JumpMaxDuration", value); OnPropertyChanged(); } }
    public double JumpTakeoffDelay { get => Data.JumpTakeoffDelay; set { Data.JumpTakeoffDelay = value; LogEdit("JumpTakeoffDelay", value); OnPropertyChanged(); } }
    public double JumpRisingTarget { get => Data.JumpRisingTarget; set { Data.JumpRisingTarget = value; LogEdit("JumpRisingTarget", value); OnPropertyChanged(); } }
    public double JumpApexFallingTarget { get => Data.JumpApexFallingTarget; set { Data.JumpApexFallingTarget = value; LogEdit("JumpApexFallingTarget", value); OnPropertyChanged(); } }
    public double JumpAccelerationResponse { get => Data.JumpAccelerationResponse; set { Data.JumpAccelerationResponse = value; LogEdit("JumpAccelerationResponse", value); OnPropertyChanged(); } }
    public double JumpAccelerationFilterTau { get => Data.JumpAccelerationFilterTau; set { Data.JumpAccelerationFilterTau = value; LogEdit("JumpAccelerationFilterTau", value); OnPropertyChanged(); } }
    public double JumpNaturalFrequency { get => Data.JumpNaturalFrequency; set { Data.JumpNaturalFrequency = value; LogEdit("JumpNaturalFrequency", value); OnPropertyChanged(); } }
    public double JumpDampingRatio { get => Data.JumpDampingRatio; set { Data.JumpDampingRatio = value; LogEdit("JumpDampingRatio", value); OnPropertyChanged(); } }
    public double JumpLandingImpulseGain { get => Data.JumpLandingImpulseGain; set { Data.JumpLandingImpulseGain = value; LogEdit("JumpLandingImpulseGain", value); OnPropertyChanged(); } }

    // Main-page UI helpers. The stored schema remains unchanged.
    public double JumpRisingMagnitude {
        get => Math.Abs(Data.JumpRisingTarget);
        set {
            Data.JumpRisingTarget = -Math.Abs(value);
            LogEdit("JumpRisingTarget", Data.JumpRisingTarget);
            OnPropertyChanged();
            OnPropertyChanged(nameof(JumpRisingTarget));
        }
    }

    static readonly (double Tau, double Frequency, double Duration)[] JumpShakePresets = {
        (0.30, 2.50, 1.60),
        (0.55, 2.00, 3.00),
        (0.50, 2.50, 2.80),
        (0.57, 2.25, 3.11),
        (0.70, 2.50, 4.40),
    };

    public double JumpShakePresetIndex {
        get {
            int best = 0;
            double bestScore = double.MaxValue;
            for (int i = 0; i < JumpShakePresets.Length; ++i) {
                var p = JumpShakePresets[i];
                double score = Math.Abs(Data.JumpDampingTau - p.Tau) +
                               Math.Abs(Data.JumpFrequency - p.Frequency) +
                               Math.Abs(Data.JumpMaxDuration - p.Duration);
                if (score < bestScore) { best = i; bestScore = score; }
            }
            return best;
        }
        set {
            int index = Math.Clamp((int)Math.Round(value), 0, JumpShakePresets.Length - 1);
            var p = JumpShakePresets[index];
            Data.JumpDampingTau = p.Tau;
            Data.JumpFrequency = p.Frequency;
            Data.JumpMaxDuration = p.Duration;
            LogEdit("JumpShakePreset", (index + 1) +
                " (tau=" + p.Tau + ", frequency=" + p.Frequency +
                ", duration=" + p.Duration + ")");
            OnPropertyChanged();
            OnPropertyChanged(nameof(JumpDampingTau));
            OnPropertyChanged(nameof(JumpFrequency));
            OnPropertyChanged(nameof(JumpMaxDuration));
            OnPropertyChanged(nameof(JumpShakePresetSummary));
        }
    }

    public string JumpShakePresetSummary => L10n.Get(
        "M_jump_shake_preset_values",
        Data.JumpDampingTau.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture),
        Data.JumpFrequency.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture),
        Data.JumpMaxDuration.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture));
}

public class CharactersViewModel : ViewModelBase {
    readonly AppCtx _ctx;

    public ObservableCollection<CharacterItem> Items { get; } = new();

    CharacterItem? _selected;
    public CharacterItem? Selected {
        get => _selected;
        set { if (Set(ref _selected, value)) OnPropertyChanged(nameof(HasSelection)); }
    }
    public bool HasSelection => _selected != null;

    public string ApplyMessage => _ctx.ApplyMessage;
    public bool IsApplying => _ctx.ApplyStatus == ApplyState.Applying;

    public RelayCommand ApplyCommand { get; }
    public RelayCommand RefreshCommand { get; }
    public RelayCommand DeleteCommand { get; }

    public CharactersViewModel(AppCtx ctx) {
        _ctx = ctx;
        ApplyCommand = new RelayCommand(_ => {
            _ctx.SaveDisplayNames();
            _ctx.Apply();
        });
        RefreshCommand = new RelayCommand(_ => Refresh());
        DeleteCommand = new RelayCommand(_ => DeleteSelected());
        _ctx.ApplyChanged += () => {
            OnPropertyChanged(nameof(ApplyMessage));
            OnPropertyChanged(nameof(IsApplying));
        };
        Refresh();
    }

    void DeleteSelected() {
        if (Selected == null) return;
        string id = Selected.Data.Id;
        string name = Selected.Data.DisplayName.Length > 0 ? Selected.Data.DisplayName : id;
        var ask = System.Windows.MessageBox.Show(
            L10n.Get("Msg_DeleteConfirm", name, id),
            L10n.Get("Msg_DeleteConfirmTitle"),
            System.Windows.MessageBoxButton.YesNo,
            System.Windows.MessageBoxImage.Warning);
        if (ask != System.Windows.MessageBoxResult.Yes) return;
        ChangeLog.Append("[Character] delete: " + name + " (" + id + ")");
        _ctx.DeleteCharacter(id);
        Refresh();
    }

    public void Refresh() {
        var sel = Selected?.Data.Id;
        Items.Clear();
        foreach (var c in _ctx.Characters) Items.Add(new CharacterItem(c));
        if (sel != null)
            foreach (var it in Items)
                if (it.Data.Id == sel) { Selected = it; break; }
    }
}
