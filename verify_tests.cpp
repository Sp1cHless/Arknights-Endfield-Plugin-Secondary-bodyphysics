// verify_tests.cpp — V2 automated verification (architecture §40).
// Standalone console test; not part of the plugin.  Run via verify.bat.
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <limits>

#include "src/common/quat.h"
#include "src/config/json_mini.h"
#include "src/config/config_types.h"
#include "src/config/config_loader.h"
#include "src/config/config_validator.h"
#include "src/motion/gait_classifier.h"
#include "src/motion/freq_lock.h"
#include "src/motion/locomotion_envelope.h"
#include "src/motion/locomotion_release.h"
#include "src/motion/synthetic_base_filter.h"
#include "src/motion/jump_inertial_source.h"
#include "src/runtime/dev_command.h"
#include "src/diagnostics/bone_dump_json.h"
#include "src/diagnostics/movement_signal_types.h"
#include "src/diagnostics/jump_phase_a_probe.h"
#include "src/diagnostics/transform_recorder_policy.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(name, cond)                                                     \
  do {                                                                        \
    if (cond) {                                                               \
      g_pass++;                                                               \
      printf("  PASS %s\n", name);                                            \
    } else {                                                                  \
      g_fail++;                                                               \
      printf("  FAIL %s\n", name);                                            \
    }                                                                         \
  } while (0)

static bool Near(float a, float b, float eps = 1e-4f) {
  return fabsf(a - b) <= eps;
}

// ---------- JSON parse ----------
static void TestJson() {
  printf("[JSON]\n");
  const char *files[] = {
      "SecondaryMotion/data/characters.default.json",
      "SecondaryMotion/presets/Default.json",
      "SecondaryMotion/presets/User.json",
      "SecondaryMotion/runtime/config.json",
  };
  for (const char *f : files) {
    jsonmini::Value root;
    CHECK(f, jsonmini::LoadJsonFile(f, root));
  }
}

// ---------- character DB + preset merge ----------
static void TestDatabase() {
  printf("[DATABASE]\n");
  ConfigSnapshot snap;
  CHECK("db load", LoadCharacterDatabase(
                       "SecondaryMotion/data/characters.default.json", snap));
  if (snap.characters.empty()) return;
  CHECK("db has aurora", snap.characters.count("chr_0014_aurora") == 1);
  CHECK("db has yvonne", snap.characters.count("chr_0017_yvonne") == 1);
  CHECK("db aurora axis explicit Z",
        snap.characters["chr_0014_aurora"].axisExplicit &&
            snap.characters["chr_0014_aurora"].axis.axis == Axis::Z);
  CHECK("db aurora run 8.5",
        Near(snap.characters["chr_0014_aurora"].run.amplitudeDeg, 8.5f));
  CHECK("db jump enabled for existing characters",
        snap.characters["chr_0014_aurora"].jump.enabled);
  CHECK("db jump carries validated Phase-E defaults",
        Near(snap.characters["chr_0014_aurora"].jump.risingTargetDeg,
             -23.333f, 0.001f) &&
        Near(snap.characters["chr_0014_aurora"].jump.landingImpulseGain,
             6.667f, 0.001f));

  // preset merge
  CHECK("preset apply", ApplyUserPreset("SecondaryMotion/presets/User.json",
                                        snap));
  CHECK("preset disables chen", !snap.characters["chr_0005_chen"].enabled);
  CHECK("preset tunes yvonne run 10.0",
        Near(snap.characters["chr_0017_yvonne"].run.amplitudeDeg, 10.0f));
  CHECK("preset keeps aurora bones",
        snap.characters["chr_0014_aurora"].bones.rightName ==
            "R_breast_01_jnt");

  // unknown fail-closed: not in DB -> not in snapshot
  ConfigSnapshot snap2;
  LoadCharacterDatabase("SecondaryMotion/data/characters.default.json", snap2);
  CHECK("unknown char absent", snap2.characters.count("chr_9999_xx") == 0);

  // Character-owned extension whitelist is loaded from the technical DB,
  // never from a user preset and never parsed in the animation callback.
  const char *ruleDbPath = "verify_animation_rules.json";
  FILE *ruleDb = fopen(ruleDbPath, "wb");
  const char *ruleJson =
      "{\"schema_version\":1,\"characters\":{\"chr_test\":{"
      "\"animation_rules\":{"
      "\"special_hop\":\"run\","
      "\"special_dash\":\"sprint\","
      "\"bad\":\"idle\"}}}}";
  if (ruleDb) {
    fwrite(ruleJson, 1, strlen(ruleJson), ruleDb);
    fclose(ruleDb);
  }
  ConfigSnapshot ruleSnap;
  bool ruleLoaded = LoadCharacterDatabase(ruleDbPath, ruleSnap);
  remove(ruleDbPath);
  CHECK("animation rules db load", ruleLoaded);
  CHECK("animation rules parsed",
        ruleLoaded && ruleSnap.characters.count("chr_test") == 1 &&
            ruleSnap.characters.at("chr_test").animationRules.size() == 2);
  if (ruleLoaded && ruleSnap.characters.count("chr_test") == 1 &&
      ruleSnap.characters.at("chr_test").animationRules.size() == 2) {
    const CharacterProfile &rp = ruleSnap.characters.at("chr_test");
    CHECK("animation rule run mapping",
          rp.animationRules[0].contains == "special_hop" &&
              rp.animationRules[0].gait == GaitRun);
    CHECK("animation rule sprint mapping",
          rp.animationRules[1].contains == "special_dash" &&
              rp.animationRules[1].gait == GaitSprint);
  }

  std::string tooManyJson = "{\"animation_rules\":{";
  for (int i = 0; i < 17; ++i) {
    if (i) tooManyJson += ",";
    tooManyJson += "\"rule_" + std::to_string(i) + "\":\"run\"";
  }
  tooManyJson += "}}";
  jsonmini::Value tooManyValue;
  CharacterProfile tooManyProfile;
  jsonmini::Parser tooManyParser;
  tooManyParser.p = tooManyJson.c_str();
  bool tooManyParsed = tooManyParser.ParseValue(tooManyValue);
  if (tooManyParsed) ParseAnimationRules(tooManyValue, tooManyProfile);
  CHECK("oversized animation rules rejected",
        tooManyParsed && tooManyProfile.animationRules.empty());
}

// ---------- gait classifier ----------
static void TestGait() {
  printf("[GAIT]\n");
  CHECK("run loop -> 2", ClassifyClipName("A_actor_lady_run_loop") == 2);
  CHECK("sprint loop -> 3", ClassifyClipName("A_actor_girl_sprint_loop") == 3);
  CHECK("walk loop -> 1", ClassifyClipName("A_actor_girl_walk_loop") == 1);
  CHECK("idle loop excluded", ClassifyClipName("A_actor_girl_idle_loop") == -1);
  CHECK("relax loop excluded", ClassifyClipName("A_actor_zhuangfy_relax_loop") == -1);
  CHECK("relax special excluded",
        ClassifyClipName("A_actor_zhuangfy_relax_sp_01") == -1);
  CHECK("generic skill start excluded",
        ClassifyClipName("A_actor_lady_skill_start") == -1);
  CHECK("generic battle stop excluded",
        ClassifyClipName("A_actor_lady_battle_stop") == -1);
  CHECK("generic attack transition excluded",
        ClassifyClipName("A_actor_lady_attack_to_idle") == -1);
  CHECK("unknown special excluded by default",
        ClassifyClipName("A_actor_lossi_special_hop_loop") == -1);
  CHECK("special containing run excluded by default",
        ClassifyClipName("A_actor_future_special_run_hop") == GaitNone);
  CHECK("dash containing sprint excluded by default",
        ClassifyClipName("A_actor_future_sprint_dash_sp") == GaitNone);
  CharacterProfile specialProfile;
  AnimationRule specialRun;
  specialRun.contains = "lossi_special_hop";
  specialRun.gait = GaitRun;
  specialProfile.animationRules.push_back(specialRun);
  CHECK("character special rule opts clip into run",
        ClassifyClipName("A_actor_lossi_special_hop_run_loop", &specialProfile) ==
            GaitRun);
  AnimationRule specialDash;
  specialDash.contains = "special_dash";
  specialDash.gait = GaitSprint;
  specialProfile.animationRules.push_back(specialDash);
  CHECK("character dash rule opts clip into sprint",
        ClassifyClipName("A_actor_future_sprint_special_dash", &specialProfile) ==
            GaitSprint);
  CHECK("rules are character scoped",
        ClassifyClipName("A_actor_future_special_dash") == GaitNone);
  CharacterProfile overlapProfile;
  AnimationRule broadDash;
  broadDash.contains = "dash";
  broadDash.gait = GaitWalk;
  overlapProfile.animationRules.push_back(broadDash);
  AnimationRule specificDash;
  specificDash.contains = "special_dash";
  specificDash.gait = GaitSprint;
  overlapProfile.animationRules.push_back(specificDash);
  CHECK("longest character rule wins",
        ClassifyClipName("A_actor_future_special_dash", &overlapProfile) ==
            GaitSprint);
  CHECK("none is not write eligible", !IsLocomotionGait(GaitNone));
  CHECK("idle is not write eligible", !IsLocomotionGait(GaitIdle));
  CHECK("walk is write eligible", IsLocomotionGait(GaitWalk));
  CHECK("run is write eligible", IsLocomotionGait(GaitRun));
  CHECK("sprint is write eligible", IsLocomotionGait(GaitSprint));
  CHECK("zipline is write eligible", IsLocomotionGait(GaitZipline));
  CHECK("normal run can write",
        CanWriteSynthetic(GaitRun, false, false));
  CHECK("disabled jump cannot write",
        !CanWriteSynthetic(GaitRun, true, false));
  CHECK("enabled jump can write",
        CanWriteSynthetic(GaitRun, true, true));
  CHECK("jump -> 2 (baseline)", ClassifyClipName("idle_jump_start_l") == 2);
  CHECK("run_stop -> 1", ClassifyClipName("run_stop_r") == 1);
  CHECK("sprint_stop -> 1", ClassifyClipName("sprint_stop_l") == 1);
  CHECK("run_to_walk -> 1", ClassifyClipName("run_to_walk_r") == 1);
  CHECK("run_to_sprint -> 3", ClassifyClipName("run_to_sprint_l") == 3);
  CHECK("walk_start -> 1", ClassifyClipName("walk_start_l_0_r") == 1);
  CHECK("sprint_start -> 3", ClassifyClipName("sprint_start_l") == 3);
  CHECK("zipline start -> 4", ClassifyClipName("A_actor_lady_interact_zipline_start") == 4);
  CHECK("zipline slide -> 4", ClassifyClipName("A_actor_lady_interact_zipline_sp_02") == 4);
  CHECK("zipline stop -> 4", ClassifyClipName("A_actor_lady_interact_zipline_stop") == 4);
  CHECK("unknown monster idle excluded",
        ClassifyClipName("A_actor_monster_hound_idle") == -1);
  GaitClassification c = ClassifyClipNameFull("run_stop_r");
  CHECK("stop -> toIdle flag", c.transitionToIdle);
  GaitClassification excludedStop =
      ClassifyClipNameFull("A_actor_lady_battle_stop");
  CHECK("excluded stop has no transition flag",
        excludedStop.gait == GaitNone && !excludedStop.transitionToIdle);
  GaitClassification excludedToIdle =
      ClassifyClipNameFull("A_actor_lady_attack_to_idle");
  CHECK("excluded to-idle has no transition flag",
        excludedToIdle.gait == GaitNone && !excludedToIdle.transitionToIdle);
  GaitClassification excludedSpecialJump =
      ClassifyClipNameFull("A_actor_future_special_jump_land");
  CHECK("excluded special jump has no jump flags",
        excludedSpecialJump.gait == GaitNone &&
            !excludedSpecialJump.jumpDetected &&
            !excludedSpecialJump.landingDetected);
  GaitClassification j = ClassifyClipNameFull("idle_jump_land_l");
  CHECK("land clip detected", j.landingDetected);
  GaitClassification i = ClassifyClipNameFull("idle_jump_start_l");
  CHECK("jump start NOT landing", !i.landingDetected);
}

// ---------- envelope ----------
// FrameGate needs >0.5ms between advances (game frames); sleep to simulate.
static void EnvAdvance(LocomotionEnvelope &env, float amp, float down,
                       float freq, bool toIdle, float attack, float release,
                       float freqTau) {
  Sleep(2);
  env.Advance(amp, down, freq, toIdle, attack, release, freqTau);
}

