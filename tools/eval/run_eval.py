#!/usr/bin/env python3
"""End-to-end accuracy evaluation on synthetic deliveries.

Generates scenes (or reuses a folder of them), runs pocketeye-cli on every
video, matches the reported delivery to the ground truth and summarises the
errors. Optionally writes a Markdown report and fails if the errors exceed
the documented limits (CI gate).

    python3 tools/eval/run_eval.py --generate 40 --seed 7 --work out/eval \
        --report docs/accuracy.md --gate
"""
from __future__ import annotations

import argparse
import json
import math
import os
import statistics
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "synth"))

# metric key in the engine JSON -> (key in gt.json, kind, unit)
METRICS = {
    "release_speed_kmh": ("release_speed_kmh", "rel", "%"),
    "average_speed_kmh": ("average_speed_kmh", "rel", "%"),
    "swing_cm": ("swing_cm", "abs", "cm"),
    "turn_deg": ("turn_deg", "abs", "deg"),
    "bounce_length_m": ("length_m", "abs", "m"),
    "bounce_line_cm": ("bounce_line_cm", "abs", "cm"),
    "height_at_stumps_cm": ("height_at_stumps_cm", "abs", "cm"),
    "line_at_stumps_cm": ("line_at_stumps_cm", "abs", "cm"),
    "impact_line_cm": ("impact_line_cm", "abs", "cm"),
}

# CI gate: median and 90th-percentile limits per preset (generous first
# versions; tightened as the engine improves).
GATES = {
    "720p30": {"found_rate": 0.75, "wrong_lbw_rate": 0.10,
               "release_speed_kmh": (8, 20), "line_at_stumps_cm": (6, 20), "bounce_length_m": (0.6, 1.5)},
    "1080p60": {"found_rate": 0.8, "wrong_lbw_rate": 0.08,
                "release_speed_kmh": (6, 15), "line_at_stumps_cm": (5, 15), "bounce_length_m": (0.5, 1.2)},
}


def run_cli(cli: str, scene: str) -> dict | None:
    out = os.path.join(scene, "result.json")
    cmd = [cli, "analyze", "--video", os.path.join(scene, "video.mp4"), "--config",
           os.path.join(scene, "config.json"), "--timestamps", os.path.join(scene, "timestamps.txt"),
           "--out", out]
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
    if p.returncode != 0:
        return {"error": p.stderr.strip()}
    with open(out) as f:
        return json.load(f)


def pick_delivery(result: dict, gt: dict):
    t0 = gt["t_release"]
    t1 = gt.get("t_impact", t0 + 1.5)
    best = None
    for d in result.get("deliveries", []):
        if not d["valid"]:
            continue
        a, b = d["time_s"]
        if b < t0 or a > t1 + 0.5:
            continue
        if best is None or len(d["observations"]) > len(best["observations"]):
            best = d
    return best


def evaluate_scene(scene: str, result: dict) -> dict:
    gt = json.load(open(os.path.join(scene, "gt.json")))
    row = {"scene": os.path.basename(scene), "ball": gt["scene"]["delivery"]["ball"],
           "preset": gt["scene"]["preset"], "errors": {}, "covered": {}}
    if "error" in result:
        row["status"] = "cli error: " + result["error"][:200]
        return row
    row["calib_rms_px"] = result["calibration"]["rms_px"]
    row["timings"] = result["timings_ms"]
    row["false_deliveries"] = 0
    d = pick_delivery(result, gt)
    for x in result.get("deliveries", []):
        if x["valid"] and x is not d:
            row["false_deliveries"] += 1
    if d is None:
        row["status"] = "missed"
        return row
    row["status"] = "found"
    row["analysis_ms"] = d["timings_ms"].get("analysis_wall")
    row["has_bounce"] = (d["has_bounce"], gt["has_bounce"])
    for k, (gk, kind, _) in METRICS.items():
        m = d["metrics"].get(k)
        if m is None or gk not in gt:
            continue
        truth = gt[gk]
        err = m["value"] - truth
        if kind == "rel":
            err = 100.0 * err / truth
        row["errors"][k] = err
        row["covered"][k] = m["lo"] - 1e-6 <= truth <= m["hi"] + 1e-6
    if "lbw" in gt:
        lbw = d["lbw"]
        row["lbw_truth"] = gt["lbw"]["decision"]
        row["lbw_engine"] = lbw["decision"] if lbw["applicable"] else "n/a (" + lbw["impact_kind"] + ")"
        row["lbw_margins"] = gt["lbw"]["margins_m"]
        row["lbw_components"] = {c: (lbw[c]["status"] if lbw.get(c) else None, gt["lbw"][c])
                                 for c in ("pitching", "impact", "wickets")}
    return row


def pct(values, q):
    v = sorted(values)
    if not v:
        return float("nan")
    i = min(len(v) - 1, max(0, int(math.ceil(q * len(v))) - 1))
    return v[i]


