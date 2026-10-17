#pragma once
// diagnostics/transform_recorder.h — long-window CSV recorder for locating
// local/native composition, parent-chain/world-space motion, self-feedback,
// and post-write mismatch. Worker-triggered; Unity Transform reads remain on
// the animation callback.
#include <cstdio>
#include "../character/active_character.h"
#include "../common/logger.h"
#include "../common/safe_unity.h"
#include "../runtime/runtime_paths.h"
#include "transform_recorder_policy.h"

static FILE *g_recFile = nullptr;
static int g_recFrames = 0;
static char g_recSessionPath[512] = {0};
static void *g_recBoneR = nullptr;
static void *g_recBoneL = nullptr;
static void *g_recParentR = nullptr;
static void *g_recParentL = nullptr;
static void *g_recGrandR = nullptr;
static void *g_recGrandL = nullptr;

static void TransformRecorderResetHierarchy() {
  g_recBoneR = nullptr;
  g_recBoneL = nullptr;
  g_recParentR = nullptr;
  g_recParentL = nullptr;
  g_recGrandR = nullptr;
  g_recGrandL = nullptr;
}

static void TransformRecorderResolveHierarchy(
    const ActiveCharacterRuntime &c) {
  if (g_recBoneR == c.bones.breastR && g_recBoneL == c.bones.breastL) return;
  g_recBoneR = c.bones.breastR;
  g_recBoneL = c.bones.breastL;
  g_recParentR = SafeGetParent(g_recBoneR);
  g_recParentL = SafeGetParent(g_recBoneL);
  g_recGrandR = SafeGetParent(g_recParentR);
  g_recGrandL = SafeGetParent(g_recParentL);

  char parentR[128] = {0}, parentL[128] = {0};
  char grandR[128] = {0}, grandL[128] = {0};
  SafeGetObjectName(g_recParentR, parentR, sizeof(parentR));
  SafeGetObjectName(g_recParentL, parentL, sizeof(parentL));
  SafeGetObjectName(g_recGrandR, grandR, sizeof(grandR));
  SafeGetObjectName(g_recGrandL, grandL, sizeof(grandL));
  ProbeLog("[REC] hierarchy parentR=%s parentL=%s grandR=%s grandL=%s\n",
           parentR, parentL, grandR, grandL);
}

static void TransformRecorderAppendQuat(Quat q) {
  fprintf(g_recFile, ",%.6f,%.6f,%.6f,%.6f", q.x, q.y, q.z, q.w);
}

static void TransformRecorderAppendVec3(Vec3 v) {
  fprintf(g_recFile, ",%.6f,%.6f,%.6f", v.x, v.y, v.z);
}

static void TransformRecorderClose(const char *reason) {
  if (!g_recFile) return;
  fflush(g_recFile);
  fclose(g_recFile);
  g_recFile = nullptr;

  char latestPath[512] = {0};
  RuntimePath(latestPath, sizeof(latestPath), kTransformRecorderLatestFile);
  BOOL copied = CopyFileA(g_recSessionPath, latestPath, FALSE);
  ProbeLog("[REC] %s frames=%d session=%s latest=%s copy=%d\n",
           reason ? reason : "stopped", g_recFrames, g_recSessionPath,
           latestPath, copied ? 1 : 0);
  g_recSessionPath[0] = 0;
  TransformRecorderResetHierarchy();
}

static bool TransformRecorderEnsureInit(const ActiveCharacterRuntime &c) {
  (void)c;
  if (g_recFile) return true;
  if (!RuntimePathsInit()) {
    ProbeLog("[REC] start failed: runtime root unavailable\n");
    return false;
  }

  char dir[512] = {0};
  RuntimePath(dir, sizeof(dir), "developer");
  CreateDirectoryA(dir, nullptr);

  SYSTEMTIME st = {};
  GetLocalTime(&st);
  char fileName[128] = {0};
  BuildTransformRecorderFileName(
      fileName, sizeof(fileName), st.wYear, st.wMonth, st.wDay, st.wHour,
      st.wMinute, st.wSecond, static_cast<unsigned long>(GetTickCount()));
  snprintf(g_recSessionPath, sizeof(g_recSessionPath), "%s\\%s", dir,
           fileName);

  g_recFile = fopen(g_recSessionPath, "w");
  if (g_recFile) {
    fprintf(g_recFile, "%s\n", kTransformRecorderCsvHeader);
    g_recFrames = 0;
    TransformRecorderResetHierarchy();
    ProbeLog("[REC] session=%s max_frames=%d\n", g_recSessionPath,
             kTransformRecorderMaxFrames);
    return true;
  }

  ProbeLog("[REC] start failed: cannot open %s\n", g_recSessionPath);
  g_recSessionPath[0] = 0;
  return false;
}

