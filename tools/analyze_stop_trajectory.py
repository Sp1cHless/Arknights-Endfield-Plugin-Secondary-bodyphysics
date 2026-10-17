"""Inspect a recorded stop trajectory without loading or modifying the game."""
import argparse
import collections
import csv
import json
import math
from pathlib import Path


def quat(row, prefix):
    return tuple(float(row[prefix + axis]) for axis in "xyzw")


def similarity(a, b):
    norm = math.sqrt(sum(x * x for x in a) * sum(x * x for x in b))
    return min(1.0, abs(sum(x * y for x, y in zip(a, b))) / norm) if norm else 0.0


def distance(a, b):
    return math.degrees(2 * math.acos(similarity(a, b)))


def analyze(path, output):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.DictReader(stream))
    if not rows or "stop_armed" not in rows[0]:
        raise ValueError("Capture is empty or lacks stop-release trace columns")
    output.mkdir(parents=True, exist_ok=True)
    origin = int(rows[0]["time_ms"])
    times = [(int(row["time_ms"]) - origin) / 1000 for row in rows]
    edges = []
    feedback = []
    segments = []
    start = 0
    for index, row in enumerate(rows):
        if index and row["dominant_clip"] != rows[index - 1]["dominant_clip"]:
            segments.append({"start": times[start], "end": times[index - 1],
                             "clip": rows[start]["dominant_clip"]})
            start = index
        if not index:
            continue
        previous = rows[index - 1]
        if previous["target_valid"] == "1" and row["target_valid"] == "0":
            edges.append({"time": times[index], "clip": row["dominant_clip"],
                          "amp": float(row["amp_env_deg"]),
                          "down": float(row["down_env_deg"]),
                          "angle": float(row["synthetic_angle_deg"]),
                          "armed": row["stop_armed"], "blocked": row["stop_blocked"],
                          "transition": row["transition_to_idle"],
                          "blocker": row["blocked_clip"],
                          "actual_step": distance(quat(row, "actualR"), quat(previous, "actualR"))})
        if row["target_valid"] != "1" or previous["target_valid"] != "1":
            continue
        if row["transition_to_idle"] != "1" and row["release_tail"] != "1":
            continue
        if (similarity(quat(row, "preR"), quat(previous, "targetR")) > 0.99995
                and similarity(quat(row, "preL"), quat(previous, "targetL")) > 0.99995):
            feedback.append({"time": times[index], "clip": row["dominant_clip"],
                             "angle": float(row["synthetic_angle_deg"]),
                             "target_step": distance(quat(row, "targetR"), quat(previous, "targetR"))})
    segments.append({"start": times[start], "end": times[-1], "clip": rows[start]["dominant_clip"]})
    report = {"source": str(path.resolve()), "frames": len(rows), "duration": times[-1],
              "write_cutoffs": edges, "readback_candidate_frames": len(feedback),
              # Near-equal read-back poses are candidates, not proof of accumulation.
              "largest_readback_steps": sorted(feedback, key=lambda item: item["target_step"], reverse=True)[:12],
              "clip_segments": segments,
              "stop_flags": dict(collections.Counter(
                  f"transition={r['transition_to_idle']},seen={r['stop_seen']},armed={r['stop_armed']},blocked={r['stop_blocked']},tail={r['release_tail']}"
                  for r in rows))}
    (output / "summary.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps({key: value for key, value in report.items() if key not in ("clip_segments", "stop_flags")}, indent=2))
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("Plot skipped: matplotlib is unavailable")
        return
    fig, axes = plt.subplots(3, 1, figsize=(15, 9), sharex=True)
    for field, label in (("amp_env_deg", "Up envelope"), ("down_env_deg", "Down envelope"),
                         ("synthetic_angle_deg", "Synthetic angle")):
        axes[0].plot(times, [float(r[field]) for r in rows], label=label, linewidth=0.8)
    axes[0].set_ylabel("Degrees")
    axes[0].legend(loc="upper right")
    for field in ("target_valid", "transition_to_idle", "stop_armed", "stop_blocked", "release_tail"):
        axes[1].step(times, [int(r[field]) for r in rows], label=field, linewidth=0.8, where="post")
    axes[1].legend(loc="upper right", ncol=3)
    actual_steps = [0.0] + [distance(quat(rows[i], "actualR"), quat(rows[i-1], "actualR"))
                            for i in range(1, len(rows))]
    axes[2].plot(times, actual_steps, label="Actual right-bone rotation per frame", linewidth=0.7)
    axes[2].set_ylabel("Degrees / frame")
    axes[2].set_xlabel("Seconds since capture start")
    axes[2].legend(loc="upper right")
    for ax in axes:
        ax.grid(alpha=0.2)
    fig.tight_layout()
    fig.savefig(output / "trajectory.png", dpi=150)
    plt.close(fig)
    print("Plot:", output / "trajectory.png")
    sprint_feedback = [item for item in feedback if "sprint_stop" in item["clip"]]
    windows = []
    if edges:
        windows.append(("First write cutoff", edges[0]["time"] - 4, edges[0]["time"] + 4))
    if sprint_feedback:
        peak = max(sprint_feedback, key=lambda item: item["target_step"])
        windows.append(("Sprint stop: interrupted writes", peak["time"] - 2, peak["time"] + 5))
    if windows:
        fig, zoom_axes = plt.subplots(3, len(windows), figsize=(7 * len(windows), 8), squeeze=False)
        for column, (title, lower, upper) in enumerate(windows):
            zoom_axes[0, column].set_title(title)
            for field in ("amp_env_deg", "down_env_deg", "synthetic_angle_deg"):
                zoom_axes[0, column].plot(times, [float(r[field]) for r in rows], label=field, linewidth=1)
            for field in ("target_valid", "stop_armed", "transition_to_idle"):
                zoom_axes[1, column].step(times, [int(r[field]) for r in rows], label=field, where="post")
            zoom_axes[2, column].plot(times, actual_steps, label="Actual rotation per frame", linewidth=1)
            for axis in zoom_axes[:, column]:
                axis.set_xlim(max(0, lower), upper)
                axis.grid(alpha=0.2)
                axis.legend(loc="upper right", fontsize=8)
            zoom_axes[0, column].set_ylabel("Degrees")
            zoom_axes[2, column].set_ylabel("Degrees / frame")
            zoom_axes[2, column].set_xlabel("Seconds since capture start")
        fig.tight_layout()
        fig.savefig(output / "stop_details.png", dpi=150)
        plt.close(fig)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path, default=Path("artifacts/stop-trajectory"))
    args = parser.parse_args()
    analyze(args.capture, args.output)
