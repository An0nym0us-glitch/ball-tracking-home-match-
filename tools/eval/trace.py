#!/usr/bin/env python3
"""Debug view of one scene: truth timeline vs. the engine's labelled observations."""
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "synth"))
import generate  # noqa: E402
import physics  # noqa: E402

d = sys.argv[1]
cli = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "engine", "build", "pocketeye-cli")
env = dict(os.environ, PE_DEBUG="1")
p = subprocess.run([cli, "analyze", "--video", d + "/video.mp4", "--config", d + "/config.json",
                    "--timestamps", d + "/timestamps.txt", "--out", d + "/trace.json"] + sys.argv[2:],
                   capture_output=True, text=True, env=env)
print("\n".join(l for l in p.stderr.splitlines() if l.startswith("hyp"))[-1500:])
g = json.load(open(d + "/gt.json"))
r = json.load(open(d + "/trace.json"))
s = g["scene"]
cam = generate.make_camera(s)
dl = physics.Delivery(**{k: (tuple(v) if k == "release" else v) for k, v in s["delivery"].items()})
seen, _ = physics.truth(dl, s["pitch_length"], s["stump_height"], s["stump_width"], s["hand"])
print("truth: release 0.8, bounce", g.get("t_bounce"), "impact", g.get("t_impact"), "L", s["pitch_length"])
for x in r["deliveries"]:
    print("delivery valid", x["valid"], x.get("reject_reason"), "t_bounce", x.get("t_bounce"), "t_impact",
          x.get("t_impact"), x["lbw"].get("impact_kind"))
    for o in x["observations"]:
        pt, _ = physics.state_at(seen, o["t"])
        uv, _ = cam.project(pt)
        print("  t=%.3f uv=(%6.1f,%6.1f) r=%4.1f lab=%2d | true (%6.1f,%6.1f) y=%5.2f z=%4.2f" % (
            o["t"], o["uv"][0], o["uv"][1], o["r"], o["label"], uv[0], uv[1], pt[1], pt[2]))