static void TransformRecorderFrame(const ActiveCharacterRuntime &c) {
  if (!g_recFile || !c.bones.breastR || !c.bones.breastL) return;
  if (TransformRecorderShouldAutoStop(g_recFrames)) {
    TransformRecorderClose("auto-stop");
    return;
  }

  const SyntheticRuntime &s = c.synthetic;
  TransformRecorderResolveHierarchy(c);

  Quat actualR = SafeGetLocalRotation(c.bones.breastR);
  Quat actualL = SafeGetLocalRotation(c.bones.breastL);
  Quat worldRotR = SafeGetWorldRotation(c.bones.breastR);
  Quat worldRotL = SafeGetWorldRotation(c.bones.breastL);
  Vec3 worldPosR = SafeGetWorldPosition(c.bones.breastR);
  Vec3 worldPosL = SafeGetWorldPosition(c.bones.breastL);
  Quat parentLocalR = SafeGetLocalRotation(g_recParentR);
  Quat parentLocalL = SafeGetLocalRotation(g_recParentL);
  Quat parentWorldR = SafeGetWorldRotation(g_recParentR);
  Quat parentWorldL = SafeGetWorldRotation(g_recParentL);
  Quat grandWorldR = SafeGetWorldRotation(g_recGrandR);
  Quat grandWorldL = SafeGetWorldRotation(g_recGrandL);

  fprintf(
      g_recFile,
      "%d,%lu,%d,%d,%.9f,%.6f,%.6f,%.6f,%.6f,%s,%d,%.3f,"
      "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
      "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,"
      "%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f",
      g_recFrames, NowMs(), c.currentGait, s.targetValid ? 1 : 0, s.phase,
      RadToDeg(s.ampEnv), RadToDeg(s.downEnv), s.freqEnv,
      RadToDeg(s.outAngleRad), c.characterId, static_cast<int>(c.axis),
      c.axisSign,
      s.lastNativeR.x, s.lastNativeR.y, s.lastNativeR.z, s.lastNativeR.w,
      s.lastNativeL.x, s.lastNativeL.y, s.lastNativeL.z, s.lastNativeL.w,
      s.targetR.x, s.targetR.y, s.targetR.z, s.targetR.w,
      s.targetL.x, s.targetL.y, s.targetL.z, s.targetL.w,
      actualR.x, actualR.y, actualR.z, actualR.w,
      actualL.x, actualL.y, actualL.z, actualL.w);
  TransformRecorderAppendQuat(worldRotR);
  TransformRecorderAppendQuat(worldRotL);
  TransformRecorderAppendVec3(worldPosR);
  TransformRecorderAppendVec3(worldPosL);
  TransformRecorderAppendQuat(parentLocalR);
  TransformRecorderAppendQuat(parentLocalL);
  TransformRecorderAppendQuat(parentWorldR);
  TransformRecorderAppendQuat(parentWorldL);
  TransformRecorderAppendQuat(grandWorldR);
  TransformRecorderAppendQuat(grandWorldL);
  fprintf(g_recFile, ",%lu,%d,%d,%d,%d,%d,%d,",
          s.clipSampleMs, s.clipReadOk ? 1 : 0,
          c.transitionToIdle ? 1 : 0, s.stopRelease.armed ? 1 : 0,
          s.stopRelease.blocked ? 1 : 0, s.stopRelease.wasStop ? 1 : 0,
          s.releaseTail ? 1 : 0);
  TransformRecorderCsvText(g_recFile, s.dominantClip);
  fputc(',', g_recFile);
  TransformRecorderCsvText(g_recFile, s.releaseBlockedClip);
  fputc('\n', g_recFile);

  g_recFrames++;
  if ((g_recFrames % 300) == 0) {
    fflush(g_recFile);
    ProbeLog("[REC] frames=%d\n", g_recFrames);
  }
}

static void TransformRecorderStop() {
  TransformRecorderClose("marker-stop");
}