static void TestEnvelope() {
  printf("[ENVELOPE]\n");
  CHECK("ground run is onset eligible",
        IsGroundLocomotionOnsetEligible(GaitRun, false, false));
  CHECK("zipline is not onset eligible",
        !IsGroundLocomotionOnsetEligible(GaitZipline, false, false));
  CHECK("stop is not onset eligible",
        !IsGroundLocomotionOnsetEligible(GaitWalk, true, false));
  CHECK("jump-mapped run is not onset eligible",
        !IsGroundLocomotionOnsetEligible(GaitRun, false, true));
  bool wasGroundMoving = false;
  bool onsetActive = false;
  float onsetTau = SelectLocomotionAttackTau(
      true, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f,
      wasGroundMoving, onsetActive);
  CHECK("first ground movement uses internal 0.10s onset tau",
        Near(onsetTau, 0.10f) && wasGroundMoving && onsetActive);
  float settledTau = SelectLocomotionAttackTau(
      true, 0.96f, 0.96f, 1.0f, 1.0f, 1.0f,
      wasGroundMoving, onsetActive);
  CHECK("onset exits near 95 percent target",
        Near(settledTau, 1.0f) && wasGroundMoving && !onsetActive);
  float upgradeTau = SelectLocomotionAttackTau(
      true, 1.0f, 1.0f, 2.0f, 2.0f, 1.0f,
      wasGroundMoving, onsetActive);
  CHECK("moving gait upgrade keeps normal attack tau",
        Near(upgradeTau, 1.0f) && !onsetActive);
  float stoppedTau = SelectLocomotionAttackTau(
      false, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
      wasGroundMoving, onsetActive);
  CHECK("stop disarms onset epoch",
        Near(stoppedTau, 1.0f) && !wasGroundMoving && !onsetActive);
  float restartTau = SelectLocomotionAttackTau(
      true, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f,
      wasGroundMoving, onsetActive);
  CHECK("new ground movement rearms onset",
        Near(restartTau, 0.10f) && wasGroundMoving && onsetActive);
  LocomotionEnvelope env;
  // run target: 8.5 deg -> rad 0.1484
  float target = DegToRad(8.5f);
  for (int i = 0; i < 300; i++)
    EnvAdvance(env, target, target, 1.7f, false, 0.15f, 0.015f, 0.20f);
  CHECK("amp converges to run", Near(env.Amplitude(), target, 1e-3f));
  CHECK("freq converges to 1.7", Near(env.freqEnv, 1.7f, 1e-3f));
  // phase bounded
  CHECK("phase in [0,2pi)", env.Phase() >= 0.0f && env.Phase() < 6.2832f);
  // to-idle fast release
  LocomotionEnvelope env2;
  for (int i = 0; i < 120; i++)
    EnvAdvance(env2, target, target, 1.7f, false, 0.15f, 0.015f, 0.20f);
  for (int i = 0; i < 30; i++)
    EnvAdvance(env2, 0.0f, 0.0f, 1.7f, true, 0.15f, 0.015f, 0.20f);
  CHECK("to-idle releases fast", env2.Amplitude() < 0.02f);
  // asymmetric down channel
  LocomotionEnvelope env3;
  float downT = DegToRad(5.0f);
  for (int i = 0; i < 300; i++)
    EnvAdvance(env3, target, downT, 1.7f, false, 0.15f, 0.015f, 0.20f);
  CHECK("down amp converges separately", Near(env3.DownAmplitude(), downT, 1e-3f));
  // phase anchor (mechanism-A fix)
  LocomotionEnvelope envA;
  envA.SetPhase(1.0);
  EnvAdvance(envA, 0.0f, 0.0f, 1.7f, false, 0.15f, 0.015f, 0.20f);  // prime gate
  EnvAdvance(envA, 0.0f, 0.0f, 1.7f, false, 0.15f, 0.015f, 0.20f);  // advance
  CHECK("SetPhase anchors then advances from the anchor",
        envA.Phase() > 1.0f && envA.Phase() < 1.3f);
  LocomotionEnvelope envB;
  envB.SetPhase(3.0 * 3.14159265358979);
  CHECK("SetPhase wraps into [0,2pi)", envB.Phase() >= 0.0f &&
                                          envB.Phase() < 6.2832f);
  envB.SetPhase(-0.5);
  CHECK("SetPhase wraps negatives", envB.Phase() > 5.5f);
  CHECK("walk/run/sprint/zipline align eligible",
        IsPhaseAlignEligible(GaitWalk, false, false) &&
            IsPhaseAlignEligible(GaitRun, false, false) &&
            IsPhaseAlignEligible(GaitSprint, false, false) &&
            IsPhaseAlignEligible(GaitZipline, false, false));
  CHECK("idle not align eligible",
        !IsPhaseAlignEligible(GaitIdle, false, false));
  CHECK("to-idle not align eligible",
        !IsPhaseAlignEligible(GaitWalk, true, false));
  CHECK("jump-mapped run not align eligible",
        !IsPhaseAlignEligible(GaitRun, false, true));
  CHECK("2-cycle mapping: loop start is phase 0",
        Near((float)PhaseFromNormalizedTime(0.0f, 2.0), 0.0f));
  CHECK("2-cycle mapping: quarter loop is half cycle",
        Near((float)PhaseFromNormalizedTime(0.25f, 2.0),
             3.14159265358979f));
  CHECK("2-cycle mapping: half loop wraps to full cycle",
        Near((float)PhaseFromNormalizedTime(0.5f, 2.0), 0.0f));
  CHECK("2-cycle mapping: three-quarter loop is half cycle",
        Near((float)PhaseFromNormalizedTime(0.75f, 2.0),
             3.14159265358979f));
  CHECK("1-cycle mapping unchanged",
        Near((float)PhaseFromNormalizedTime(0.5f, 1.0),
             3.14159265358979f));
  CHECK("phase offset 180 inverts",
        Near((float)ApplyGaitPhaseOffset(0.0, 180.0f),
             3.14159265358979f));
  CHECK("phase offset 0 keeps mapping",
        Near((float)ApplyGaitPhaseOffset(1.0, 0.0f), 1.0f));
  CHECK("phase offset wraps within [0,2pi)",
        ApplyGaitPhaseOffset(5.5, 90.0f) >= 0.0 &&
            ApplyGaitPhaseOffset(5.5, 90.0f) < 6.2832);
  CHECK("per-gait offset lookup defaults 0",
        GaitPhaseOffsetDeg(CharacterProfile(), GaitWalk) == 0.0f);
  CharacterProfile po;
  po.run.phaseOffsetDeg = 180.0f;
  po.sprint.phaseOffsetDeg = 90.0f;
  CHECK("per-gait offset lookup run",
        GaitPhaseOffsetDeg(po, GaitRun) == 180.0f);
  CHECK("per-gait offset lookup sprint",
        GaitPhaseOffsetDeg(po, GaitSprint) == 90.0f);
  CHECK("per-gait offset lookup zipline default",
        GaitPhaseOffsetDeg(po, GaitZipline) == 0.0f);
  // loop stability (phase-tracking reference validity)
  CHECK("run loop is loop stable",
        ClassifyClipNameFull("A_actor_girl_run_loop").loopStable);
  CHECK("walk loop is loop stable",
        ClassifyClipNameFull("A_actor_lady_walk_loop").loopStable);
  CHECK("zipline traversal is loop stable",
        ClassifyClipNameFull("A_actor_lady_interact_zipline_sp_02").loopStable);
  CHECK("start clip not loop stable",
        !ClassifyClipNameFull("walk_start_l_0_r").loopStable);
  CHECK("stop clip not loop stable",
        !ClassifyClipNameFull("run_stop_r").loopStable);
  CHECK("to clip not loop stable",
        !ClassifyClipNameFull("run_to_walk_r").loopStable);
  CHECK("jump clip not loop stable",
        !ClassifyClipNameFull("idle_jump_start_l").loopStable);
  CHECK("zipline start not loop stable",
        !ClassifyClipNameFull("A_actor_lady_interact_zipline_start").loopStable);
  CHECK("idle loop not loop stable",
        !ClassifyClipNameFull("A_actor_girl_idle_loop").loopStable);
  CHECK("walk pivot keeps gait but cannot drive loop timing",
        ClassifyClipNameFull("walk_pivot_r").gait == GaitWalk &&
        !ClassifyClipNameFull("walk_pivot_r").loopStable);
  CHECK("run and sprint bumps cannot drive loop timing",
        ClassifyClipNameFull("run_bump").gait == GaitRun &&
        !ClassifyClipNameFull("run_bump").loopStable &&
        ClassifyClipNameFull("sprint_bump").gait == GaitSprint &&
        !ClassifyClipNameFull("sprint_bump").loopStable);
  // PLL phase tracking
  CHECK("wrap error shortest path forward",
        Near((float)WrapPhaseError(0.2, 1.0), 0.8f));
  CHECK("wrap error shortest path backward",
        Near((float)WrapPhaseError(0.2, 5.9), -0.583185307179586f));
  LocomotionEnvelope pll;
  pll.freqEnv = 2.0f;
  const float replayDt = 1.0f / 60.0f;
  double movingRef = 3.14159265358979;
  bool cadenceBounded = true;
  for (int i = 0; i < 720; i++) {
    movingRef = fmod(movingRef + 6.28318530717959 * 2.0 * replayDt,
                     6.28318530717959);
    pll.SetPhaseTracking(true, movingRef);
    double before = pll.phase;
    pll.AdvanceDt(replayDt, 0.0f, 0.0f, 2.0f, false, 0.5f, 0.015f, 1.0f);
    double hz = WrapPhaseError(before, pll.phase) / (6.28318530717959 * replayDt);
    cadenceBounded &= hz >= 1.59999 && hz <= 2.40001;
  }
  CHECK("PLL never halves or doubles cadence while erasing half-cycle error",
        cadenceBounded);
  CHECK("PLL stays in [0,2pi)", pll.Phase() >= 0.0f && pll.Phase() < 6.2832f);
  CHECK("PLL converges to moving ref", fabs(WrapPhaseError(pll.phase, movingRef)) < 0.03);
  LocomotionEnvelope freeEnv;
  freeEnv.SetPhaseTracking(false, 3.14159265358979);
  CHECK("no tracking disables PLL", !freeEnv.PhaseTrackingActive());
  EnvAdvance(freeEnv, 0.0f, 0.0f, 1.5f, false, 0.5f, 0.015f, 0.20f);  // prime
  EnvAdvance(freeEnv, 0.0f, 0.0f, 1.5f, false, 0.5f, 0.015f, 0.20f);  // advance
  // two frames can only integrate a few degrees; a wrongly-enabled snap
  // would land the phase at the reference (pi) instead.
  CHECK("no tracking = integration only, not pulled to ref",
        fabsf((float)WrapPhaseError(freeEnv.Phase(), 3.14159265358979)) > 2.0f);
  CHECK("verified stop tail active with energy off-gait",
        ReleaseTailActive(true, false, 0.1f, 0.0f) &&
            ReleaseTailActive(true, false, 0.0f, 0.02f));
  CHECK("small finite tail continues to zero instead of an amplitude cutoff",
        ReleaseTailActive(true, false, 0.0001f, 0.0f));
  CHECK("release tail inactive when settled or on locomotion",
        !ReleaseTailActive(true, false, 0.0f, 0.0f) &&
            !ReleaseTailActive(true, true, 0.1f, 0.1f));
  CHECK("residual energy cannot start a tail without a verified stop",
        !ReleaseTailActive(false, false, 0.1f, 0.1f));
  // User-scale to-idle tau (0.3s): the decay must be GRADUAL (still most of
  // the amplitude shortly after the stop) and must eventually settle at
  // zero — the engine keeps writing this bounded decay instead
  // of truncating it mid-swing.
  LocomotionEnvelope tail;
  for (int i = 0; i < 300; i++)
    EnvAdvance(tail, target, target, 1.7f, false, 0.15f, 0.3f, 0.20f);
  for (int i = 0; i < 5; i++)
    EnvAdvance(tail, 0.0f, 0.0f, 1.7f, true, 0.15f, 0.3f, 0.20f);
  float earlyTail = tail.Amplitude();
  for (int i = 0; i < 600; i++)
    EnvAdvance(tail, 0.0f, 0.0f, 1.7f, true, 0.15f, 0.3f, 0.20f);
  CHECK("slow to-idle tau decays gradually, no mid-swing snap",
        earlyTail > target * 0.5f && tail.Amplitude() == 0.0f);
  // Stop->idle is one continuous release. Verify the closed form at low
  // amplitude too, where the old exponential tail could oscillate for ages.
  bool finiteReleases = true;
  bool releaseShape = true;
  bool releaseFrequency = true;
  for (float tau : {0.015f, 0.3f, 1.0f, 3.0f}) {
    for (float startAmp : {0.001f, 0.15f, 0.5f}) {
      for (float hz : {1.85f, 3.6f}) {
        for (float dt : {1.0f/30, 1.0f/60, 1.0f/144}) {
          LocomotionEnvelope release;
          release.ampEnv = startAmp;
          release.downEnv = startAmp * .4f;
          release.freqEnv = hz;
          double elapsed = 0.0;
          float previous = startAmp;
          int frames = (int)ceil(kStopReleaseDurationInTau * tau / dt) + 1;
          for (int i = 0; i < frames; i++) {
            release.AdvanceDt(dt, 0, 0, hz, true, .15f, tau, .2f);
            elapsed += dt;
            double handoff = log(2.0) * tau;
            double remaining = fmax(0.0, 1.0 - (elapsed - handoff) / (2.0 * tau));
            float expected = (float)(startAmp * (elapsed <= handoff ?
                exp(-elapsed / tau) : .5 * remaining * remaining));
            releaseShape &= Near(release.ampEnv, expected, 1e-7f) &&
                            Near(release.downEnv, expected * .4f, 1e-7f) &&
                            release.ampEnv <= previous;
            releaseFrequency &= release.freqEnv == hz;
            previous = release.ampEnv;
          }
          finiteReleases &= release.ampEnv == 0 && release.downEnv == 0;
        }
      }
    }
  }
  CHECK("finite stop release follows joined curve across amplitudes and frame rates", releaseShape);
  CHECK("all stop releases reach exact zero within ln2 plus two tau and one frame", finiteReleases);
  CHECK("finite stop release preserves cadence", releaseFrequency);
  bool onsetUnchanged = true, handoffSmooth = true, endpointSmooth = true;
  bool lastFrameSmall = true;
  for (float tau : {.015f, .2f, .5f, .7f, 1.0f, 3.0f}) {
    double join = log(2.0) * tau;
    double end = (log(2.0) + 2.0) * tau;
    double epsilon = 1e-4 * tau;
    for (double fraction : {0.0, .1, .3, .6})
      onsetUnchanged &= Near(StopReleaseGain(fraction * tau, tau), (float)exp(-fraction), 1e-7f);
    double leftSlope = (StopReleaseGain(join, tau) - StopReleaseGain(join-epsilon, tau)) / epsilon;
    double rightSlope = (StopReleaseGain(join+epsilon, tau) - StopReleaseGain(join, tau)) / epsilon;
    handoffSmooth &= Near(StopReleaseGain(join, tau), .5f, 1e-7f) &&
                     fabs(leftSlope + .5/tau) < .002/tau &&
                     fabs(rightSlope + .5/tau) < .002/tau;
    for (double hz : {1.2, 1.85, 3.6}) {
      for (double phase : {0.0, 1.0, 2.5, 4.7}) {
        auto angle = [&](double time) {
          return StopReleaseGain(time, tau) * sin(phase + 6.28318530717959 * hz * time);
        };
        endpointSmooth &= angle(end) == 0 && angle(end+epsilon) == 0 &&
                          fabs((angle(end)-angle(end-epsilon))/epsilon) < .001/tau;
      }
    }
    // When the finite endpoint is crossed, the last contribution is already
    // bounded quadratically by dt^2/(8*tau^2), even at 20fps.
    if (tau >= .2f) {
      for (double dt : {1.0/20, 1.0/30, 1.0/60, 1.0/144})
        lastFrameSmall &= StopReleaseGain(end-dt, tau) <= dt*dt/(8*tau*tau) + 1e-7 &&
                          StopReleaseGain(end-dt, tau) < .008;
    }
  }
  CHECK("stop onset is unchanged until half amplitude", onsetUnchanged);
  CHECK("exponential to finite tail handoff preserves amplitude and slope", handoffSmooth);
  CHECK("stop angle and angular velocity approach zero for different cadences and phases", endpointSmooth);
  CHECK("last frame is below 0.8 percent at supported preset tau even at 20fps", lastFrameSmall);
  // One stop epoch crosses stop->idle without renewing duration or amplitude.
  StopReleaseState acceptedStop;
  StopReleaseSample stopSample, idleSample;
  stopSample.Observe("run_stop", ClassifyClipNameFull("run_stop"), 0);
  idleSample.Observe("idle_loop", ClassifyClipNameFull("idle_loop"), 0);
  acceptedStop.Observe(stopSample);
  LocomotionEnvelope intoIdle;
  intoIdle.ampEnv = .2f;
  intoIdle.downEnv = .3f;
  intoIdle.freqEnv = 3.0f;
  bool completedInIdle = true;
  for (int i = 0; i < 180; i++) {
    bool inStop = i < 30;
    acceptedStop.Observe(inStop ? stopSample : idleSample);
    bool tailAllowed = ReleaseTailActive(acceptedStop.armed, inStop,
                                         intoIdle.ampEnv, intoIdle.downEnv);
    bool release = inStop || tailAllowed;
    float previous = intoIdle.ampEnv;
    intoIdle.AdvanceDt(1.0f/60, 0, 0, 3.0f, release, .15f, 1.0f, .2f);
    completedInIdle &= intoIdle.ampEnv <= previous;
    if (!inStop && previous > 0) completedInIdle &= tailAllowed;
  }
  CHECK("accepted stop continues through idle to finite zero without restarting",
        completedInIdle && intoIdle.ampEnv == 0 && intoIdle.downEnv == 0);
  LocomotionEnvelope restartRelease;
  restartRelease.ampEnv = .2f;
  restartRelease.downEnv = .1f;
  restartRelease.AdvanceDt(.05f, 0, 0, 2, true, .15f, 1, .2f);
  float beforeResume = restartRelease.ampEnv;
  restartRelease.AdvanceDt(.05f, .2f, .1f, 2, false, .15f, 1, .2f);
  CHECK("movement interrupts stop release with ordinary continuous attack",
        !restartRelease.stopReleasing &&
        Near(restartRelease.ampEnv, beforeResume + (.2f-beforeResume)*(1-expf(-.05f/.15f))));
  float newStart = restartRelease.ampEnv;
  restartRelease.AdvanceDt(.05f, 0, 0, 2, true, .15f, 1, .2f);
  CHECK("next stop captures current amplitude and starts a fresh release",
        Near(restartRelease.ampEnv, newStart * expf(-.05f)));
  restartRelease.Reset();
  CHECK("runtime reset clears finite stop state",
        !restartRelease.stopReleasing && restartRelease.releaseElapsedSec == 0);
  // Rate-assisted PLL: with the oscillator free-running at a WRONG
  // frequency while tracking a reference, the positional pull alone would
  // hold error = rate mismatch x tau forever (the frequency smoothing
  // regression).  The integral rate correction must absorb the mismatch.
  LocomotionEnvelope pllRate;
  pllRate.freqEnv = 2.0f;
  pllRate.SetPhase(0.0);
  movingRef = 0.0;
  for (int i = 0; i < 720; i++) {
    movingRef = fmod(movingRef + 6.28318530717959 * 1.8 * replayDt,
                     6.28318530717959);
    pllRate.SetPhaseTracking(true, movingRef);
    pllRate.AdvanceDt(replayDt, 0.0f, 0.0f, 2.0f, false, 0.5f, 0.015f, 1.0f);
  }
  CHECK("PLL rate assist converges despite frequency mismatch",
        fabs(WrapPhaseError(pllRate.phase, movingRef)) < 0.03);
  CHECK("PLL rate correction pulls toward the reference rate",
        fabs(pllRate.phaseCorrHz + 0.2) < 0.02);
  CHECK("explicit interval rejects invalid frames",
        !pllRate.AdvanceDt(0.0f, 0, 0, 2, false, .5f, .015f, 1));
  pllRate.SetPhaseTracking(false, 0.0);
  CHECK("tracking disengage clears the rate correction",
        pllRate.phaseCorrHz == 0.0);
  // Rate limits are common to all gaits and remain effective with the
  // real 20Hz held phase reference at several render frame rates.
  bool commonRateBounds = true, phaseSwitchOff = true;
  for (float initialHz : {1.2f, 1.85f, 3.6f}) {
    for (float targetHz : {1.2f, 1.85f, 3.6f}) {
      for (float dt : {1.0f/20, 1.0f/30, 1.0f/60, 1.0f/144}) {
        LocomotionEnvelope tracked;
        tracked.freqEnv = initialHz;
        tracked.SetPhase(2.5);
        double time = 0, sampleTime = -1, heldRef = 0;
        for (int frame = 0; frame < (int)(4/dt); frame++) {
          time += dt;
          if (time-sampleTime >= .05) {
            sampleTime = time;
            heldRef = fmod(6.28318530717959 * targetHz * time, 6.28318530717959);
          }
          tracked.SetPhaseTracking(true, heldRef);
          double previous = tracked.phase;
          tracked.AdvanceDt(dt, .1f, .1f, targetHz, false, .15f, 1, 1);
          double effective = WrapPhaseError(previous, tracked.phase) / (6.28318530717959 * dt);
          double bound = fmin(kPhaseCorrMaxHz, tracked.freqEnv * kPhaseCorrMaxFraction);
          commonRateBounds &= fabs(effective-tracked.freqEnv) <= bound + 1e-6;
        }
        tracked.SetPhaseTracking(false, 0);
        double previous = tracked.phase;
        tracked.AdvanceDt(dt, .1f, .1f, targetHz, false, .15f, 1, 1);
        phaseSwitchOff &= fabs(WrapPhaseError(previous, tracked.phase) /
            (6.28318530717959 * dt) - tracked.freqEnv) < 1e-6;
      }
    }
  }
  CHECK("all gait rates obey combined PLL bounds at 20Hz phase sampling", commonRateBounds);
  CHECK("phase align off leaves pure configured frequency integration", phaseSwitchOff);
  // GaitDownAmplitude fallback semantics (0 = symmetric = up); inlined here
  // because synthetic_motion.h pulls IL2CPP deps that verify cannot link.
  CharacterProfile p0;
  p0.run.amplitudeDeg = 8.5f;
  p0.run.amplitudeDownDeg = 0.0f;
  CHECK("down=0 -> symmetric", Near(
      p0.run.amplitudeDownDeg > 0.0f ? p0.run.amplitudeDownDeg : p0.run.amplitudeDeg,
      8.5f, 1e-4f));
  p0.run.amplitudeDownDeg = 5.0f;
  CHECK("down>0 -> explicit", Near(
      p0.run.amplitudeDownDeg > 0.0f ? p0.run.amplitudeDownDeg : p0.run.amplitudeDeg,
      5.0f, 1e-4f));
}

