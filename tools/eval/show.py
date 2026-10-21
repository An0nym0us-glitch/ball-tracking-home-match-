#!/usr/bin/env python3
"""Print an engine result next to the synthetic ground truth (debugging aid)."""
import json
import sys

d = sys.argv[1]
r = json.load(open(d + "/result.json"))
g = json.load(open(d + "/gt.json"))
c = r["calibration"]
print("calib rms %.2f px, L %.2f (true %.2f), cam %s (true %s)" % (
    c["rms_px"], c["pitch_length_m"], g["scene"]["pitch_length"],
    [round(x, 2) for x in c["camera_position_m"]], [round(x, 2) for x in g["camera_center"]]))
print("timings", {k: round(v, 1) for k, v in r["timings_ms"].items()})
reasons = {}
for x in r["deliveries"]:
    if not x["valid"]:
        reasons[x["reject_reason"]] = reasons.get(x["reject_reason"], 0) + 1
        continue
    print("DELIVERY t=%.2f-%.2f n_obs=%d" % (x["time_s"][0], x["time_s"][1], len(x["observations"])))
    for k, v in x["metrics"].items():
        if isinstance(v, dict):
            print("   %-22s %8.1f  [%6.1f, %6.1f]   truth %s" % (k, v["value"], v["lo"], v["hi"],
                  round(g[k.replace("bounce_length_m", "length_m")], 1) if k.replace("bounce_length_m", "length_m") in g else "-"))
    print("   lbw", {k: (v["status"] if isinstance(v, dict) and "status" in v else v) for k, v in x["lbw"].items()})
    print("   quality", x["quality"], "timings", x["timings_ms"])
print("rejected:", reasons)
print("TRUTH lbw", g.get("lbw"), "t_bounce", g.get("t_bounce"), "t_impact", g.get("t_impact"))
