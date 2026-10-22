#!/usr/bin/env python3
"""Checks automatic stump finding on synthetic scenes.

For every scene folder (tools/synth/generate.py output) the calibration
picture (calib.jpg: the empty pitch) is given to `pocketeye-cli autocal`, once with the measured pitch length and
once without (the length must then come from the two stump sets). Found taps
are compared with the exact stump positions, and the measured length with the
true one.

    python3 tools/eval/eval_autocal.py --scenes out/eval_random [--gate]
"""
import argparse
import glob
import json
import os
import subprocess
import sys
import tempfile

import cv2
import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "synth"))
import generate  # noqa: E402


def true_taps(gt):
    s = gt["scene"]
    cam = generate.make_camera(s)
    L, H, W = s["pitch_length"], s["stump_height"], s["stump_width"]
    xs = 0.5 * (W - 0.035)
    out = {}
    for end, y in (("bowler", 0.0), ("batter", L)):
        for side, x in (("left", -xs), ("right", xs)):
            for part, z in (("base", 0.0), ("top", H)):
                uv, depth = cam.project((x, y, z))
                if depth > 0.3:
                    out[f"{end}_{side}_{part}"] = uv
    return out


def stumps_error_cm(gt, calib):
    """Largest world error (cm) at the batter's stumps when picture points there
    are mapped back with the estimated camera: what matters for LBW."""
    if not calib or not calib.get("ok"):
        return None
    s = gt["scene"]
    cam = generate.make_camera(s)
    c = calib["camera"]
    R, t = np.array(c["R"]), np.array(c["t"])
    C = -R.T @ t
    L_true, L_est = s["pitch_length"], calib["pitch_length_m"]
    worst = 0.0
    for x in (-0.3, -0.15, 0.0, 0.15, 0.3):
        for z in (0.1, 0.4, 0.7):
            uv, _ = cam.project((x, L_true, z))
            d = R.T @ np.array([(uv[0] - c["cx"]) / c["f"], (uv[1] - c["cy"]) / c["f"], 1.0])
            k = (L_est - C[1]) / d[1]
            p = C + k * d
            worst = max(worst, abs(p[0] - x), abs(p[2] - z))
    return 100 * worst


def calibrate(cli, cfg, tmp):
    cpath = os.path.join(tmp, "cfg.json")
    with open(cpath, "w") as f:
        json.dump(cfg, f)
    p = subprocess.run([cli, "calibrate", "--config", cpath], capture_output=True, text=True)
    return json.loads(p.stdout) if p.stdout.strip() else None


def run(cli, frame_path, cfg, tmp):
    cpath = os.path.join(tmp, "cfg.json")
    with open(cpath, "w") as f:
        json.dump(cfg, f)
    p = subprocess.run([cli, "autocal", "--image", frame_path, "--config", cpath], capture_output=True, text=True)
    return json.loads(p.stdout) if p.stdout.strip() else {"ok": False, "message": p.stderr}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenes", required=True, help="folder with scene_* subfolders")
    ap.add_argument("--cli", default="engine/build/pocketeye-cli")
    ap.add_argument("--gate", action="store_true", help="exit 1 below the CI limits")
    ap.add_argument("-v", action="store_true")
    a = ap.parse_args()

    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for d in sorted(glob.glob(os.path.join(a.scenes, "scene_*"))):
            gt = json.load(open(os.path.join(d, "gt.json")))
            cfg = json.load(open(os.path.join(d, "config.json")))
            fpath = os.path.join(d, "calib.jpg")  # empty pitch, as the app calibrates
            if not os.path.exists(fpath):
                ok, frame = cv2.VideoCapture(os.path.join(d, "video.mp4")).read()
                if not ok:
                    continue
                fpath = os.path.join(tmp, "frame.png")
                cv2.imwrite(fpath, frame)
            truth = true_taps(gt)
            bowler_visible = sum(1 for k in cfg["taps"] if k.startswith("bowler")) >= 2
            base = {k: v for k, v in cfg.items() if k != "taps"}
            with_len = run(a.cli, fpath, base, tmp)
            no_len = dict(base)
            no_len["pitch"] = {k: v for k, v in base["pitch"].items() if k != "length_m"}
            no_len.pop("camera_height_m", None)
            no_len.pop("camera_distance_m", None)
            # The scenes' field-of-view hint is within +/-1.5 deg, like a phone's lens data.
            no_len["camera_hint"] = dict(no_len["camera_hint"], hfov_sigma_deg=1.0)
            est = run(a.cli, fpath, no_len, tmp)

            errs = []
            for name, uv in with_len.get("taps", {}).items():
                if name in truth:
                    errs.append(float(np.hypot(uv[0] - truth[name][0], uv[1] - truth[name][1])))
            L = gt["scene"]["pitch_length"]
            manual = calibrate(a.cli, cfg, tmp)
            row = {
                "err_cm": stumps_error_cm(gt, with_len.get("calibration")),
                "manual_err_cm": stumps_error_cm(gt, manual),
                "scene": os.path.basename(d),
                "bowler_visible": bowler_visible,
                "batter_found": with_len.get("batter_found", False),
                "bowler_found": with_len.get("bowler_found", False),
                "ok": with_len.get("ok", False),
                "max_tap_err_px": max(errs) if errs else None,
                "length_true": L,
                "length_est": est.get("pitch_length_m") if est.get("length_estimated") else None,
                "length_sigma": est.get("pitch_length_sigma_m"),
                "length_uncertain": est.get("length_uncertain", False),
                "message": est.get("message", ""),
            }
            rows.append(row)
            if a.v:
                print(json.dumps(row))

    n = len(rows)
    vis = [r for r in rows if r["bowler_visible"]]
    measured = [r for r in vis if r["length_est"] is not None]
    len_err = [abs(r["length_est"] - r["length_true"]) for r in measured]
    auto_cm = [r["err_cm"] for r in rows if r["err_cm"] is not None]
    man_cm = [r["manual_err_cm"] for r in rows if r["manual_err_cm"] is not None]
    print(f"scenes: {n}")
    print(f"calibrated automatically: {len(auto_cm)}/{n}")
    if auto_cm:
        print(f"  error at the batter's stumps: median {np.median(auto_cm):.1f} cm, 90% {np.percentile(auto_cm, 90):.1f} cm, "
              f"max {max(auto_cm):.1f} cm")
    if man_cm:
        print(f"  (hand taps, 1 px noise: median {np.median(man_cm):.1f} cm, 90% {np.percentile(man_cm, 90):.1f} cm, "
              f"max {max(man_cm):.1f} cm)")
    print(f"bowler's stumps in view: {len(vis)}; pitch length measured: {len(measured)}")
    if len_err:
        print(f"  length error: median {np.median(len_err):.2f} m, max {max(len_err):.2f} m")
    # Lengths the app would use without asking (it asks above +/-0.4 m) must be right.
    confident_wrong = [r for r in measured if not r["length_uncertain"] and abs(r["length_est"] - r["length_true"]) > 0.8]
    for r in confident_wrong:
        print(f"  confidently wrong length: {r['scene']} {r['length_est']:.2f} m (true {r['length_true']:.2f})")
    if a.gate:
        good = (len(auto_cm) >= 0.85 * n and np.percentile(auto_cm, 90) <= 5.0 and not confident_wrong
                and len(measured) >= 0.8 * len(vis))
        print("gate passed" if good else "gate FAILED")
        sys.exit(0 if good else 1)


if __name__ == "__main__":
    main()