// ---------- quaternion ----------
static void TestQuat() {
  printf("[QUAT]\n");
  Quat a = QuatAxisAngle(2, DegToRad(8.5f));  // Z +8.5 deg
  Quat id = QuatIdentity();
  CHECK("axisangle w positive", a.w > 0.9f);
  CHECK("mul identity", Near(QuatMul(a, id).w, a.w) &&
                            Near(QuatMul(a, id).z, a.z));
  Quat inv = QuatMul(a, QuatInv(a));
  CHECK("inv product identity", Near(inv.w, 1.0f, 1e-5f) &&
                                    Near(inv.x, 0.0f, 1e-5f) &&
                                    Near(inv.y, 0.0f, 1e-5f) &&
                                    Near(inv.z, 0.0f, 1e-5f));
  Quat pk1 = QuatPowK(a, 1.0f);
  CHECK("powk 1 identity", Near(pk1.w, a.w, 1e-4f) && Near(pk1.z, a.z, 1e-4f));
  Quat pk2 = QuatPowK(a, 2.0f);
  CHECK("powk 2 doubles", pk2.w < a.w);  // 17° -> half-angle 8.5° -> w smaller
  CHECK("slerp 0 = a", Near(QuatSlerp(a, id, 0.0f).w, a.w));
  CHECK("slerp 1 = b", Near(QuatSlerp(a, id, 1.0f).w, 1.0f));
}

// ---------- synthetic composition base ----------
static float ZAngleDeg(Quat q) {
  return RadToDeg(2.0f * atan2f(q.z, q.w));
}

static void TestSyntheticBaseFilter() {
  printf("[SYNTHETIC BASE FILTER]\n");
  SyntheticBaseFilter filter;
  float maxNativeResidualDeg = 0.0f;
  const float dt = 1.0f / 60.0f;
  for (int frame = 0; frame < 7200; ++frame) {
    float t = frame * dt;
    float nativeDeg = 7.0f * sinf(2.0f * 3.14159265358979f * 2.983f * t);
    float syntheticDeg =
        22.0f * sinf(2.0f * 3.14159265358979f * 3.0f * t);
    Quat current = QuatAxisAngle(2, DegToRad(nativeDeg));
    Quat outR, outL;
    filter.Compose(current, current, 2, DegToRad(syntheticDeg), dt,
                   outR, outL);
    if (t >= 2.0f) {
      float residual = fabsf(ZAngleDeg(outR) - syntheticDeg);
      if (residual > maxNativeResidualDeg) maxNativeResidualDeg = residual;
    }
  }
  CHECK("synthetic base suppresses near-frequency native twist",
        maxNativeResidualDeg < 1.6f);

  SyntheticBaseFilter feedback;
  Quat feedbackR, feedbackL;
  feedback.Compose(QuatIdentity(), QuatIdentity(), 2, DegToRad(20.0f),
                   dt, feedbackR, feedbackL);
  for (int frame = 0; frame < 600; ++frame) {
    const Quat previousR = feedbackR;
    const Quat previousL = feedbackL;
    feedback.Compose(previousR, previousL, 2, DegToRad(20.0f),
                     dt, feedbackR, feedbackL);
  }
  CHECK("reading our own output never accumulates rotation",
        Near(ZAngleDeg(feedbackR), 20.0f, 1e-3f) &&
            Near(ZAngleDeg(feedbackL), 20.0f, 1e-3f));
  Quat equivalent = {-feedbackR.x, -feedbackR.y, -feedbackR.z, -feedbackR.w};
  feedback.Compose(equivalent, equivalent, 2, DegToRad(20.0f),
                   dt, feedbackR, feedbackL);
  CHECK("equivalent quaternion signs still block self feedback",
        Near(ZAngleDeg(feedbackR), 20.0f, 1e-3f));
  const Quat changedNative = QuatAxisAngle(2, DegToRad(10.0f));
  feedback.Compose(changedNative, changedNative, 2, DegToRad(20.0f),
                   dt, feedbackR, feedbackL);
  CHECK("a refreshed native pose still updates the composition base",
        ZAngleDeg(feedback.baseR) > 0.1f && ZAngleDeg(feedback.baseR) < 10.0f);
  feedback.Reset();
  CHECK("reset discards previous output and native base",
        !feedback.hasLastOut && !feedback.initialized);
  feedback.Compose(changedNative, changedNative, 2, DegToRad(5.0f),
                   dt, feedbackR, feedbackL);
  CHECK("composition after reset captures the new native pose",
        Near(ZAngleDeg(feedbackR), 15.0f, 1e-3f));

  Quat directR, directL;
  Quat current = QuatAxisAngle(2, DegToRad(10.0f));
  filter.ComposeDirect(current, current, 2, DegToRad(5.0f), directR, directL);
  CHECK("synthetic direct composition preserves native pose",
        Near(ZAngleDeg(directR), 15.0f, 1e-3f));
  for (int frame = 0; frame < 120; ++frame) {
    Quat previousR = directR, previousL = directL;
    filter.ComposeDirect(previousR, previousL, 2, DegToRad(5.0f), directR, directL);
  }
  CHECK("direct stop composition does not accumulate read-back rotation",
        Near(ZAngleDeg(directR), 15.0f, 1e-3f));
  current = QuatAxisAngle(2, DegToRad(30.0f));
  filter.ComposeDirect(current, current, 2, DegToRad(5.0f), directR, directL);
  CHECK("direct composition still follows a genuinely refreshed native pose",
        Near(ZAngleDeg(directR), 35.0f, 1e-3f));
  filter.ComposeDirect(directR, directL, 2, DegToRad(2.0f), directR, directL);
  CHECK("direct stop changes the offset without adding the previous offset",
        Near(ZAngleDeg(directR), 32.0f, 1e-3f));
}

// ---------- hot reload ----------
static void TestHotReload() {
  printf("[HOT-RELOAD]\n");
  RuntimePathsSetRootForTest("SecondaryMotion");
  RuntimeConfigFile rc;
  CHECK("runtime config parse",
        LoadRuntimeConfigFile(rc) && rc.activePreset == "Default");
  // ConfigReloadIfChanged with a fake revision: reload reads the real files
  // (revision 0 on disk) -> snapshot revision must equal the requested one.
  bool ok = ConfigReloadIfChanged(7);
  const ConfigSnapshot *s = ConfigAcquire();
  CHECK("reload installs", ok && s && s->revision == 7);
  CHECK("reload has chars", s && s->characters.size() >= 6);
  if (s) {
    CHECK("reload yvonne present", s->characters.count("chr_0017_yvonne") == 1);
    // config.json active_preset=Default -> Default.json overrides apply
    CHECK("reload uses Default preset (chen enabled)",
          s->characters.count("chr_0005_chen") == 1 &&
              s->characters.at("chr_0005_chen").enabled);
    CHECK("reload Default keeps aurora run amplitude from preset",
          s->characters.at("chr_0014_aurora").run.amplitudeDeg > 0.0f);
  }
  // reloading the same revision is a no-op
  CHECK("same revision no-op", !ConfigReloadIfChanged(7));
}