def summarise(rows: list[dict]) -> dict:
    found = [r for r in rows if r["status"] == "found"]
    s = {"scenes": len(rows), "found": len(found),
         "found_rate": len(found) / max(len(rows), 1),
         "false_deliveries": sum(r.get("false_deliveries", 0) for r in rows),
         "metrics": {}}
    for k, (_, _, unit) in METRICS.items():
        errs = [abs(r["errors"][k]) for r in found if k in r["errors"]]
        cov = [r["covered"][k] for r in found if k in r["covered"]]
        if errs:
            s["metrics"][k] = {"n": len(errs), "median": statistics.median(errs), "p90": pct(errs, 0.9),
                               "unit": unit, "interval_coverage": sum(cov) / len(cov)}
    lbw = [r for r in found if "lbw_truth" in r]
    right = sum(1 for r in lbw if r["lbw_engine"] == r["lbw_truth"])
    ucall = sum(1 for r in lbw if r["lbw_engine"] == "UMPIRE'S CALL")
    na = sum(1 for r in lbw if r["lbw_engine"].startswith("n/a"))
    wrong = len(lbw) - right - ucall - na
    s["lbw"] = {"n": len(lbw), "correct": right, "umpires_call": ucall, "not_applicable": na, "wrong": wrong}
    s["wrong_lbw_rate"] = wrong / max(len(lbw), 1)
    ams = [r["analysis_ms"] for r in found if r.get("analysis_ms") is not None]
    s["analysis_ms_median"] = statistics.median(ams) if ams else float("nan")
    s["analysis_ms_p90"] = pct(ams, 0.9) if ams else float("nan")
    det = [r["timings"]["detect_per_frame"] for r in rows if "timings" in r]
    s["detect_ms_per_frame"] = statistics.median(det) if det else float("nan")
    return s


def report_md(summary: dict, rows: list[dict], preset: str) -> str:
    names = {
        "release_speed_kmh": "Release speed", "average_speed_kmh": "Average speed",
        "swing_cm": "Swing", "turn_deg": "Turn", "bounce_length_m": "Length (bounce point)",
        "bounce_line_cm": "Line at bounce", "height_at_stumps_cm": "Height at stumps",
        "line_at_stumps_cm": "Line at stumps", "impact_line_cm": "Line at pad impact",
    }
    out = [f"### Preset `{preset}`", "",
           f"- Deliveries found: **{summary['found']}/{summary['scenes']}** "
           f"({100 * summary['found_rate']:.0f} %), false deliveries: {summary['false_deliveries']}",
           f"- LBW decisions ({summary['lbw']['n']} pad impacts): correct {summary['lbw']['correct']}, "
           f"umpire's call {summary['lbw']['umpires_call']}, impact not recognised {summary['lbw']['not_applicable']}, "
           f"**wrong {summary['lbw']['wrong']}**",
           f"- Analysis time per delivery: median {summary['analysis_ms_median']:.0f} ms, "
           f"90th percentile {summary['analysis_ms_p90']:.0f} ms; detection {summary['detect_ms_per_frame']:.1f} ms/frame",
           "", "| Metric | Median error | 90 % of errors below | 90 % interval contains truth |",
           "|---|---|---|---|"]
    for k, m in summary["metrics"].items():
        u = m["unit"]
        out.append(f"| {names[k]} | {m['median']:.2f} {u} | {m['p90']:.2f} {u} | "
                   f"{100 * m['interval_coverage']:.0f} % (n={m['n']}) |")
    out.append("")
    misses = [r for r in rows if r["status"] != "found"]
    if misses:
        out.append("Missed scenes: " + ", ".join(f"{r['scene']} ({r['status']})" for r in misses))
        out.append("")
    wrong = [r for r in rows if r["status"] == "found" and "lbw_truth" in r
             and r["lbw_engine"] not in (r["lbw_truth"], "UMPIRE'S CALL") and not r["lbw_engine"].startswith("n/a")]
    if wrong:
        out.append("Wrong LBW calls: " + "; ".join(
            f"{r['scene']} engine {r['lbw_engine']} vs truth {r['lbw_truth']} {r['lbw_components']}" for r in wrong))
        out.append("")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--work", required=True, help="folder for scenes and results")
    ap.add_argument("--generate", type=int, default=0, help="generate this many new scenes first")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--preset", default="720p30")
    ap.add_argument("--cli", default=os.path.join(ROOT, "engine", "build", "pocketeye-cli"))
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--report", help="write a Markdown summary here")
    ap.add_argument("--json", help="write per-scene rows and the summary here")
    ap.add_argument("--gate", action="store_true", help="exit 1 if the CI limits are exceeded")
    a = ap.parse_args()

    if a.generate:
        import generate
        generate.generate(a.work, a.generate, a.seed, a.preset, a.workers)
    scenes = sorted(os.path.join(a.work, d) for d in os.listdir(a.work) if d.startswith("scene_"))
    with ThreadPoolExecutor(a.workers) as ex:
        results = list(ex.map(lambda sc: run_cli(a.cli, sc), scenes))
    rows = [evaluate_scene(sc, r) for sc, r in zip(scenes, results)]
    summary = summarise(rows)
    md = report_md(summary, rows, a.preset)
    print(md)
    if a.report:
        with open(a.report, "w") as f:
            f.write(md + "\n")
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"summary": summary, "rows": rows}, f, indent=1)
    if a.gate:
        g = GATES[a.preset]
        failures = []
        if summary["found_rate"] < g["found_rate"]:
            failures.append(f"found rate {summary['found_rate']:.2f} < {g['found_rate']}")
        if summary["wrong_lbw_rate"] > g["wrong_lbw_rate"]:
            failures.append(f"wrong LBW rate {summary['wrong_lbw_rate']:.2f} > {g['wrong_lbw_rate']}")
        for k, lim in g.items():
            if k in summary["metrics"]:
                m = summary["metrics"][k]
                if m["median"] > lim[0] or m["p90"] > lim[1]:
                    failures.append(f"{k}: median {m['median']:.2f}/p90 {m['p90']:.2f} > {lim}")
        if failures:
            print("GATE FAILED:\n  " + "\n  ".join(failures))
            sys.exit(1)
        print("gate passed")


if __name__ == "__main__":
    main()
