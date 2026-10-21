#!/usr/bin/env python3
"""Detection recall: fraction of frames where a candidate lies near the true ball.

    pocketeye-cli analyze ... --dump-candidates cands.json
    python3 tools/eval/recall.py SCENE_DIR
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "synth"))
import generate  # noqa: E402
import physics  # noqa: E402

d = sys.argv[1]
C = json.load(open(os.path.join(d, "cands.json")))
g = json.load(open(os.path.join(d, "gt.json")))
s = g["scene"]
cam = generate.make_camera(s)
dl = physics.Delivery(**{k: (tuple(v) if k == "release" else v) for k, v in s["delivery"].items()})
seen, _ = physics.truth(dl, s["pitch_length"], s["stump_height"], s["stump_width"], s["hand"])
t_imp = g.get("t_impact", 1e9)
hit = tot = 0
line = ""
for fr in C:
    t = fr["t"]
    if t < dl.t_release + 0.03 or t > min(seen.ts[-1], t_imp):
        continue
    p, _ = physics.state_at(seen, t)
    uv, depth = cam.project(p)
    if not (0 <= uv[0] < cam.width and 0 <= uv[1] < cam.height):
        continue
    tot += 1
    ok = any(math.dist(c[:2], uv) < 3.0 + 0.3 * cam.ball_radius_px(p, 0.0335) for c in fr["c"])
    hit += ok
    line += "x" if ok else "."
print(f"{os.path.basename(d)}: pre-impact recall {hit}/{tot}  {line}  cands/frame "
      f"{sum(len(f['c']) for f in C) / max(len(C), 1):.1f}")