static void TestConfigValidation() {
  printf("[CONFIG-VALIDATION]\n");
  const char *text =
      "{\"gait\":{\"run\":{\"amplitude_deg\":-500,"
      "\"frequency_hz\":-7}},\"envelope\":{"
      "\"amplitude_attack_tau_sec\":-0.2,"
      "\"frequency_tau_sec\":0,"
      "\"to_idle_release_tau_sec\":99}}";
  jsonmini::Parser parser;
  parser.p = text;
  jsonmini::Value value;
  CharacterProfile profile;
  CHECK("finite amplitude/frequency/tau have no range limits",
        parser.ParseValue(value) && ParseParamBlock(value, profile));

  MotionMode mode = MotionMode::Off;
  Axis axis = Axis::Z;
  CHECK("known motion mode accepted",
        ParseMotionModeStrict("synthetic", mode) &&
            mode == MotionMode::Synthetic);
  CHECK("unknown motion mode rejected", !ParseMotionModeStrict("bad", mode));
  CHECK("known axis accepted", ParseAxisStrict("Y", axis) && axis == Axis::Y);
  CHECK("unknown axis rejected", !ParseAxisStrict("Q", axis));
  CHECK("axis sign accepts exact +/-1",
        IsValidAxisSign(1.0) && IsValidAxisSign(-1.0));
  CHECK("axis sign rejects other values", !IsValidAxisSign(0.0));
  CHECK("bone pair allows both empty", HasCompleteBonePair("", ""));
  CHECK("bone pair allows both present", HasCompleteBonePair("R", "L"));
  CHECK("bone pair rejects one side", !HasCompleteBonePair("R", ""));

  auto ParamBlockAccepted = [](const char *json) {
    jsonmini::Parser p;
    p.p = json;
    jsonmini::Value v;
    CharacterProfile profile;
    return p.ParseValue(v) && ParseParamBlock(v, profile);
  };
  CHECK("gait numeric type mismatch rejected",
        !ParamBlockAccepted("{\"gait\":{\"run\":{\"frequency_hz\":\"bad\"}}}"));
  CHECK("envelope non-object rejected",
        !ParamBlockAccepted("{\"envelope\":[]}"));
  CHECK("jump mode type mismatch rejected",
        !ParamBlockAccepted("{\"jump\":{\"mode\":3}}"));
  CHECK("native factor type mismatch rejected",
        !ParamBlockAccepted("{\"native_amplify\":{\"factor\":\"bad\"}}"));
  // per-gait phase-align / auto-frequency / dev-threshold parsing
  CHECK("phase_align bool accepted",
        ParamBlockAccepted("{\"gait\":{\"run\":{\"phase_align\":false}}}"));
  CHECK("phase_align type mismatch rejected",
        !ParamBlockAccepted("{\"gait\":{\"run\":{\"phase_align\":1}}}"));
  CHECK("auto_frequency bool accepted",
        ParamBlockAccepted("{\"gait\":{\"run\":{\"auto_frequency\":false}}}"));
  CHECK("auto_frequency type mismatch rejected",
        !ParamBlockAccepted("{\"gait\":{\"run\":{\"auto_frequency\":\"yes\"}}}"));
  CHECK("freq_dev_threshold number accepted",
        ParamBlockAccepted("{\"gait\":{\"run\":{\"freq_dev_threshold\":0.1}}}"));
  CHECK("freq_dev_threshold type mismatch rejected",
        !ParamBlockAccepted("{\"gait\":{\"run\":{\"freq_dev_threshold\":\"5%\"}}}"));
  {
    jsonmini::Parser p;
    p.p = "{\"gait\":{\"run\":{\"phase_align\":false,\"auto_frequency\":false,"
          "\"freq_dev_threshold\":0.15}}}";
    jsonmini::Value v;
    CharacterProfile profile;
    CHECK("new gait fields parsed into GaitParam",
          p.ParseValue(v) && ParseParamBlock(v, profile) &&
              !profile.run.phaseAlign && !profile.run.autoFrequency &&
              Near(profile.run.freqDevThreshold, 0.15f) &&
              profile.walk.phaseAlign && profile.walk.autoFrequency &&
              Near(profile.walk.freqDevThreshold, 0.05f));
  }

  ConfigSnapshot validSnapshot;
  CharacterProfile validProfile;
  validProfile.enabled = true;
  validProfile.bones.rightName = "R";
  validProfile.bones.leftName = "L";
  validSnapshot.characters["valid"] = validProfile;
  CHECK("final validator accepts complete bone pair",
        ValidateSnapshot(validSnapshot));

  ConfigSnapshot invalidSnapshot = validSnapshot;
  invalidSnapshot.characters["valid"].bones.leftName.clear();
  CHECK("final validator rejects one-sided bone pair",
        !ValidateSnapshot(invalidSnapshot));

  ConfigSnapshot badThreshold = validSnapshot;
  badThreshold.characters["valid"].run.freqDevThreshold = 1.5f;
  CHECK("final validator rejects threshold >= 1",
        !ValidateSnapshot(badThreshold));
  badThreshold.characters["valid"].run.freqDevThreshold = -0.1f;
  CHECK("final validator rejects threshold <= 0",
        !ValidateSnapshot(badThreshold));
  badThreshold.characters["valid"].run.freqDevThreshold = 0.05f;
  CHECK("final validator accepts default threshold",
        ValidateSnapshot(badThreshold));

  auto RuntimeConfigAccepted = [](const char *json) {
    jsonmini::Parser p;
    p.p = json;
    jsonmini::Value v;
    RuntimeConfigFile runtime;
    return p.ParseValue(v) && ParseRuntimeConfigValue(v, runtime);
  };
  CHECK("runtime config valid types accepted",
        RuntimeConfigAccepted("{\"revision\":2,\"enabled\":true,"
                              "\"active_preset\":\"Default\","
                              "\"global\":{\"gait_sample_interval_ms\":50}}"));
  CHECK("runtime revision type mismatch rejected",
        !RuntimeConfigAccepted("{\"revision\":\"2\"}"));
  CHECK("runtime enabled type mismatch rejected",
        !RuntimeConfigAccepted("{\"enabled\":1}"));
  CHECK("runtime interval must be positive integer",
        !RuntimeConfigAccepted("{\"global\":{\"gait_sample_interval_ms\":0}}"));
}

static void TestRuntimePaths() {
  printf("[RUNTIME-PATHS]\n");
  RuntimePathsSetRootForTest(
      "D:\\Games\\Endfield Game\\SecondaryMotion");
  char marker[512] = {};
  CHECK("runtime plugin path derived",
        RuntimePluginPath(marker, sizeof(marker), "spring_test.txt"));
  CHECK("runtime plugin path points to sibling plugin directory",
        strcmp(marker,
               "D:\\Games\\Endfield Game\\plugin\\spring_test.txt") == 0);
}

static void TestBoneDumpJson() {
  printf("[BONE-DUMP-JSON]\n");
  FILE *f = tmpfile();
  CHECK("bone dump temp file", f != nullptr);
  if (!f) return;
  bool first = true;
  BoneScannerWriteEntry(f, "root\"slash\\line\n", 0, first);
  BoneScannerWriteEntry(f, "child", 1, first);
  fflush(f);
  rewind(f);
  char body[1024] = {};
  size_t count = fread(body, 1, sizeof(body) - 1, f);
  fclose(f);
  body[count] = 0;
  std::string json = std::string("{\"bones\":[") + body + "]}";
  jsonmini::Parser parser;
  parser.p = json.c_str();
  jsonmini::Value root;
  CHECK("bone dump entries form valid escaped JSON array",
        parser.Parse(root) && root.Find("bones") &&
            root.Find("bones")->arr.size() == 2);
}

static void TestDevCommandRevision() {
  printf("[DEV-COMMAND]\n");
  g_devCmd = DevCommand();
  g_devCmd.active = true;
  g_devCmd.revision = 7;
  strncpy(g_devCmd.type, "axis_test", sizeof(g_devCmd.type) - 1);
  DevCommandClear();
  CHECK("clear deactivates command", !g_devCmd.active);
  CHECK("clear preserves processed revision", g_devCmd.revision == 7);
}

static JumpPhaseAObservation MakeJumpPhaseAObservation(
    DWORD ms, bool pluginEnabled, const char *characterId, void *animator,
    bool readOk, int reported, size_t parsed) {
  JumpPhaseAObservation o;
  o.sampleMs = ms;
  o.pluginEnabled = pluginEnabled;
  o.readOk = readOk;
  o.reportedCount = reported;
  o.parsedCount = parsed;
  snprintf(o.characterId, sizeof(o.characterId), "%s", characterId);
  o.animator = animator;
  return o;
}

static size_t CountCsvColumns(const std::string &line) {
  size_t columns = 1;
  bool quoted = false;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '"') {
      if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
        ++i;
      } else {
        quoted = !quoted;
      }
    } else if (line[i] == ',' && !quoted) {
      ++columns;
    }
  }
  return columns;
}

