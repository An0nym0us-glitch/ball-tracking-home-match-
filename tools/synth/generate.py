#!/usr/bin/env python3
"""Generate synthetic delivery videos with exact ground truth.

    python3 tools/synth/generate.py --out synth_out --count 20 --seed 1 --preset 720p30

Each scene folder gets video.mp4, timestamps.txt (capture time per frame),
config.json (stump taps for calibration, like the app would produce) and
gt.json (true speed, swing, turn, bounce, impact, LBW).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
from concurrent.futures import ProcessPoolExecutor

import cv2
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import physics  # noqa: E402
import render  # noqa: E402

PRESETS = {
    # Budget 2016-2017 phone streaming to a laptop.
    "720p30": dict(width=1280, height=720, fps=30.0, crf=(20, 27), noise=(1.5, 3.5)),
    # Stronger phone, phone-only mode.
    "1080p60": dict(width=1920, height=1080, fps=60.0, crf=(18, 24), noise=(1.0, 2.5)),
}

SPEEDS = {"tennis": (15, 30), "plastic": (12, 24), "rubber": (15, 30), "leather": (18, 36)}


def random_scene(rng: np.random.Generator, preset: str, overrides: dict | None = None,
                 scenario: str = "random") -> dict:
    p = PRESETS[preset]
    L = float(rng.uniform(12.0, 20.12))
    ball = str(rng.choice(list(physics.BALLS)))
    b = physics.BALLS[ball]
    full_toss = rng.random() < 0.12
    bounce_y = None if full_toss else float(L * rng.uniform(0.45, 0.9))
    pad = rng.random() < 0.75
    pad_y = None
    if pad:
        lo = L - 2.2 if full_toss else max(bounce_y + 0.6, L - 2.2)
        if lo < L - 0.4:
            pad_y = float(rng.uniform(lo, L - 0.4))
    scene = {
        "preset": preset,
        "width": p["width"], "height": p["height"], "fps": p["fps"],
        "pitch_length": L,
        "stump_height": float(rng.uniform(0.60, 0.72)),
        "stump_width": float(rng.uniform(0.18, 0.23)),
        "strip_width": float(rng.uniform(1.5, 3.0)),
        "surface": str(rng.choice(["concrete", "grass"])),
        "camera_eye": [float(rng.uniform(-0.15, 0.15)), float(rng.uniform(-2.5, -1.0)),
                       float(rng.uniform(1.0, 1.5))],
        "camera_target_frac": float(rng.uniform(0.55, 0.7)),
        "camera_target_z": float(rng.uniform(0.1, 0.4)),
        "camera_target_x": float(rng.uniform(-0.1, 0.1)),
        "roll_deg": float(rng.uniform(-1.5, 1.5)),
        "hfov_deg": float(rng.uniform(60, 72)),
        "hand": str(rng.choice(["right", "left"])),
        "exposure_s": float(1.0 / rng.uniform(60, 500)),
        "noise_sigma": float(rng.uniform(*p["noise"])),
        "crf": int(rng.integers(p["crf"][0], p["crf"][1] + 1)),
        "frame_jitter_s": 0.001,
        "drop_prob": 0.01,
        "tap_sigma_px": 1.0,
        "give_length": True,  # the app requires a measured length (the stumps-only estimate is a fallback)
        "delivery": {
            "ball": ball,
            "speed": float(rng.uniform(*SPEEDS[ball])),
            "release": [float(rng.uniform(-0.35, 0.35)), float(rng.uniform(0.6, 1.0)),
                        float(rng.uniform(1.6, 2.2))],
            "bounce_x": float(rng.uniform(-0.35, 0.35)),
            "bounce_y": bounce_y,
            "full_toss_z_at_stumps": float(rng.uniform(0.15, 0.7)),
            "lateral_accel": float(rng.uniform(-2.5, 2.5)),
            "turn_deg": float(rng.uniform(-6, 6)),
            "pad_y": pad_y,
            "restitution": float(b["restitution"] * rng.uniform(0.85, 1.15)),
            "friction": float(min(0.98, b["friction"] * rng.uniform(0.9, 1.1))),
            "drag_k": float(b["drag_k"] * rng.uniform(0.8, 1.2)),
            "t_release": 0.8,
        },
    }
    if scenario == "plumb":
        # Straight, full deliveries that hit the pad in line and go on to hit
        # the stumps: checks that clear LBWs are given OUT.
        dl = scene["delivery"]
        dl["release"][0] = float(rng.uniform(-0.1, 0.1))
        dl["release"][2] = float(rng.uniform(1.7, 1.9))
        dl["bounce_x"] = float(rng.uniform(-0.05, 0.05))
        dl["bounce_y"] = float(L * rng.uniform(0.78, 0.88))
        dl["lateral_accel"] = float(rng.uniform(-0.5, 0.5))
        dl["turn_deg"] = float(rng.uniform(-1.0, 1.0))
        dl["pad_y"] = float(L - rng.uniform(0.6, 1.1))
        dl["speed"] = float(rng.uniform(*SPEEDS[ball]) * 0.85)
    if scenario == "practice":
        # Bowling at empty stumps: no batter, no pad.
        scene["batter_present"] = False
        scene["delivery"]["pad_y"] = None
        scene["hand"] = "right"  # off/leg sides as the app reports them without a batter
    if overrides:
        for k, v in overrides.items():
            if k == "delivery":
                scene["delivery"].update(v)
            else:
                scene[k] = v
    return scene


def make_camera(s: dict) -> render.Camera:
    L = s["pitch_length"]
    target = (s["camera_target_x"], s["camera_target_frac"] * L, s["camera_target_z"])
    return render.Camera(s["width"], s["height"], s["hfov_deg"], s["camera_eye"], target, s["roll_deg"])


def stump_taps(cam: render.Camera, s: dict, rng: np.random.Generator) -> dict:
    L, H, W = s["pitch_length"], s["stump_height"], s["stump_width"]
    xs = 0.5 * (W - 0.035)
    taps = {}
    for end, y in (("bowler", 0.0), ("batter", L)):
        for side, x in (("left", -xs), ("right", xs)):
            for part, z in (("base", 0.0), ("top", H)):
                uv, depth = cam.project((x, y, z))
                if depth < 0.3:
                    continue
                if not (5 <= uv[0] < cam.width - 5 and 5 <= uv[1] < cam.height - 5):
                    continue  # out of frame: the user cannot tap it
                uv = uv + rng.normal(0, s["tap_sigma_px"], 2)
                taps[f"{end}_{side}_{part}"] = [round(float(uv[0]), 2), round(float(uv[1]), 2)]
    return taps


def render_scene(scene: dict, out_dir: str, seed: int) -> dict:
    rng = np.random.default_rng(seed)
    os.makedirs(out_dir, exist_ok=True)
    s = scene
    d = physics.Delivery(**{k: (tuple(v) if k == "release" else v) for k, v in s["delivery"].items()})
    L = s["pitch_length"]
    seen, gt = physics.truth(d, L, s["stump_height"], s["stump_width"], s["hand"])
    cam = make_camera(s)
    radius = physics.BALLS[d.ball]["radius"]
    color = physics.BALLS[d.ball]["color"]

    bg = render.ground_background(cam, L, s["strip_width"], rng, s["surface"])
    noise_bank = [rng.normal(0, s["noise_sigma"], bg.shape).astype(np.float32) for _ in range(6)]

    # Batter position: the pad meets the ball, or the batter stands aside.
    if seen.padded:
        bat_x, bat_y = seen.pad[1][0] + rng.uniform(-0.04, 0.04), d.pad_y
    else:
        y_b = L - 1.0
        t_b = physics.cross_y(seen, y_b)
        bx = physics.state_at(seen, t_b)[0][0] if t_b else 0.0
        bat_x, bat_y = bx + (0.5 if rng.random() < 0.5 else -0.5), y_b
    shirt = tuple(int(c) for c in rng.integers(30, 220, 3))
    head = (60, 90, 140)
    phase = float(rng.uniform(0, 6.28))

    t_end = min(seen.ts[-1] + 0.4, d.t_release + 2.6)
    times = []
    k = 0
    while True:
        t = k / s["fps"] + rng.normal(0, s["frame_jitter_s"])
        k += 1
        if t > t_end:
            break
        if rng.random() < s["drop_prob"]:
            continue
        times.append(max(t, 0.0))

    enc = render.Encoder(os.path.join(out_dir, "video.mp4"), s["width"], s["height"], s["fps"], s["crf"])
    n_sub = 1 if s["exposure_s"] * s["fps"] < 0.05 else 7
    t_visible = d.t_release + 0.03
    for i, t in enumerate(times):
        img = bg.copy()
        items = [(L, "stumps_far"), (bat_y, "batter"), (0.0, "stumps_near")]
        ball_pos = None
        if t >= t_visible and t <= seen.ts[-1]:
            subs = [t + (j / max(n_sub - 1, 1) - 0.5) * s["exposure_s"] for j in range(n_sub)] if n_sub > 1 else [t]
            ball_pos = [physics.state_at(seen, ts)[0] for ts in subs]
            items.append((ball_pos[len(ball_pos) // 2][1], "ball"))
        dt = t - d.t_release
        if -0.6 <= dt <= 0.5:
            items.append((d.release[1] - 0.35 + 0.8 * min(max(dt + 0.6, 0), 0.7), "bowler"))
        for _, kind in sorted(items, key=lambda it: -it[0]):
            if kind == "stumps_far":
                render.draw_stumps(img, cam, L, s["stump_height"], s["stump_width"])
            elif kind == "stumps_near" and s.get("bowler_stumps", True):
                render.draw_stumps(img, cam, 0.0, s["stump_height"], s["stump_width"])
            elif kind == "batter" and s.get("batter_present", True):
                render.draw_batter(img, cam, bat_x, bat_y, t, (shirt, head), phase)
            elif kind == "bowler":
                render.draw_bowler(img, cam, d.release, t, d.t_release)
            elif kind == "ball":
                render.draw_ball(img, cam, ball_pos, radius, color)
        img += noise_bank[i % len(noise_bank)]
        enc.write(np.clip(img, 0, 255).astype(np.uint8))
    enc.close()

    # The picture the app calibrates on: the empty pitch (before the batter
    # takes guard), as a camera JPEG.
    img = bg.copy()
    render.draw_stumps(img, cam, L, s["stump_height"], s["stump_width"])
    if s.get("bowler_stumps", True):
        render.draw_stumps(img, cam, 0.0, s["stump_height"], s["stump_width"])
    img += noise_bank[0]
    cv2.imwrite(os.path.join(out_dir, "calib.jpg"), np.clip(img, 0, 255).astype(np.uint8), [cv2.IMWRITE_JPEG_QUALITY, 90])

    with open(os.path.join(out_dir, "timestamps.txt"), "w") as f:
        f.write("\n".join(f"{t:.6f}" for t in times) + "\n")

    config = {
        "image": {"width": s["width"], "height": s["height"]},
        "pitch": {"stump_height_m": round(s["stump_height"], 3), "stump_width_m": round(s["stump_width"], 3)},
        "taps": stump_taps(cam, s, rng),
        "camera_hint": {"hfov_deg": round(s["hfov_deg"] + float(rng.uniform(-1.5, 1.5)), 1)},
        "ball": d.ball,
        "batter": s["hand"] if s.get("batter_present", True) else "none",
    }
    if s["give_length"]:
        config["pitch"]["length_m"] = round(L, 2)
    if not s.get("bowler_stumps", True):
        # Stumps only at the batter's end: no bowler's-end points; the app
        # needs the pitch length (from the bowling mark) and the camera position.
        config["bowler_stumps"] = False
        config["taps"] = {k: v for k, v in config["taps"].items() if not k.startswith("bowler")}
    if sum(1 for k in config["taps"] if k.startswith("bowler")) < 2:
        # Bowler's stumps out of frame: the app asks for two tape measurements.
        config["camera_height_m"] = round(float(cam.C[2] + rng.normal(0, 0.02)), 3)
        config["camera_distance_m"] = round(float(-cam.C[1] + rng.normal(0, 0.04)), 3)
        config["pitch"]["length_m"] = round(L, 2)
    with open(os.path.join(out_dir, "config.json"), "w") as f:
        json.dump(config, f, indent=2)
    gt["scene"] = s
    gt["camera_center"] = [float(c) for c in cam.C]
    gt["frames"] = len(times)
    with open(os.path.join(out_dir, "gt.json"), "w") as f:
        json.dump(gt, f, indent=2)
    return gt


def _job(args):
    scene, out_dir, seed = args
    render_scene(scene, out_dir, seed)
    return out_dir


def generate(out: str, count: int, seed: int, preset: str, workers: int, overrides: dict | None = None,
             scenario: str = "random"):
    rng = np.random.default_rng(seed)
    jobs = []
    for i in range(count):
        scene = random_scene(rng, preset, overrides, scenario)
        jobs.append((scene, os.path.join(out, f"scene_{i:04d}"), seed * 100003 + i))
    if workers <= 1:
        return [_job(j) for j in jobs]
    with ProcessPoolExecutor(workers) as ex:
        return list(ex.map(_job, jobs))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--count", type=int, default=10)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--preset", choices=sorted(PRESETS), default="720p30")
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--override", default=None, help="JSON dict merged into every scene")
    ap.add_argument("--scenario", choices=["random", "plumb", "practice"], default="random")
    a = ap.parse_args()
    overrides = json.loads(a.override) if a.override else None
    for d in generate(a.out, a.count, a.seed, a.preset, a.workers, overrides, a.scenario):
        print(d)


if __name__ == "__main__":
    main()
