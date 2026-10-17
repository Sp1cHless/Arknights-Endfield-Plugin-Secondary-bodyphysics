// Models/CharacterData.cs — per-character merged view (DB defaults + preset overrides)
using System.Globalization;

namespace SecondaryMotion.Manager.Models;

public class CharacterData {
    public string Id = "";
    public string DisplayName = "";
    public bool Enabled = true;
    public string Mode = "synthetic";           // off|synthetic|amplify_native
    public double[] Amp = { 0, 3.6, 8.5, 12, 8.5 };       // up amplitude (idx4=zipline)
    public double[] AmpDown = { 0, 0, 0, 0, 0 };          // 0 = symmetric (=up)
    public double[] Freq = { 1.2, 1.5, 1.7, 2.0, 1.7 };
    public double[] PhaseOffset = { 0, 0, 0, 0, 0 };         // phase align offset (deg, 0-180)
    public bool[] PhaseAlign = { true, true, true, true, true };   // PLL phase tracking per gait
    public bool[] AutoFreq = { true, true, true, true, true };     // auto frequency alignment per gait
    public double[] FreqDevThreshold = { 0.05, 0.05, 0.05, 0.05, 0.05 }; // dev threshold (fraction) per gait
    public double EnvAttack = 0.15, EnvFreq = 0.20, EnvIdle = 0.015;
    public double NativeFactor = 2.0;
    public bool JumpEnabled = false;
    public double JumpAmplitude = 23.333;
    public double JumpDampingTau = 0.35;
    public double JumpFrequency = 3.0;
    public double JumpMaxDuration = 1.20;
    public double JumpTakeoffDelay = 0.08;
    public double JumpRisingTarget = -23.333;
    public double JumpApexFallingTarget = 23.333;
    public double JumpAccelerationResponse = 0.1667;
    public double JumpAccelerationFilterTau = 0.08;
    public double JumpNaturalFrequency = 2.2;
    public double JumpDampingRatio = 0.52;
    public double JumpLandingImpulseGain = 6.667;
    public string Axis = "";                    // ""=auto | X|Y|Z
    public double AxisSign = 1.0;
    public string BoneRight = "";               // DB bone names (wizard)
    public string BoneLeft = "";
    public string OriginalDisplayName = "";     // DB name at load time

    public CharacterData Clone() => (CharacterData)MemberwiseClone();

    public string ModeDisplay => Mode == "off" ? "Original" : Mode == "amplify_native" ? "Amplify Native" : "Synthetic";

    static string F(double v) => v.ToString("0.##", CultureInfo.InvariantCulture);
}