static void TestJumpPhaseAProbe() {
  printf("[JUMP-PHASE-A]\n");
  static JumpPhaseATimeline timeline;
  CHECK("Phase A ring storage initializes before producer use",
        timeline.Initialize());
  void *animatorA = reinterpret_cast<void *>(0x1000);
  void *animatorB = reinterpret_cast<void *>(0x2000);

  JumpPhaseAObservation idle = MakeJumpPhaseAObservation(
      1000, false, "chr_a", animatorA, true, 1, 1);
  snprintf(idle.clips[0].name, sizeof(idle.clips[0].name),
           "A_actor_lady_idle_loop");
  idle.clips[0].weight = 1.0f;
  CHECK("Phase A ignores non-jump before trigger", !timeline.Observe(idle));
  CHECK("Phase A remains idle before trigger",
        timeline.State() == JumpPhaseAState::Idle && timeline.Count() == 0);

  JumpPhaseAObservation enabledJump = MakeJumpPhaseAObservation(
      1050, true, "chr_a", animatorA, true, 2, 2);
  snprintf(enabledJump.clips[0].name, sizeof(enabledJump.clips[0].name),
           "idle_jump_start_l");
  snprintf(enabledJump.clips[1].name, sizeof(enabledJump.clips[1].name),
           "A_actor_lady_run_loop");
  enabledJump.sampleJumpEvidence = true;
  CHECK("Phase A cannot start while production is enabled",
        !timeline.Observe(enabledJump) &&
            timeline.State() == JumpPhaseAState::Idle);

  JumpPhaseAObservation trigger = enabledJump;
  trigger.pluginEnabled = false;
  trigger.sampleMs = 1100;
  trigger.clips[0].weight = 0.0f;
  trigger.clips[1].weight = 1.0f;
  CHECK("first disabled Jump sample starts capture", timeline.Observe(trigger));
  CHECK("trigger sample is retained",
        timeline.State() == JumpPhaseAState::Capturing &&
            timeline.Count() == 1 && timeline.Frames()[0].sampleJump &&
            !timeline.Frames()[0].sampleLanding);

  JumpPhaseAObservation landing = MakeJumpPhaseAObservation(
      1150, false, "chr_a", animatorA, true, 4, 4);
  snprintf(landing.clips[0].name, sizeof(landing.clips[0].name),
           "A_actor_lady_run_loop");
  snprintf(landing.clips[1].name, sizeof(landing.clips[1].name),
           "idle_jump_land_l");
  snprintf(landing.clips[2].name, sizeof(landing.clips[2].name),
           "idle_jump_start_l");
  snprintf(landing.clips[3].name, sizeof(landing.clips[3].name),
           "decorative,\r\n\"clip\"");
  landing.clips[0].weight = 1.0f;
  landing.clips[1].weight = -1.0f;
  landing.clips[2].weight = NAN;
  landing.sampleJumpEvidence = true;
  landing.sampleLandingEvidence = true;
  CHECK("jump and landing retain gait sampler's profile-aware aggregation",
        timeline.Observe(landing) && timeline.Count() == 2 &&
            timeline.Frames()[1].sampleJump &&
            timeline.Frames()[1].sampleLanding &&
            timeline.Frames()[1].hasNonFiniteWeight);

  JumpPhaseAObservation failure = MakeJumpPhaseAObservation(
      1200, false, "chr_a", animatorA, false, 5, 0);
  failure.readFailure = 5;
  failure.cachedGait = GaitRun;
  failure.cachedJump = true;
  CHECK("read failure is recorded without becoming a fresh Jump edge",
        timeline.Observe(failure) && timeline.Count() == 3 &&
            !timeline.Frames()[2].readOk &&
            !timeline.Frames()[2].sampleJump &&
            timeline.Frames()[2].cachedJump &&
            timeline.Frames()[2].readFailure == 5 &&
            timeline.Frames()[2].failureStreak == 1);

  JumpPhaseAObservation switched = MakeJumpPhaseAObservation(
      1250, false, "chr_b", animatorB, true, 1, 1);
  snprintf(switched.clips[0].name, sizeof(switched.clips[0].name),
           "idle_jump_start_l");
  CHECK("character epoch change seals before appending new rows",
        !timeline.Observe(switched) &&
            timeline.State() == JumpPhaseAState::Sealed &&
            timeline.Count() == 3);

  const char *csvPath = "verify_jump_phase_a.csv";
  remove(csvPath);
  remove("verify_jump_phase_a.csv.tmp");
  CHECK("Phase A CSV writes sealed timeline atomically",
        JumpPhaseAWriteCsvAtomic(csvPath, timeline));
  FILE *csv = fopen(csvPath, "rb");
  std::string csvText;
  if (csv) {
    char buffer[4096];
    size_t n = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), csv)) > 0)
      csvText.append(buffer, n);
    fclose(csv);
  }
  CHECK("Phase A CSV final exists and temp is gone",
        !csvText.empty() &&
            GetFileAttributesA("verify_jump_phase_a.csv.tmp") ==
                INVALID_FILE_ATTRIBUTES);
  CHECK("Phase A CSV escapes clip names",
        csvText.find("\"decorative,\\r\\n\"\"clip\"\"\"") !=
            std::string::npos);
  bool columnsOk = true;
  size_t pos = 0;
  while (pos < csvText.size()) {
    size_t end = csvText.find('\n', pos);
    std::string line = csvText.substr(
        pos, end == std::string::npos ? std::string::npos : end - pos);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (!line.empty() && CountCsvColumns(line) != 51) columnsOk = false;
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  CHECK("Phase D CSV header and rows have 51 columns", columnsOk);
  CHECK("CSV success does not mutate producer state",
        timeline.State() == JumpPhaseAState::Sealed);
  timeline.MarkFlushed();
  CHECK("worker marks flushed only after success",
        timeline.State() == JumpPhaseAState::Flushed);
  remove(csvPath);

  timeline.ResetForTest();
  JumpPhaseAObservation profileJump = MakeJumpPhaseAObservation(
      2000, false, "chr_profile", animatorA, true, 1, 1);
  snprintf(profileJump.clips[0].name, sizeof(profileJump.clips[0].name),
           "character_special_jump_rule");
  CharacterProfile jumpProfile;
  AnimationRule jumpRule;
  jumpRule.contains = "special_jump_rule";
  jumpRule.gait = GaitRun;
  jumpProfile.animationRules.push_back(jumpRule);
  GaitClassification profileClassification =
      ClassifyClipNameFull(profileJump.clips[0].name, &jumpProfile);
  profileJump.sampleJumpEvidence = profileClassification.jumpDetected;
  profileJump.sampleLandingEvidence = profileClassification.landingDetected;
  CHECK("profile rule produces real accepted Jump evidence",
        profileClassification.gait == GaitRun &&
            profileClassification.jumpDetected);
  CHECK("Phase A uses profile-aware result supplied by gait sampler",
        timeline.Observe(profileJump) &&
            timeline.State() == JumpPhaseAState::Capturing &&
            timeline.Frames()[0].sampleJump);

  JumpPhaseAObservation truncated = MakeJumpPhaseAObservation(
      2050, false, "chr_profile", animatorA, true, 12, 8);
  truncated.truncated = true;
  for (size_t i = 0; i < kJumpPhaseAMaxClips; ++i)
    snprintf(truncated.clips[i].name, sizeof(truncated.clips[i].name),
             "clip_%zu", i);
  CHECK("truncated read is retained but evidence is explicitly incomplete",
        timeline.Observe(truncated) && timeline.Count() == 2 &&
            timeline.Frames()[1].clipCount == kJumpPhaseAMaxClips &&
            !timeline.Frames()[1].evidenceComplete);

  JumpPhaseAObservation seal = truncated;
  seal.sampleMs = 2100;
  seal.pluginEnabled = true;
  CHECK("re-enabling production seals Phase A immediately",
        !timeline.Observe(seal) &&
            timeline.State() == JumpPhaseAState::Sealed);
  CHECK("failed CSV write preserves sealed data",
        !JumpPhaseAWriteCsvAtomic("missing_phase_a_dir\\timeline.csv",
                                  timeline) &&
            timeline.State() == JumpPhaseAState::Sealed);

  JumpPhaseAFlushSchedule retry;
  CHECK("sealed CSV is eligible for immediate first attempt",
        retry.Due(100, JumpPhaseAState::Sealed));
  retry.OnFailure(100);
  CHECK("first failure backs off for one second",
        !retry.Due(1099, JumpPhaseAState::Sealed) &&
            retry.Due(1100, JumpPhaseAState::Sealed));
  retry.OnFailure(1100);
  CHECK("second failure doubles the backoff",
        !retry.Due(3099, JumpPhaseAState::Sealed) &&
            retry.Due(3100, JumpPhaseAState::Sealed));
  retry.Reset();
  retry.OnFailure(0xFFFFFFF0u);
  CHECK("flush deadline remains correct across GetTickCount wrap",
        !retry.Due(500, JumpPhaseAState::Sealed) &&
            retry.Due(984, JumpPhaseAState::Sealed));

  timeline.ResetForTest();
  JumpPhaseAObservation wrapTrigger = profileJump;
  wrapTrigger.sampleMs = 0xFFFFFFF0u;
  CHECK("capture starts near GetTickCount wrap", timeline.Observe(wrapTrigger));
  JumpPhaseAObservation wrapDeadline = profileJump;
  wrapDeadline.sampleMs = 59984u;
  CHECK("60 second deadline seals correctly across GetTickCount wrap",
        timeline.Observe(wrapDeadline) &&
            timeline.State() == JumpPhaseAState::Sealed &&
            timeline.Frames()[1].elapsedMs == kJumpPhaseACaptureMs);

  timeline.ResetForTest();
  JumpPhaseAObservation fill = profileJump;
  fill.sampleMs = 3000;
  bool filled = timeline.Observe(fill);
  for (size_t i = 1; filled && i < kJumpPhaseAMaxFrames; ++i)
    filled = timeline.Observe(fill);
  CHECK("fixed ring seals exactly at capacity without overflow",
        filled && timeline.Count() == kJumpPhaseAMaxFrames &&
            timeline.State() == JumpPhaseAState::Sealed);

  timeline.ResetForTest();
  JumpPhaseAObservation truncatedWithoutVisibleJump = truncated;
  truncatedWithoutVisibleJump.sampleMs = 4000;
  truncatedWithoutVisibleJump.sampleJumpEvidence = false;
  CHECK("truncation itself starts a diagnostic capture fail-closed",
        timeline.Observe(truncatedWithoutVisibleJump) &&
            timeline.State() == JumpPhaseAState::Capturing &&
            timeline.Count() == 1 &&
            !timeline.Frames()[0].evidenceComplete &&
            !timeline.Frames()[0].sampleJump);

  timeline.ResetForTest();
  JumpPhaseAObservation partialParse = MakeJumpPhaseAObservation(
      5000, false, "chr_partial", animatorA, true, 8, 5);
  CHECK("reported clips missing from parsed set are incomplete evidence",
        timeline.Observe(partialParse) && timeline.Count() == 1 &&
            !timeline.Frames()[0].evidenceComplete);

  timeline.ResetForTest();
  JumpPhaseAObservation tickZero = profileJump;
  tickZero.sampleMs = 0;
  CHECK("successful sample at tick zero starts capture",
        timeline.Observe(tickZero));
  JumpPhaseAObservation afterTickZeroFailure = tickZero;
  afterTickZeroFailure.sampleMs = 50;
  afterTickZeroFailure.readOk = false;
  afterTickZeroFailure.readFailure = 5;
  afterTickZeroFailure.sampleJumpEvidence = false;
  CHECK("sample age does not use tick zero as an unset sentinel",
        timeline.Observe(afterTickZeroFailure) && timeline.Count() == 2 &&
            timeline.Frames()[1].sampleAgeMs == 50);

  MovementSignalSample movement;
  movement.identityValid = true;
  movement.resolutionMask = MovementResolveEntityField |
                            MovementResolveClass |
                            MovementResolveVelocity |
                            MovementResolveFallingSpeed;
  movement.entity = 0x3000;
  movement.movement = 0x4000;
  movement.velocityValid = true;
  movement.velocity = {1.0f, 2.0f, 3.0f};
  movement.fallingSpeedValid = true;
  movement.fallingSpeed = -4.5f;
  CHECK("Phase D continuous validity requires finite velocity and falling speed",
        MovementSignalContinuousValid(movement));
  movement.velocity.x = NAN;
  CHECK("Phase D continuous validity rejects non-finite values",
        !MovementSignalContinuousValid(movement));
  movement.velocity.x = 1.0f;

  timeline.ResetForTest();
  JumpPhaseAObservation phaseD = idle;
  phaseD.sampleMs = 6000;
  phaseD.phaseDActive = true;
  phaseD.movement = movement;
  CHECK("Phase D starts immediately from a safe identity-valid movement sample",
        timeline.Observe(phaseD) && timeline.Count() == 1 &&
            timeline.Frames()[0].movement.entity == 0x3000 &&
            timeline.Frames()[0].movement.velocityValid);
  JumpPhaseAObservation phaseDStop = phaseD;
  phaseDStop.sampleMs = 6050;
  phaseDStop.pluginEnabled = true;
  CHECK("Phase D re-enable seals before another row",
        !timeline.Observe(phaseDStop) &&
            timeline.State() == JumpPhaseAState::Sealed &&
            timeline.Count() == 1);
  const char *phaseDPath = "verify_jump_phase_d.csv";
  remove(phaseDPath);
  remove("verify_jump_phase_d.csv.tmp");
  CHECK("Phase D movement CSV writes atomically",
        JumpPhaseAWriteCsvAtomic(phaseDPath, timeline));
  FILE *phaseDCsv = fopen(phaseDPath, "rb");
  std::string phaseDText;
  if (phaseDCsv) {
    char buffer[4096];
    size_t n = 0;
    while ((n = fread(buffer, 1, sizeof(buffer), phaseDCsv)) > 0)
      phaseDText.append(buffer, n);
    fclose(phaseDCsv);
  }
  CHECK("Phase D CSV contains validity and continuous signal columns",
        phaseDText.find("movement_identity_valid") != std::string::npos &&
            phaseDText.find("velocity_x") != std::string::npos &&
            phaseDText.find("falling_speed") != std::string::npos &&
            phaseDText.find(",1,2,3,") != std::string::npos &&
            phaseDText.find("-4.5") != std::string::npos);
  remove(phaseDPath);
}

// ---------- Phase E live Jump FSM + bounded inertial source ----------
static JumpLiveSignal JumpSignal(uint32_t serial, DWORD sampleMs, float speed,
                                 bool start = false, bool land = false) {
  JumpLiveSignal s;
  s.serial = serial;
  s.sampleMs = sampleMs;
  s.entity = 0x1234;
  s.identityValid = true;
  s.clipReadValid = true;
  s.fallingSpeedValid = true;
  s.fallingSpeed = speed;
  s.jumpStartActive = start;
  s.landingActive = land;
  return s;
}

