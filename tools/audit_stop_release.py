"""Compare stop-envelope candidates against captured stop-window durations.

Predictions use captured oscillator amplitude/cadence and preset tau; they
are offline envelope comparisons, not measurements of a newly installed DLL.
"""
import argparse
import csv
import itertools
import json
import math
from pathlib import Path


def release_gain(elapsed, tau, mode):
    elapsed = max(0.0, elapsed)
    if mode == "exponential":
        return math.exp(-elapsed / tau)
    if mode == "quadratic":
        return max(0.0, 1.0 - elapsed / (2 * tau)) ** 2
    # Preserve exponential decay down to half amplitude, then match its
    # value/slope with a finite quadratic tail. Endpoint and slope are zero.
    handoff = math.log(2) * tau
    if elapsed <= handoff:
        return math.exp(-elapsed / tau)
    return 0.5 * max(0.0, 1.0 - (elapsed - handoff) / (2 * tau)) ** 2


def analyze(captures, preset_path, output):
    preset = json.loads(preset_path.read_text(encoding="utf-8-sig"))
    parameters = {}
    preset_windows = []
    for character, node in preset.get("characters", {}).items():
        values = node.get("defaults", node)
        tau = values.get("envelope", {}).get("to_idle_release_tau_sec")
        if tau is not None and tau > 0:
            parameters[character] = tau
            amplitude = max((max(gait.get("amplitude_deg", 0), gait.get("amplitude_down_deg", 0))
                             for gait in values.get("gait", {}).values()), default=0)
            preset_windows.append({"character": character, "tau_sec": tau,
                "finite_release_sec": (math.log(2) + 2) * tau,
                "max_configured_amplitude_deg": amplitude,
                "last_frame_envelope_bound_deg": {
                    str(fps): amplitude * release_gain((math.log(2)+2)*tau - 1/fps,
                                                       tau, "half_then_quadratic")
                    for fps in (20, 30, 60, 144)}})
    events = []
    for capture in captures:
        with capture.open(encoding="utf-8-sig", newline="") as stream:
            rows = list(csv.DictReader(stream))
        origin = int(rows[0]["time_ms"])
        for stopping, grouped in itertools.groupby(
                range(len(rows)), key=lambda i: rows[i]["transition_to_idle"] == "1"):
            indices = list(grouped)
            if not stopping or indices[-1] + 1 >= len(rows):
                continue
            first, after = indices[0], indices[-1] + 1
            row = rows[first]
            tau = parameters.get(row["character_id"])
            if tau is None:
                continue
            wall_duration = (int(rows[after]["time_ms"]) - int(row["time_ms"])) / 1000
            intervals = [(int(rows[i + 1]["time_ms"]) - int(rows[i]["time_ms"])) / 1000
                         for i in indices]
            # The runtime caps integration intervals at 50ms. A long menu/
            # pause gap with a retained stop flag is not a long animation.
            duration = sum(min(.05, dt) for dt in intervals)
            if duration < 0.1:
                continue
            initial = rows[max(0, first - 1)]
            amplitude = max(float(initial["amp_env_deg"]), float(initial["down_env_deg"]))
            events.append({
                "capture": str(capture), "start_sec": (int(row["time_ms"]) - origin) / 1000,
                "clip": row["dominant_clip"], "character": row["character_id"],
                "stop_window_wall_sec": wall_duration,
                "stop_window_integrated_sec_estimate": duration,
                "capped_intervals": sum(dt > .05 for dt in intervals), "tau_sec": tau,
                "initial_max_amplitude_deg": amplitude,
                "cadence_hz": float(initial["freq_env_hz"]),
                "initial_phase_rad": float(initial["phase_rad"]),
                "predicted_amplitude_at_stop_window_end_deg": {
                    mode: amplitude * release_gain(duration, tau, mode)
                    for mode in ("exponential", "quadratic", "half_then_quadratic")},
                "quadratic_finishes_before_stop_window_end": 2 * tau < duration,
                "half_then_quadratic_finishes_before_stop_window_end": (math.log(2) + 2) * tau < duration,
            })
    report = {"kind": "offline envelope comparison", "preset_taus": parameters,
              "preset_release_windows": preset_windows,
              "normalized_gain": [
                  {"time_in_tau": time, **{mode: release_gain(time, 1, mode)
                   for mode in ("exponential", "quadratic", "half_then_quadratic")}}
                  for time in (0, .25, .5, 1, 1.5, 2, 2.5, 2 + math.log(2), 3)],
              "events": events}
    output.mkdir(parents=True, exist_ok=True)
    (output / "summary.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("Plot skipped: matplotlib is unavailable")
        return report
    figure, axes = plt.subplots(2, 1, figsize=(11, 7))
    times = [index / 200 for index in range(701)]
    modes = (("exponential", "Previous exponential"),
             ("quadratic", "Earlier 2-tau proposal"),
             ("half_then_quadratic", "Revised finite tail"))
    for mode, label in modes:
        axes[0].plot(times, [100 * release_gain(time, 1, mode) for time in times], label=label)
    axes[0].set(xlabel="Elapsed / tau", ylabel="Remaining amplitude (%)",
                title="Offline curve comparison: preserve onset, shorten the tail")
    axes[0].axvline(math.log(2), color="gray", linestyle=":", label="Half-amplitude handoff")
    sample = next((event for event in reversed(events) if "walk_stop" in event["clip"]), None)
    if sample:
        tau, hz = sample["tau_sec"], sample["cadence_hz"]
        phase, amplitude = sample["initial_phase_rad"], sample["initial_max_amplitude_deg"]
        for mode, label in modes:
            angles = [amplitude * release_gain(time*tau, tau, mode) *
                      math.sin(phase + math.tau*hz*time*tau) for time in times]
            axes[1].plot([time*tau for time in times], angles, label=label)
        axes[1].axvline(sample["stop_window_integrated_sec_estimate"], color="gray", linestyle=":",
                        label="Recorded stop flag ends (integrated-time estimate)")
        axes[1].set(xlabel="Seconds after stop", ylabel="Modeled oscillator offset (deg)",
                    title=f"Captured walk-stop parameters: tau={tau:g}s, cadence={hz:.2f}Hz")
    for axis in axes:
        axis.grid(alpha=.2)
        axis.legend(loc="upper right", fontsize=8)
    figure.tight_layout()
    figure.savefig(output / "comparison.png", dpi=150)
    plt.close(figure)
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", nargs="+", type=Path)
    parser.add_argument("--preset", type=Path, default=Path("SecondaryMotion/presets/Default.json"))
    parser.add_argument("--output", type=Path, default=Path("artifacts/stop-release-audit"))
    args = parser.parse_args()
    analyze(args.captures, args.preset, args.output)
