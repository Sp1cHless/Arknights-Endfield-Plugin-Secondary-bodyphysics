"""Measure oscillator cadence from recorded phase, including PLL corrections."""
import argparse
import csv
import itertools
import json
import math
import statistics
from pathlib import Path


def analyze(capture, output):
    with capture.open(encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError("Empty capture")
    output.mkdir(parents=True, exist_ok=True)
    origin = int(rows[0]["time_ms"])
    times = [(int(row["time_ms"]) - origin) / 1000 for row in rows]
    phases = [float(row["phase_rad"]) for row in rows]
    unwrapped = [phases[0]]
    for previous, current in zip(phases, phases[1:]):
        unwrapped.append(unwrapped[-1] + (current - previous + math.pi) % math.tau - math.pi)
    actual_hz = []
    left = 0
    for index, time in enumerate(times):
        while left + 1 < index and times[left + 1] <= time - 0.25:
            left += 1
        duration = time - times[left]
        actual_hz.append((unwrapped[index] - unwrapped[left]) / (math.tau * duration)
                         if duration > 0.08 else float("nan"))
    epochs = []
    walking = lambda index: rows[index]["gait"] == "1" and rows[index]["transition_to_idle"] == "0"
    for active, grouped in itertools.groupby(range(len(rows)), key=walking):
        indices = list(grouped)
        if not active or times[indices[-1]] - times[indices[0]] < 4:
            continue
        start = times[indices[0]]
        bins = []
        for bucket, values in itertools.groupby(indices, key=lambda index: int((times[index] - start) / 0.5)):
            values = [index for index in values if math.isfinite(actual_hz[index])]
            if values:
                bins.append({"elapsed": bucket * 0.5,
                             "phase_hz": statistics.median(actual_hz[index] for index in values),
                             "envelope_hz": statistics.median(float(rows[index]["freq_env_hz"]) for index in values),
                             "clip": rows[values[len(values)//2]]["dominant_clip"]})
        epochs.append({"start": start, "end": times[indices[-1]], "half_second_bins": bins})
    (output / "summary.json").write_text(json.dumps(epochs, indent=2), encoding="utf-8")
    for epoch in epochs:
        print(f"Walk {epoch['start']:.2f}..{epoch['end']:.2f}s")
        for bucket in epoch["half_second_bins"]:
            print(f"  +{bucket['elapsed']:4.1f}s actual={bucket['phase_hz']:.2f}Hz envelope={bucket['envelope_hz']:.2f}Hz {bucket['clip']}")
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("Plot skipped: matplotlib is unavailable")
        return
    if epochs:
        fig, axes = plt.subplots(len(epochs), 1, figsize=(12, 3.3 * len(epochs)), squeeze=False)
        for axis, epoch in zip(axes[:, 0], epochs):
            lower, upper = epoch["start"], epoch["end"]
            indices = [index for index, time in enumerate(times) if lower <= time <= upper]
            relative = [times[index] - lower for index in indices]
            axis.plot(relative, [actual_hz[index] for index in indices], label="Actual phase cadence (250ms)")
            axis.plot(relative, [float(rows[index]["freq_env_hz"]) for index in indices], label="Frequency envelope")
            axis.set_title(f"Walk at capture {lower:.2f}s")
            axis.set_ylabel("Hz")
            axis.set_xlabel("Seconds since walking started")
            axis.grid(alpha=0.2)
            axis.legend(loc="upper right")
        fig.tight_layout()
        fig.savefig(output / "walk_frequency.png", dpi=150)
        plt.close(fig)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--output", type=Path, default=Path("artifacts/gait-frequency"))
    args = parser.parse_args()
    analyze(args.capture, args.output)