static void TestJumpInertialSource() {
  printf("[JUMP PHASE E LIVE]\n");
  CHECK("Jump direction keeps Y-axis characters positive",
        Near(AutomaticJumpDirection(Axis::Y), 1.0f));
  CHECK("Jump direction flips Z-axis characters",
        Near(AutomaticJumpDirection(Axis::Z), -1.0f));
  CHECK("Jump direction defaults X-axis characters positive",
        Near(AutomaticJumpDirection(Axis::X), 1.0f));

  JumpConfig tuning;
  tuning.enabled = true;
  tuning.mode = "landing_damped";

  JumpInertialSource direct;
  JumpInertialOutput d = direct.Tick(1000, JumpSignal(1, 1000, -12.0f,
                                                      false, true), tuning,
                                      true);
  CHECK("direct fall remains native-only",
        !d.valid && d.state == JumpInertialState::NativeOnly &&
            Near(d.angleRad, 0.0f));

  JumpInertialSource src;
  JumpLiveSignal start = JumpSignal(1, 2000, 10.0f, true, false);
  JumpInertialOutput rise = src.Tick(2000, start, tuning, true);
  CHECK("exact start arms one Rising epoch",
        rise.valid && rise.state == JumpInertialState::Rising &&
            rise.eventEpoch == 1 && Near(rise.angleRad, 0.0f));

  JumpInertialOutput held = src.Tick(2016, start, tuning, true);
  CHECK("held sample does not retrigger epoch",
        held.valid && held.eventEpoch == 1);

  JumpInertialSource phasedMotion;
  phasedMotion.Tick(4000, JumpSignal(1, 4000, 10.0f, true), tuning, true);
  float delayedPeakDeg = 0.0f;
  for (int i = 1; i <= 5; ++i) {
    DWORD now = 4000 + (DWORD)(i * 16);
    JumpInertialOutput delayed = phasedMotion.Tick(
        now, JumpSignal(1 + i, now, 10.0f, i < 3), tuning, true);
    float deg = fabsf(RadToDeg(delayed.angleRad));
    if (deg > delayedPeakDeg) delayedPeakDeg = deg;
  }
  CHECK("takeoff inertia has an 80ms visual delay", delayedPeakDeg < 0.1f);

  JumpInertialOutput downward;
  for (int i = 6; i <= 20; ++i) {
    DWORD now = 4000 + (DWORD)(i * 16);
    downward = phasedMotion.Tick(
        now, JumpSignal(1 + i, now, 8.0f, false), tuning, true);
  }
  CHECK("late Rising response moves chest downward",
        RadToDeg(downward.angleRad) < -10.0f &&
            RadToDeg(downward.angleRad) > -35.0f);

  JumpConfig stronger = tuning;
  stronger.risingTargetDeg = tuning.risingTargetDeg * 2.0f;
  JumpInertialSource baseConfigured;
  JumpInertialSource strongConfigured;
  baseConfigured.Tick(5000, JumpSignal(1, 5000, 10.0f, true), tuning, true);
  strongConfigured.Tick(5000, JumpSignal(1, 5000, 10.0f, true), stronger,
                        true);
  JumpInertialOutput baseOut, strongOut;
  for (int i = 1; i <= 20; ++i) {
    DWORD now = 5000 + (DWORD)(i * 16);
    baseOut = baseConfigured.Tick(now, JumpSignal(1 + i, now, 8.0f),
                                  tuning, true);
    strongOut = strongConfigured.Tick(now, JumpSignal(1 + i, now, 8.0f),
                                      stronger, true);
  }
  CHECK("per-profile Rising target changes live source amplitude",
        RadToDeg(strongOut.angleRad) < RadToDeg(baseOut.angleRad) * 1.7f);

  phasedMotion.Tick(4336, JumpSignal(22, 4336, 0.0f), tuning, true);
  float fallingHeldMinDeg = 1000000.0f;
  JumpInertialOutput upward;
  for (int i = 22; i <= 45; ++i) {
    DWORD now = 4000 + (DWORD)(i * 16);
    upward = phasedMotion.Tick(
        now, JumpSignal(1 + i, now, -6.0f), tuning, true);
    if (i >= 38) {
      float deg = RadToDeg(upward.angleRad);
      if (deg < fallingHeldMinDeg) fallingHeldMinDeg = deg;
    }
  }
  CHECK("Apex and Falling response moves upward and stays upward",
        upward.state == JumpInertialState::Falling &&
            fallingHeldMinDeg > 6.0f &&
            RadToDeg(upward.angleRad) < 35.0f);

  JumpInertialOutput apex =
      src.Tick(2063, JumpSignal(2, 2063, 0.1f), tuning, true);
  CHECK("near-zero fallingSpeed enters Apex",
        apex.valid && apex.state == JumpInertialState::Apex);
  JumpInertialOutput fall =
      src.Tick(2126, JumpSignal(3, 2126, -1.0f), tuning, true);
  CHECK("negative fallingSpeed enters Falling",
        fall.valid && fall.state == JumpInertialState::Falling);

  JumpInertialSource withLanding;
  JumpInertialSource noLanding;
  withLanding.Tick(3000, JumpSignal(1, 3000, 10.0f, true), tuning, true);
  noLanding.Tick(3000, JumpSignal(1, 3000, 10.0f, true), tuning, true);
  withLanding.Tick(3063, JumpSignal(2, 3063, -1.0f), tuning, true);
  noLanding.Tick(3063, JumpSignal(2, 3063, -1.0f), tuning, true);
  JumpInertialOutput landed = withLanding.Tick(
      3126, JumpSignal(3, 3126, -12.0f, false, true), tuning, true);
  JumpInertialOutput control = noLanding.Tick(
      3126, JumpSignal(3, 3126, -12.0f, false, false), tuning, true);
  CHECK("landing impulse is causal and position-continuous",
        landed.valid && landed.state == JumpInertialState::LandingTail &&
            Near(landed.angleRad, control.angleRad, 1e-5f) &&
            !Near(landed.angularVelocityDegSec,
                  control.angularVelocityDegSec, 0.01f));

  // Regression: a jump that resolves in a clip whose name has no "land" (e.g.
  // an air attack ending in battle_air_atk_*) never fires a landing edge, so
  // the Rising/Apex/Falling phase would hang forever and freeze the chest at
  // the converged angle. The air-phase watchdog must force-release it.
  JumpInertialSource stuckAir;
  stuckAir.Tick(9000, JumpSignal(1, 9000, 10.0f, true, false), tuning, true);
  JumpInertialOutput airMid = stuckAir.Tick(
      9016, JumpSignal(2, 9016, 8.0f, false, false), tuning, true);
  CHECK("no-landing jump enters an air phase", airMid.valid);
  JumpInertialOutput airBefore;   // still active well inside the ceiling
  JumpInertialOutput airAfter;
  for (int i = 3; i <= 210; ++i) {
    DWORD now = 9016 + (DWORD)(i * 16);
    JumpInertialOutput f = stuckAir.Tick(
        now, JumpSignal(1 + i, now, 8.0f, false, false), tuning, true);
    if (i == 90) airBefore = f;
    airAfter = f;
  }
  CHECK("air-phase watchdog does not release inside the normal ceiling",
        airBefore.valid && airBefore.state != JumpInertialState::NativeOnly);
  CHECK("no-landing air phase is force-released past the ceiling",
        !airAfter.valid && airAfter.state == JumpInertialState::NativeOnly &&
            Near(airAfter.angleRad, 0.0f));

  // Regression: a jump that resolves into an attack/skill clip (no "land"
  // tail) must be excluded from inertia.  An attack clip mid-jump force-
  // releases the epoch immediately, so the chest never freezes even though
  // no landing edge ever arrives.
  JumpInertialSource atkJump;
  JumpInertialOutput atkStart = atkJump.Tick(
      10000, JumpSignal(1, 10000, 10.0f, true, false), tuning, true);
  CHECK("attack-exclusion: a jump still arms an air phase on jump_start",
        atkStart.valid);
  JumpLiveSignal atkSig = JumpSignal(2, 10016, 8.0f, false, false);
  atkSig.attackActive = true;  // current clip is an attack/skill
  JumpInertialOutput atkHit = atkJump.Tick(10016, atkSig, tuning, true);
  CHECK("attack-exclusion: attack clip does not arm an air phase",
        !atkHit.valid && atkHit.state == JumpInertialState::NativeOnly);
  JumpInertialOutput atkAfter = atkJump.Tick(
      10032, JumpSignal(3, 10032, -12.0f, false, false), tuning, true);
  CHECK("attack-exclusion: epoch stays released and never refreezes",
        !atkAfter.valid && atkAfter.state == JumpInertialState::NativeOnly);

  // Regression: a jump that resolves into a zipline traversal
  // (interact_zipline_*, which never carries a "land" tail) is the same
  // no-landing-tail endpoint as an attack, so it must force-release the epoch
  // immediately — otherwise the FSM would hang and freeze the chest at the
  // converged rising/apex angle on dismount.
  JumpInertialSource zipJump;
  JumpInertialOutput zipStart = zipJump.Tick(
      10000, JumpSignal(1, 10000, 10.0f, true, false), tuning, true);
  CHECK("zipline-exclusion: a jump still arms an air phase on jump_start",
        zipStart.valid);
  JumpLiveSignal zipSig = JumpSignal(2, 10016, 8.0f, false, false);
  zipSig.ziplineActive = true;  // current clip is a zipline traversal
  JumpInertialOutput zipHit = zipJump.Tick(10016, zipSig, tuning, true);
  CHECK("zipline-exclusion: zipline clip does not arm an air phase",
        !zipHit.valid && zipHit.state == JumpInertialState::NativeOnly);
  JumpInertialOutput zipAfter = zipJump.Tick(
      10032, JumpSignal(3, 10032, -12.0f, false, false), tuning, true);
  CHECK("zipline-exclusion: epoch stays released and never refreezes",
        !zipAfter.valid && zipAfter.state == JumpInertialState::NativeOnly);

  JumpConfig longShake = tuning;
  longShake.amplitudeDeg = 40.0f;
  longShake.dampingTauSec = 0.7f;
  longShake.frequencyHz = 2.5f;
  longShake.maxDurationSec = 4.4f;
  longShake.landingImpulseGain = 0.0f;
  JumpInertialSource durationGuard;
  durationGuard.Tick(7000, JumpSignal(1, 7000, 10.0f, true), longShake,
                     true);
  durationGuard.Tick(7063, JumpSignal(2, 7063, -12.0f, false, true),
                     longShake, true);
  JumpInertialOutput guarded;
  for (int i = 1; i <= 32; ++i) {
    DWORD now = 7063 + (DWORD)(i * 16);
    guarded = durationGuard.Tick(
        now, JumpSignal(2 + i, now, -12.0f), longShake, true);
  }
  CHECK("explicit landing shake duration prevents early settle",
        guarded.valid &&
            guarded.state == JumpInertialState::LandingTail);

  float maxDeg = 0.0f;
  float earlyShakePeakDeg = 0.0f;
  float lateShakePeakDeg = 0.0f;
  bool shakePositive = false;
  bool shakeNegative = false;
  JumpInertialOutput out = landed;
  for (int i = 1; i <= 140; ++i) {
    DWORD now = 3126 + (DWORD)(i * 16);
    JumpLiveSignal tail = JumpSignal(3 + i, now, -12.0f);
    out = withLanding.Tick(now, tail, tuning, true);
    float deg = fabsf(RadToDeg(out.angleRad));
    if (deg > maxDeg) maxDeg = deg;
    float shake = out.landingShakeAngleDeg;
    if (shake > 1.0f) shakePositive = true;
    if (shake < -1.0f) shakeNegative = true;
    if (i <= 25 && fabsf(shake) > earlyShakePeakDeg)
      earlyShakePeakDeg = fabsf(shake);
    if (i >= 40 && i <= 75 && fabsf(shake) > lateShakePeakDeg)
      lateShakePeakDeg = fabsf(shake);
  }
  CHECK("landing tail adds an explicit decaying oscillation",
        shakePositive && shakeNegative && earlyShakePeakDeg > 10.0f &&
            lateShakePeakDeg < earlyShakePeakDeg * 0.5f);
  CHECK("unclamped diagnostic tail exceeds the old cap but remains finite",
        std::isfinite(maxDeg) && maxDeg > 10.0f);
  CHECK("landing tail is finite",
        out.state == JumpInertialState::NativeOnly && !out.valid);

  JumpLiveSignal invalid = JumpSignal(500, 6000, 1.0f);
  invalid.identityValid = false;
  JumpInertialOutput reset = src.Tick(6000, invalid, tuning, true);
  CHECK("invalid identity resets fail-closed",
        !reset.valid && reset.state == JumpInertialState::NativeOnly &&
            Near(reset.angleRad, 0.0f));
  CHECK("disabled config resets fail-closed",
        !src.Tick(6016, JumpSignal(501, 6016, 10.0f, true), tuning,
                  false).valid);
}

// ---------- auto-frequency alignment (freq_lock.h) ----------
static void TestStopRelease() {
  printf("[STOP-RELEASE]\n");
  auto Sample = [](const char *name) {
    StopReleaseSample sample;
    sample.Observe(name, ClassifyClipNameFull(name), 1.0f);
    return sample;
  };
  const auto run = Sample("A_actor_girl_run_loop");
  const auto stop = Sample("A_actor_girl_run_stop_r");
  const auto idle = Sample("A_actor_girl_idle_loop");
  StopReleaseState state;
  state.Observe(run);
  state.Observe(idle);
  CHECK("run straight to idle never arms a tail", !state.armed);
  state.Observe(stop);
  CHECK("accepted stop arms the decay", state.armed && !state.blocked);
  state.Observe(idle);
  CHECK("idle preserves an already armed stop decay",
        state.armed && ReleaseTailActive(state.armed, false, 0.2f, 0.2f));
  state.Observe(run);
  CHECK("resumed locomotion ends the stop epoch", !state.armed);
  state.Observe(Sample("A_actor_lady_run_to_idle"));
  CHECK("accepted locomotion to-idle transition arms a tail", state.armed);

  const char *interrupts[] = {
      "battle_attack_01", "battle_skill_01", "combo_skill_air",
      "A_actor_lady_attack_to_idle", "idle_jump_start_l",
      "special_dash", "unrecognized_pose", "battle_attack_idle_loop"};
  for (const char *clip : interrupts) {
    state.Observe(run);
    state.Observe(Sample(clip));
    CHECK("direct unsupported transition never starts a tail", !state.armed);
    state.Observe(stop);
    state.Observe(idle);
    state.Observe(Sample(clip));
    CHECK(clip, !state.armed && state.blocked);
    state.Observe(idle);
    CHECK("idle cannot resurrect an interrupted stop decay", !state.armed);
  }
  StopReleaseSample blend = stop;
  blend.Observe("battle_attack_01", ClassifyClipNameFull("battle_attack_01"), 0.2f);
  state.Observe(blend);
  CHECK("an attack blended with a stop cancels the stop", state.blocked && !state.armed);
  state.Observe(stop);
  CHECK("a lingering interrupted stop cannot re-arm the decay", !state.armed);
  StopReleaseSample zeroWeight = stop;
  zeroWeight.Observe("battle_skill_01", ClassifyClipNameFull("battle_skill_01"), 0.0f);
  state.Observe(run);
  state.Observe(zeroWeight);
  CHECK("zero weight cannot conceal an unsupported skill",
        !state.armed && state.blocked);
  state.Observe(StopReleaseSample());
  CHECK("empty clip samples cancel the stop", !state.armed);
  state.Observe(stop);
  state = StopReleaseState();
  state.Observe(idle);
  CHECK("character reset or hot reload cannot retain a stop epoch", !state.armed);

  const float unreliableWeights[] = {0.0f, -1.0f, 1e20f,
      std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()};
  for (float weight : unreliableWeights) {
    StopReleaseSample readableStop, readableIdle;
    readableStop.Observe("A_actor_girl_run_stop_l",
                        ClassifyClipNameFull("A_actor_girl_run_stop_l"), weight);
    readableIdle.Observe("A_actor_girl_idle_loop",
                        ClassifyClipNameFull("A_actor_girl_idle_loop"), weight);
    state = StopReleaseState();
    state.Observe(readableStop);
    state.Observe(readableIdle);
    CHECK("readable stop -> idle keeps its tail despite unreliable weights", state.armed);
    GaitClipSelection selected;
    selected.Observe(ClassifyClipNameFull("A_actor_yvonne_sprint_stop_l"), weight);
    selected.Observe(ClassifyClipNameFull("unrecognized_pose"), 1.0f);
    CHECK("unreliable weights cannot hide a known stop or authorize unknowns",
          selected.classification.gait == GaitWalk &&
              selected.classification.transitionToIdle);
    selected.Observe(ClassifyClipNameFull("A_actor_girl_run_loop"), 0.8f);
    CHECK("usable blend weights retain their selection priority",
          selected.classification.gait == GaitRun);
  }
}

static void TestGaitFrequencyTransition() {
  printf("[GAIT FREQUENCY TRANSITION]\n");
  bool fastResponse = true, continuous = true, phaseContinuous = true;
  for (int from = GaitWalk; from <= GaitZipline; from++) {
    for (int to = GaitWalk; to <= GaitZipline; to++) {
      if (from == to) continue;
      for (float dt : {1.0f/20, 1.0f/30, 1.0f/60, 1.0f/144}) {
        FreqTransitionState state;
        LocomotionEnvelope env;
        float initial = 1.0f + from * .6f;
        float target = 1.0f + to * .6f;
        env.freqEnv = initial;
        env.SetPhase(2.5);
        SelectGaitFrequencyTau(state, from, true, initial, initial, 1);
        for (int frame = 0; frame < (int)ceil(.35f/dt); frame++) {
          float previous = env.freqEnv;
          double oldPhase = env.phase;
          float tau = SelectGaitFrequencyTau(state, to, true, previous, target, 1);
          env.AdvanceDt(dt, 0, 0, target, false, .15f, 1, tau);
          continuous &= fabsf(env.freqEnv-previous) < fabsf(target-initial)*.4f;
          phaseContinuous &= fabs(WrapPhaseError(oldPhase, env.phase) -
                                  6.28318530717959*env.freqEnv*dt) < 1e-6;
        }
        fastResponse &= fabsf(env.freqEnv-target) <= fabsf(target-initial)*.051f;
      }
    }
  }
  CHECK("all gait frequency transitions cover 95 percent of gap within 0.35s", fastResponse);
  CHECK("gait frequency transitions remain smooth instead of snapping cadence", continuous);
  CHECK("fast frequency transition never resets oscillator phase", phaseContinuous);
  FreqTransitionState state;
  SelectGaitFrequencyTau(state, GaitRun, true, 3, 3, 1);
  CHECK("run to sprint selects fast frequency smoothing",
        Near(SelectGaitFrequencyTau(state, GaitSprint, true, 3, 3.6f, 1), .1f));
  CHECK("same gait resumes configured smoothing after transition settles",
        Near(SelectGaitFrequencyTau(state, GaitSprint, true, 3.59f, 3.6f, 1), 1));
  CHECK("later same-gait fitting changes retain configured smoothing",
        Near(SelectGaitFrequencyTau(state, GaitSprint, true, 3.6f, 3.8f, 1), 1));
  CHECK("stop or jump cancels frequency transition without changing configured tau",
        Near(SelectGaitFrequencyTau(state, GaitRun, false, 3, 3, 1), 1) && state.gait == GaitNone);
  CHECK("landing followed by sprint rearms fast cadence response",
        Near(SelectGaitFrequencyTau(state, GaitSprint, true, 3, 3.6f, 1), .1f));
  FreqTransitionState faster;
  CHECK("frequency transition respects an already faster user setting",
        Near(SelectGaitFrequencyTau(faster, GaitSprint, true, 3, 3.6f, .03f), .03f));
  LocomotionEnvelope stop;
  stop.freqEnv = 3.6f;
  for (int frame = 0; frame < 180; frame++) {
    float tau = SelectGaitFrequencyTau(state, GaitWalk, false, stop.freqEnv, stop.freqEnv, 1);
    stop.AdvanceDt(1.0f/60, 0, 0, stop.freqEnv, true, .15f, 1, tau);
  }
  CHECK("fast gait cadence policy leaves stop-release frequency frozen", stop.freqEnv == 3.6f);
}

static void TestFreqLock() {
  printf("[FREQ-LOCK]\n");
  const char *clip = "run_loop";
  GaitParam gp;  // defaults: freq 1.0, autoFrequency on, threshold 0.05
  gp.frequencyHz = 2.0f;

  const char *loops[] = {"walk_loop", "run_loop", "sprint_loop", "interact_zipline_sp_02"};
  const char *nonLoops[] = {"walk_start", "run_to_walk", "sprint_pivot_r",
      "run_bump", "interact_zipline_start", "idle_loop", "attack", "unknown_pose",
      "idle_jump_start_l", "run_stop"};
  for (int gait = GaitWalk; gait <= GaitZipline; gait++) {
    GaitTimingSample stable;
    stable.Observe(ClassifyClipNameFull(loops[gait - GaitWalk]));
    CHECK("each stable locomotion gait can publish timing", stable.CanTrack(gait));
    stable.Observe(ClassifyClipNameFull(loops[gait - GaitWalk]));
    CHECK("same-gait loop blends keep shared state timing", stable.CanTrack(gait));
    bool nonLoopsBlocked = true, foreignLoopsBlocked = true;
    for (const char *name : nonLoops) {
      GaitTimingSample blended = stable;
      blended.Observe(ClassifyClipNameFull(name));
      nonLoopsBlocked &= !blended.CanTrack(gait);
    }
    for (int other = GaitWalk; other <= GaitZipline; other++) {
      if (other == gait) continue;
      GaitTimingSample blended = stable;
      blended.Observe(ClassifyClipNameFull(loops[other - GaitWalk]));
      foreignLoopsBlocked &= !blended.CanTrack(gait);
    }
    CHECK("all gaits reject transition jump idle and unknown timing blends", nonLoopsBlocked);
    CHECK("all gaits reject timing from mixed gait loops", foreignLoopsBlocked);
  }
  CHECK("empty clip samples cannot publish timing", !GaitTimingSample().CanTrack(GaitRun));

  // All ordered gait pairs, including same configured Hz: cache ownership
  // is gait-specific, not a Walk exception or a frequency comparison.
  for (bool equalConfig : {false, true}) {
    for (bool fittedDestination : {false, true}) {
      bool targetsOwned = true, onsetOwned = true, cacheSurvives = true;
      bool switchesHonored = true, measurementConverges = true;
      for (int source = GaitWalk; source <= GaitZipline; source++) {
        for (int destination = GaitWalk; destination <= GaitZipline; destination++) {
          FreqCache cache;
          GaitParam from = gp, to = gp;
          from.frequencyHz = equalConfig ? 2.0f : 1.0f + source;
          to.frequencyHz = equalConfig ? 2.0f : 1.0f + destination;
          float fitFrom = from.frequencyHz * .9f;
          float fitTo = to.frequencyHz * .9f;
          cache.Put("chr_matrix", source, fitFrom, from.frequencyHz);
          if (fittedDestination)
            cache.Put("chr_matrix", destination, fitTo, to.frequencyHz);
          else if (source == destination)
            cache.Invalidate("chr_matrix", destination);
          FreqLockState state;
          FreqLockPrepare(state, from, source, cache, "chr_matrix");
          float expected = fittedDestination || source != destination ? fitFrom : from.frequencyHz;
          cacheSurvives &= Near(state.useHz, expected);
          float beforeSample = FreqLockTargetHz(state, to, destination);
          targetsOwned &= Near(beforeSample, source == destination ? state.useHz : to.frequencyHz);
          FreqLockPrepare(state, to, destination, cache, "chr_matrix");
          expected = fittedDestination ? fitTo : to.frequencyHz;
          onsetOwned &= Near(FreqLockTargetHz(state, to, destination), expected);
          FreqLockPause(state);  // start/transition clip cannot measure
          onsetOwned &= Near(FreqLockTargetHz(state, to, destination), expected) && !state.measValid;
          GaitParam manual = to;
          manual.autoFrequency = false;
          switchesHonored &= Near(FreqLockTargetHz(state, manual, destination), to.frequencyHz);
          for (int sample = 0; sample < 45; sample++) {
            float norm = fmodf(sample * .05f * fitTo / 2.0f, 1.0f);
            FreqLockTick(state, to, "stable_loop", norm, 1000 + sample * 50,
                         destination, cache, "chr_matrix");
          }
          measurementConverges &= state.measValid && Near(state.measHz, fitTo, .001f) &&
                                 fabsf(state.useHz - fitTo) / fitTo < .0251f;
        }
      }
      printf("  matrix: equal_config=%d fitted_destination=%d (16 gait pairs)\n",
             equalConfig, fittedDestination);
      CHECK("every gait target rejects a foreign gait fit", targetsOwned);
      CHECK("every gait start uses its own cache or configuration", onsetOwned);
      CHECK("gait matrix preserves available source cache", cacheSurvives);
      CHECK("every gait honors manual frequency override", switchesHonored);
      CHECK("every gait converges on its own loop measurement", measurementConverges);
    }
  }
  // A stalled/discontinuous clock must not keep correcting from stale EMA.
  for (int failure = 0; failure < 4; failure++) {
    FreqCache cache;
    FreqLockState state;
    for (int i = 0; i < 6; i++)
      FreqLockTick(state, gp, "run_loop", .06f * i, 1000 + 50 * i,
                   GaitRun, cache, "chr_test");
    float before = state.useHz;
    float norm = failure == 0 ? .30f : failure == 1 ? .90f :
                 failure == 2 ? std::numeric_limits<float>::quiet_NaN() : .40f;
    DWORD time = failure == 3 ? 2250 : 1300;
    bool phaseUsable = FreqLockTick(state, gp, "run_loop", norm, time, GaitRun, cache, "chr_test");
    CHECK("stalled invalid implausible or undersampled timing cannot advance a fit",
          !phaseUsable && !state.measValid && !state.correcting && state.devCount == 0 &&
          Near(state.useHz, before) && !cache.Find("chr_test", GaitRun));
    if (failure == 0) {
      bool remainsInvalid = true;
      for (int i = 1; i <= 6; i++)
        remainsInvalid &= !FreqLockTick(state, gp, "run_loop", norm, time + 50*i,
                                        GaitRun, cache, "chr_test");
      CHECK("persistent stalled clock never rearms a stale phase reference", remainsInvalid);
    }
  }

  // Regression: start clips previously borrowed the last gait's fitted Hz.
  {
    FreqCache cache;
    cache.Put("chr_test", GaitSprint, 3.6f, 3.4f);
    cache.Put("chr_test", GaitWalk, 1.85f, 2.0f);
    FreqLockState f;
    GaitParam sprint = gp;
    sprint.frequencyHz = 3.4f;
    FreqLockPrepare(f, sprint, GaitSprint, cache, "chr_test");
    CHECK("sprint cache selected", Near(f.useHz, 3.6f));
    CHECK("walk target rejects previous sprint fit before next sample",
          Near(FreqLockTargetHz(f, gp, GaitWalk), 2.0f));
    FreqLockPrepare(f, gp, GaitWalk, cache, "chr_test");
    CHECK("walk start selects its own fit without measuring a loop",
          Near(FreqLockTargetHz(f, gp, GaitWalk), 1.85f) && !f.measValid);
    GaitParam off = gp;
    off.autoFrequency = false;
    CHECK("disabled auto frequency ignores fitted target",
          Near(FreqLockTargetHz(f, off, GaitWalk), 2.0f));
    FreqLockTick(f, gp, "walk_loop", .10f, 1000, GaitWalk, cache, "chr_test");
    FreqLockTick(f, gp, "walk_loop", .15f, 1050, GaitWalk, cache, "chr_test");
    FreqLockTick(f, gp, "walk_loop", .20f, 1100, GaitWalk, cache, "chr_test");
    CHECK("loop timing established before pivot", f.measValid);
    FreqLockPause(f);
    CHECK("pivot clears timing baseline while retaining gait fit",
          !f.havePrev && !f.measValid && !f.correcting && Near(f.useHz, 1.85f));
    FreqLockTick(f, gp, "walk_loop", .40f, 3000, GaitWalk, cache, "chr_test");
    CHECK("resumed loop does not measure across pivot gap", !f.measValid);
    FreqLockTick(f, gp, "walk_loop", .45f, 3050, GaitWalk, cache, "chr_test");
    FreqLockTick(f, gp, "walk_loop", .50f, 3100, GaitWalk, cache, "chr_test");
    CHECK("resumed loop measures fresh deltas", f.measValid && Near(f.measHz, 2.0f));
  }

  // measurement + matching config: no correction at all
  {
    FreqLockState f;
    FreqCache cache;
    DWORD t = 1000;
    float norm = 0.0f;
    for (int i = 0; i < 20; i++) {  // phys 2.0 -> dNorm 0.05 per 50ms
      FreqLockTick(f, gp, clip, norm, t, GaitRun, cache, "chr_test");
      norm += 0.05f;
      if (norm >= 1.0f) norm -= 1.0f;  // also exercises the wrap path
      t += 50;
    }
    CHECK("measurement converges to matching frequency",
          f.measValid && Near(f.measHz, 2.0f, 0.1f));
    CHECK("matching frequency never corrects",
          !f.correcting && Near(f.useHz, 2.0f) && f.devCount == 0);
    CHECK("no fit means no cache entry",
          cache.Find("chr_test", GaitRun) == nullptr);
  }

  // sustained deviation (cfg 2.0 vs meas 2.2 = 9%) triggers correction,
  // converges, then freezes below threshold/2 and persists the fit
  {
    FreqLockState f;
    FreqCache cache;
    DWORD t = 2000;
    float norm = 0.0f;
    bool sawCorrecting = false;
    bool sawFreeze = false;
    for (int i = 0; i < 40; i++) {  // phys 2.2 -> dNorm 0.055 per 50ms
      FreqLockTick(f, gp, clip, norm, t, GaitRun, cache, "chr_test");
      norm += 0.055f;
      if (norm >= 1.0f) norm -= 1.0f;
      t += 50;
      if (f.correcting) sawCorrecting = true;
      if (sawCorrecting && !f.correcting) sawFreeze = true;
    }
    CHECK("sustained 9 percent deviation starts correcting", sawCorrecting);
    CHECK("correction converges and freezes near measured",
          sawFreeze && !f.correcting &&
              fabsf(f.useHz - 2.2f) < 0.11f);
    CHECK("corrected frequency differs from config",
          fabsf(f.useHz - 2.0f) > 0.1f);
    const float *cached = cache.Find("chr_test", GaitRun);
    CHECK("fitted frequency persisted to cache",
          cached != nullptr && fabsf(*cached - 2.2f) < 0.11f &&
              fabsf(*cached - f.useHz) < 1e-4f);
  }

  // cached fit is reused across clip switches (same character + gait); no
  // re-fit happens while the cached value matches the measurement
  {
    FreqCache cache;
    {
      FreqLockState f;
      DWORD t = 2100;
      float norm = 0.0f;
      for (int i = 0; i < 40; i++) {
        FreqLockTick(f, gp, clip, norm, t, GaitRun, cache, "chr_test");
        norm += 0.055f;
        if (norm >= 1.0f) norm -= 1.0f;
        t += 50;
      }
    }
    const float cachedHz = *cache.Find("chr_test", GaitRun);
    FreqLockState f2;
    DWORD t2 = 2200;
    float n2 = 0.0f;
    FreqLockTick(f2, gp, "run_loop_b", 0.0f, t2, GaitRun, cache, "chr_test");
    CHECK("clip switch reuses the cached frequency",
          Near(f2.useHz, cachedHz, 1e-4f) && f2.useHz != gp.frequencyHz);
    bool sawCorr = false;
    for (int i = 0; i < 20; i++) {
      FreqLockTick(f2, gp, "run_loop_b", n2, t2, GaitRun, cache, "chr_test");
      n2 += 0.055f;
      if (n2 >= 1.0f) n2 -= 1.0f;
      t2 += 50;
      if (f2.correcting) sawCorr = true;
    }
    CHECK("cached match requires no re-fit",
          !sawCorr && Near(f2.useHz, cachedHz, 1e-3f));
  }

  // per-character isolation: another character starts from config and gets
  // its own cache slot
  {
    FreqCache cache;
    FreqLockState f;
    DWORD t = 2300;
    float norm = 0.0f;
    FreqLockTick(f, gp, clip, 0.0f, t, GaitRun, cache, "chr_other");
    CHECK("other character starts from config frequency",
          Near(f.useHz, 2.0f));
    for (int i = 0; i < 40; i++) {
      FreqLockTick(f, gp, clip, norm, t, GaitRun, cache, "chr_other");
      norm += 0.055f;
      if (norm >= 1.0f) norm -= 1.0f;
      t += 50;
    }
    CHECK("per-character cache slots",
          cache.Find("chr_test", GaitRun) == nullptr &&
              cache.Find("chr_other", GaitRun) != nullptr);
  }

  // gait switch (different per-gait config frequencies) must NOT look like a
  // user config edit: the cache survives and the fitted value is reused when
  // returning to the gait
  {
    FreqCache cache;
    {
      FreqLockState f;
      DWORD t = 2500;
      float norm = 0.0f;
      for (int i = 0; i < 40; i++) {
        FreqLockTick(f, gp, clip, norm, t, GaitRun, cache, "chr_test");
        norm += 0.055f;
        if (norm >= 1.0f) norm -= 1.0f;
        t += 50;
      }
    }
    const float cachedRun = *cache.Find("chr_test", GaitRun);
    GaitParam gpWalk = gp;
    gpWalk.frequencyHz = 1.5f;  // walk has its own per-gait config frequency
    FreqLockState f;
    DWORD t = 2600;
    FreqLockTick(f, gpWalk, "walk_loop", 0.0f, t, GaitWalk, cache, "chr_test");
    CHECK("gait switch keeps the run cache",
          cache.Find("chr_test", GaitRun) != nullptr);
    CHECK("gait without cache starts from its own config",
          Near(f.useHz, 1.5f));
    FreqLockTick(f, gp, "run_loop", 0.0f, t + 50, GaitRun, cache, "chr_test");
    CHECK("returning to a gait reuses its cached fit",
          Near(f.useHz, cachedRun, 1e-4f));
    CHECK("no invalidation across gait switches",
        cache.Find("chr_test", GaitRun) != nullptr &&
            cache.Find("chr_test", GaitWalk) == nullptr);
  }

  // config frequency change invalidates the cached fit (user input wins)
  {
    FreqCache cache;
    {
      FreqLockState f;
      DWORD t = 2400;
      float norm = 0.0f;
      for (int i = 0; i < 40; i++) {
        FreqLockTick(f, gp, clip, norm, t, GaitRun, cache, "chr_test");
        norm += 0.055f;
        if (norm >= 1.0f) norm -= 1.0f;
        t += 50;
      }
    }
    CHECK("cache populated before invalidation",
          cache.Find("chr_test", GaitRun) != nullptr);
    GaitParam gpNew = gp;
    gpNew.frequencyHz = 2.6f;
    FreqLockState f2;
    DWORD t2 = 2450;
    float n2 = 0.0f;
    // establish the config baseline (2.0) on the old clip first
    for (int i = 0; i < 6; i++) {
      FreqLockTick(f2, gp, clip, n2, t2, GaitRun, cache, "chr_test");
      n2 += 0.055f;
      if (n2 >= 1.0f) n2 -= 1.0f;
      t2 += 50;
    }
    // switch coincides with the config change: user input must win
    FreqLockTick(f2, gpNew, "run_loop_c", n2, t2, GaitRun, cache, "chr_test");
    CHECK("config change alongside clip switch invalidates the cache",
          cache.Find("chr_test", GaitRun) == nullptr);
    CHECK("config change restarts from the new config",
          Near(f2.useHz, 2.6f));
  }

  // cache ring replacement: full cache keeps its cap and never overflows
  {
    FreqCache big;
    for (int i = 0; i < 40; i++) {
      char id[64];
      snprintf(id, sizeof(id), "chr_%02d", i);
      big.Put(id, GaitRun, 1.0f + i * 0.1f);
    }
    CHECK("cache caps at kFreqCacheMax", big.count == kFreqCacheMax);
    big.Put("chr_99", GaitWalk, 2.0f);
    big.Put("chr_98", GaitWalk, 2.1f);
    CHECK("ring replacement keeps the cap", big.count == kFreqCacheMax);
    CHECK("ring slot accepts new entries",
          big.Find("chr_99", GaitWalk) != nullptr &&
              big.Find("chr_98", GaitWalk) != nullptr);
    // invalidate compacts and frees a slot
    big.Invalidate("chr_00", GaitRun);
    CHECK("invalidate removes exactly one entry",
          big.count == kFreqCacheMax - 1 &&
              big.Find("chr_00", GaitRun) == nullptr);
  }

  // Runtime resets on hot reload; the cache must validate its own config
  // identity without relying on the discarded FreqLockState.cfgHz.
  {
    FreqCache cache;
    cache.Put("chr_test", GaitRun, 2.2f, gp.frequencyHz);
    cache.Put("chr_test", GaitWalk, 1.6f, 1.5f);
    FreqLockState reset;
    FreqLockTick(reset, gp, clip, 0.0f, 2900, GaitRun, cache, "chr_test");
    CHECK("unchanged config reuses fit after runtime reset", Near(reset.useHz, 2.2f));
    GaitParam changed = gp;
    changed.frequencyHz = 3.0f;
    reset = FreqLockState();
    FreqLockTick(reset, changed, clip, 0.0f, 2950, GaitRun, cache, "chr_test");
    CHECK("hot reload starts immediately from the changed config", Near(reset.useHz, 3.0f));
    CHECK("hot reload invalidates only the stale gait fit",
          !cache.Find("chr_test", GaitRun) && cache.Find("chr_test", GaitWalk));
    changed.frequencyHz = 1.8f;
    FreqLockTick(reset, changed, "walk_loop", 0.0f, 3000, GaitWalk, cache, "chr_test");
    CHECK("config edit to inactive gait also invalidates its fit",
          Near(reset.useHz, 1.8f) && !cache.Find("chr_test", GaitWalk));
  }

  // large threshold never triggers
  {
    FreqLockState f;
    FreqCache cache;
    GaitParam gpTh = gp;
    gpTh.freqDevThreshold = 0.5f;  // 50%
    DWORD t = 3000;
    float norm = 0.0f;
    for (int i = 0; i < 40; i++) {  // 9% deviation < 50% threshold
      FreqLockTick(f, gpTh, clip, norm, t, GaitRun, cache, "chr_test");
      norm += 0.055f;
      if (norm >= 1.0f) norm -= 1.0f;
      t += 50;
    }
    CHECK("deviation below a large threshold never corrects",
          !f.correcting && Near(f.useHz, 2.0f));
  }

  // auto_frequency off: measurement runs but correction never engages
  {
    FreqLockState f;
    FreqCache cache;
    GaitParam gpOff = gp;
    gpOff.autoFrequency = false;
    DWORD t = 4000;
    float norm = 0.0f;
    for (int i = 0; i < 40; i++) {  // meas 2.2 vs cfg 2.0 = 9%
      FreqLockTick(f, gpOff, clip, norm, t, GaitRun, cache, "chr_test");
      norm += 0.055f;
      if (norm >= 1.0f) norm -= 1.0f;
      t += 50;
    }
    CHECK("auto frequency off never corrects",
        !f.correcting && Near(f.useHz, 2.0f) && f.measValid);
  }

  // clip switch: measurement resets, then restarts on the new clip
  {
    FreqLockState f;
    FreqCache cache;
    DWORD t = 5000;
    float norm = 0.0f;
    for (int i = 0; i < 8; i++) {
      FreqLockTick(f, gp, clip, norm, t, GaitRun, cache, "chr_test");
      norm += 0.055f;
      if (norm >= 1.0f) norm -= 1.0f;
      t += 50;
    }
    CHECK("clip A measured", f.measValid);
    FreqLockTick(f, gp, "walk_loop", 0.0f, t, GaitRun, cache, "chr_test");
    CHECK("clip switch resets measurement and correction",
          !f.measValid && !f.correcting && Near(f.useHz, 2.0f));
    FreqLockTick(f, gp, "walk_loop", 0.04f, t + 50, GaitRun, cache, "chr_test");
    CHECK("re-arm sample has no delta yet", !f.measValid);
    FreqLockTick(f, gp, "walk_loop", 0.08f, t + 100, GaitRun, cache, "chr_test");
    CHECK("measurement restarts after switch", f.measValid);
  }

  // config frequency change (hot reload) restarts from the new config
  {
    FreqLockState f;
    FreqCache cache;
    DWORD t = 6000;
    float norm = 0.0f;
    for (int i = 0; i < 6; i++) {
      FreqLockTick(f, gp, clip, norm, t, GaitRun, cache, "chr_test");
      norm += 0.05f;
      if (norm >= 1.0f) norm -= 1.0f;
      t += 50;
    }
    gp.frequencyHz = 2.4f;
    FreqLockTick(f, gp, clip, norm, t, GaitRun, cache, "chr_test");
    CHECK("config change resets useHz to the new config",
          Near(f.useHz, 2.4f) && !f.correcting && f.devCount < kFreqDevSamples);
    gp.frequencyHz = 2.0f;
  }

  // invalid / implausible samples never produce a measurement
  {
    FreqLockState f;
    FreqCache cache;
    FreqLockTick(f, gp, clip, -1.0f, 7000, GaitRun, cache, "chr_test");
    FreqLockTick(f, gp, clip, -1.0f, 7050, GaitRun, cache, "chr_test");
    CHECK("invalid normalizedTime skips measurement", !f.measValid);
    FreqLockState f2;
    FreqLockTick(f2, gp, clip, 0.0f, 7100, GaitRun, cache, "chr_test");
    FreqLockTick(f2, gp, clip, 0.6f, 7150, GaitRun, cache, "chr_test");  // dNorm 0.6 > 0.5 rejected
    CHECK("implausible delta rejected", !f2.measValid);
  }

  // per-gait switch helpers
  CharacterProfile p;
  CHECK("default phase align on", GaitPhaseAlignEnabled(p, GaitRun));
  CHECK("default auto frequency on", GaitAutoFrequencyEnabled(p, GaitRun));
  CHECK("default dev threshold 0.05",
        Near(GaitFreqDevThreshold(p, GaitRun), 0.05f));
  p.run.phaseAlign = false;
  p.run.autoFrequency = false;
  p.run.freqDevThreshold = 0.1f;
  CHECK("per-gait overrides honored",
        !GaitPhaseAlignEnabled(p, GaitRun) &&
            !GaitAutoFrequencyEnabled(p, GaitRun) &&
            Near(GaitFreqDevThreshold(p, GaitRun), 0.1f));
  CHECK("other gaits keep defaults",
        GaitPhaseAlignEnabled(p, GaitWalk) &&
            GaitAutoFrequencyEnabled(p, GaitWalk) &&
            Near(GaitFreqDevThreshold(p, GaitWalk), 0.05f));
}

// ---------- transform recorder diagnostics contract ----------
static void TestTransformRecorderPolicy() {
  printf("[TRANSFORM RECORDER]\n");
  CHECK("recorder keeps at least a 120-second 60fps window",
        kTransformRecorderMaxFrames >= 7200);
  CHECK("recorder does not stop at the old 1800-frame boundary",
        !TransformRecorderShouldAutoStop(1800));
  CHECK("recorder stops at its configured long-window boundary",
        TransformRecorderShouldAutoStop(kTransformRecorderMaxFrames));

  char first[128] = {0};
  char second[128] = {0};
  BuildTransformRecorderFileName(first, sizeof(first), 2026, 9, 5, 12, 55,
                                 24, 100);
  BuildTransformRecorderFileName(second, sizeof(second), 2026, 9, 5, 12,
                                 55, 24, 101);
  CHECK("recorder session filenames are unique",
        first[0] && second[0] && strcmp(first, second) != 0);
  CHECK("recorder session filename is recognizable",
        strstr(first, "breast_record_20260905_125524_100.csv") != nullptr);

  CHECK("recorder identifies stop cancellation and the sampled clips",
        strstr(kTransformRecorderCsvHeader,
               "stop_armed,stop_blocked,stop_seen,release_tail,dominant_clip,blocked_clip") != nullptr);
  FILE *csvText = tmpfile();
  CHECK("recorder text test opens a temporary stream", csvText != nullptr);
  if (csvText) {
    bool written = TransformRecorderCsvText(csvText, "clip,\"quoted\"");
    rewind(csvText);
    char quoted[64] = {0};
    fread(quoted, 1, sizeof(quoted) - 1, csvText);
    CHECK("recorder quotes commas and embedded quotes in animation names",
          written && strcmp(quoted, "\"clip,\"\"quoted\"\"\"") == 0);
    fclose(csvText);
  }

  CHECK("recorder CSV carries pre-write pose",
        strstr(kTransformRecorderCsvHeader, "preRx,preRy,preRz,preRw") !=
            nullptr);
  CHECK("recorder CSV carries intended target",
        strstr(kTransformRecorderCsvHeader,
               "targetRx,targetRy,targetRz,targetRw") != nullptr);
  CHECK("recorder CSV carries actual post-write pose",
        strstr(kTransformRecorderCsvHeader,
               "actualRx,actualRy,actualRz,actualRw") != nullptr);
  CHECK("recorder CSV carries oscillator state",
        strstr(kTransformRecorderCsvHeader,
               "phase_rad,amp_env_deg,down_env_deg,freq_env_hz,synthetic_angle_deg") !=
            nullptr);
  CHECK("recorder CSV carries breast world rotation and position",
        strstr(kTransformRecorderCsvHeader,
               "worldRotRx,worldRotRy,worldRotRz,worldRotRw") != nullptr &&
            strstr(kTransformRecorderCsvHeader,
                   "worldPosRx,worldPosRy,worldPosRz") != nullptr);
  CHECK("recorder CSV carries parent local and world rotation",
        strstr(kTransformRecorderCsvHeader,
               "parentLocalRx,parentLocalRy,parentLocalRz,parentLocalRw") !=
                nullptr &&
            strstr(kTransformRecorderCsvHeader,
                   "parentWorldRx,parentWorldRy,parentWorldRz,parentWorldRw") !=
                nullptr);
  CHECK("recorder CSV carries grandparent world rotation",
        strstr(kTransformRecorderCsvHeader,
               "grandWorldRx,grandWorldRy,grandWorldRz,grandWorldRw") !=
            nullptr);
}

int main() {
  printf("=== SecondaryMotion V2 verification ===\n\n");
  TestJson();
  TestDatabase();
  TestGait();
  TestEnvelope();
  TestQuat();
  TestSyntheticBaseFilter();
  TestHotReload();
  TestConfigValidation();
  TestRuntimePaths();
  TestBoneDumpJson();
  TestDevCommandRevision();
  TestJumpPhaseAProbe();
  TestJumpInertialSource();
  TestFreqLock();
  TestGaitFrequencyTransition();
  TestStopRelease();
  TestTransformRecorderPolicy();

  printf("\n=== RESULT: %d passed, %d failed ===\n", g_pass, g_fail);
  if (g_fail == 0) {
    printf("VERIFICATION: PASS\n");
    return 0;
  }
  printf("VERIFICATION: FAIL\n");
  return 1;
}
