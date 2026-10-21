"""Ground-truth ball physics for synthetic deliveries.

Independent of the C++ engine on purpose: same physical model (gravity,
quadratic drag, constant sideways swing acceleration, bounce with restitution,
friction and turn), but separate code, so the engine is checked against it.

World frame: origin at the base of the bowler-end middle stump, +y towards the
batter, +x to the camera's right, +z up (metres).
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

G = 9.81

BALLS = {
    # radius m, restitution, friction (along-pitch speed kept), drag k (1/m), BGR colour
    "tennis": dict(radius=0.0335, restitution=0.70, friction=0.80, drag_k=0.020, color=(60, 225, 215)),
    "plastic": dict(radius=0.0350, restitution=0.55, friction=0.80, drag_k=0.030, color=(235, 240, 240)),
    "rubber": dict(radius=0.0350, restitution=0.70, friction=0.82, drag_k=0.012, color=(45, 45, 205)),
    "leather": dict(radius=0.0360, restitution=0.50, friction=0.88, drag_k=0.0065, color=(40, 35, 160)),
}


@dataclass
class Delivery:
    ball: str = "tennis"
    speed: float = 25.0                    # release speed, m/s
    release: tuple = (0.15, 0.8, 1.9)      # release point
    bounce_x: float = 0.0
    bounce_y: float | None = 12.0          # None = full toss
    full_toss_z_at_stumps: float = 0.4
    lateral_accel: float = 0.0             # swing, m/s^2 along world x
    turn_deg: float = 0.0                  # heading change at the bounce, + towards +x
    pad_y: float | None = None             # pad impact at this y (None = no pad)
    restitution: float | None = None       # actual values (default: ball type)
    friction: float | None = None
    drag_k: float | None = None
    t_release: float = 0.8


@dataclass
class Flight:
    ts: list = field(default_factory=list)
    ps: list = field(default_factory=list)
    vs: list = field(default_factory=list)
    bounced: bool = False
    bounce: tuple | None = None            # (t, p, v_in)
    padded: bool = False
    pad: tuple | None = None               # (t, p, v)


def _acc(v, k, lat):
    sp = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    return (lat - k * sp * v[0], -k * sp * v[1], -G - k * sp * v[2])


def _rk4(p, v, h, k, lat):
    a1 = _acc(v, k, lat)
    v2 = tuple(v[i] + 0.5 * h * a1[i] for i in range(3))
    a2 = _acc(v2, k, lat)
    v3 = tuple(v[i] + 0.5 * h * a2[i] for i in range(3))
    a3 = _acc(v3, k, lat)
    v4 = tuple(v[i] + h * a3[i] for i in range(3))
    a4 = _acc(v4, k, lat)
    p = tuple(p[i] + h / 6 * (v[i] + 2 * v2[i] + 2 * v3[i] + v4[i]) for i in range(3))
    v = tuple(v[i] + h / 6 * (a1[i] + 2 * a2[i] + 2 * a3[i] + a4[i]) for i in range(3))
    return p, v


def params(d: Delivery):
    b = BALLS[d.ball]
    return (b["radius"],
            d.restitution if d.restitution is not None else b["restitution"],
            d.friction if d.friction is not None else b["friction"],
            d.drag_k if d.drag_k is not None else b["drag_k"])


def run(d: Delivery, v0, y_stop: float, use_pad: bool, dt: float = 2e-4,
        stop_at_bounce: bool = False, t_after_pad: float = 0.6) -> Flight:
    r, e, mu, k = params(d)
    f = Flight()
    t, p, v = d.t_release, tuple(d.release), tuple(v0)
    lat = d.lateral_accel
    f.ts.append(t); f.ps.append(p); f.vs.append(v)
    t_pad_end = 1e9
    full_toss = d.bounce_y is None
    while t < d.t_release + 3.0:
        pp, pv = p, v
        p, v = _rk4(p, v, dt, k, lat)
        t += dt
        if not full_toss and not f.bounced and p[2] <= r:
            a = (pp[2] - r) / (pp[2] - p[2])
            tb = t - dt + a * dt
            pb = tuple(pp[i] + a * (p[i] - pp[i]) for i in range(3))
            vb = tuple(pv[i] + a * (v[i] - pv[i]) for i in range(3))
            pb = (pb[0], pb[1], r)
            f.bounced = True
            f.bounce = (tb, pb, vb)
            if stop_at_bounce:
                return f
            th = math.radians(d.turn_deg)
            c, s = math.cos(th), math.sin(th)
            vx, vy = vb[0], vb[1]
            v = (mu * (vx * c + vy * s), mu * (-vx * s + vy * c), -e * vb[2])
            p, t = pb, tb
            lat = 0.0
        if use_pad and not f.padded and d.pad_y is not None and p[1] >= d.pad_y:
            f.padded = True
            f.pad = (t, p, v)
            v = (0.3 * v[0] + 0.4, -0.25 * v[1], 0.4 * abs(v[2]) + 0.5)
            lat = 0.0
            t_pad_end = t + t_after_pad
        if p[2] < r and (f.bounced or f.padded) and v[2] < 0:
            v = (0.8 * v[0], 0.8 * v[1], -0.5 * v[2])
        f.ts.append(t); f.ps.append(p); f.vs.append(v)
        if p[1] > y_stop or t > t_pad_end:
            break
    return f


def state_at(f: Flight, t: float):
    ts = f.ts
    if t <= ts[0]:
        return f.ps[0], f.vs[0]
    lo, hi = 0, len(ts) - 1
    if t >= ts[hi]:
        return f.ps[hi], f.vs[hi]
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if ts[mid] <= t:
            lo = mid
        else:
            hi = mid
    a = (t - ts[lo]) / (ts[hi] - ts[lo])
    p = tuple(f.ps[lo][i] + a * (f.ps[hi][i] - f.ps[lo][i]) for i in range(3))
    v = tuple(f.vs[lo][i] + a * (f.vs[hi][i] - f.vs[lo][i]) for i in range(3))
    return p, v


def cross_y(f: Flight, y: float):
    for i in range(1, len(f.ts)):
        if f.ps[i - 1][1] < y <= f.ps[i][1]:
            a = (y - f.ps[i - 1][1]) / (f.ps[i][1] - f.ps[i - 1][1])
            return f.ts[i - 1] + a * (f.ts[i] - f.ts[i - 1])
    return None


def aim(d: Delivery, L: float):
    """Elevation/azimuth so that the ball pitches at (bounce_x, bounce_y) or, for a
    full toss, reaches the batter's stumps at the requested height."""
    full_toss = d.bounce_y is None
    target_y = L if full_toss else d.bounce_y

    def vel(el, az):
        return (d.speed * math.cos(el) * math.sin(az), d.speed * math.cos(el) * math.cos(az),
                d.speed * math.sin(el))

    def miss(el, az):
        if full_toss:
            f = run(d, vel(el, az), L + 0.5, False, dt=1e-3)
            t = cross_y(f, L)
            if t is None:
                return -1.0, 0.0
            p, _ = state_at(f, t)
            return p[2] - d.full_toss_z_at_stumps, p[0] - d.bounce_x
        f = run(d, vel(el, az), L + 2.0, False, dt=1e-3, stop_at_bounce=True)
        if not f.bounced:
            return 1.0, 0.0
        pb = f.bounce[1]
        return pb[1] - d.bounce_y, pb[0] - d.bounce_x

    az = math.atan2(d.bounce_x - d.release[0], target_y - d.release[1])
    el = 0.0
    for _ in range(4):
        lo, hi = -0.8, 0.6
        for _ in range(30):
            mid = 0.5 * (lo + hi)
            if miss(mid, az)[0] > 0:
                hi = mid
            else:
                lo = mid
        el = 0.5 * (lo + hi)
        _, ex = miss(el, az)
        az -= ex / (target_y - d.release[1])
    return vel(el, az)


def truth(d: Delivery, L: float, stump_height: float, stump_width: float, hand: str,
          release_offset: float = 0.8):
    """Simulates the delivery and returns (seen flight, ground-truth dict)."""
    v0 = aim(d, L)
    free = run(d, v0, L + 2.0, use_pad=False)
    seen = run(d, v0, L + 1.5, use_pad=True)
    r = params(d)[0]
    off = -1.0 if hand == "right" else 1.0   # world x -> off side
    edge = 0.5 * stump_width + r

    gt = {"has_bounce": free.bounced, "t_release": d.t_release}
    t_rel = cross_y(free, release_offset)
    if t_rel is None or t_rel < d.t_release:
        t_rel = d.t_release
    p_rel, v_rel = state_at(free, t_rel)
    gt["release_speed_kmh"] = 3.6 * math.sqrt(sum(c * c for c in v_rel))
    t_st = cross_y(free, L)
    p_st = state_at(free, t_st)[0] if t_st is not None else None
    t_end = t_st
    if seen.padded:
        t_end = min(t_end, seen.pad[0]) if t_end is not None else seen.pad[0]
    # Average speed: path length from the release line to the stumps or the pad.
    length, prev, n = 0.0, p_rel, 200
    for i in range(1, n + 1):
        p, _ = state_at(free, t_rel + (t_end - t_rel) * i / n)
        length += math.dist(p, prev)
        prev = p
    gt["average_speed_kmh"] = 3.6 * length / (t_end - t_rel)

    if free.bounced:
        tb, pb, vin = free.bounce
        _, vout = state_at(free, tb + 1e-3)
        gt["t_bounce"] = tb
        gt["bounce"] = list(pb)
        gt["turn_deg"] = off * math.degrees(math.atan2(vout[0], vout[1]) - math.atan2(vin[0], vin[1]))
        ref = pb
        gt["length_m"] = L - pb[1]
        gt["bounce_line_cm"] = off * pb[0] * 100
    else:
        ref = seen.pad[1] if seen.padded else p_st
    x_line = p_rel[0] + v_rel[0] / v_rel[1] * (ref[1] - p_rel[1])
    gt["swing_cm"] = off * (ref[0] - x_line) * 100
    if p_st is not None:
        gt["at_stumps"] = list(p_st)
        gt["line_at_stumps_cm"] = off * p_st[0] * 100
        gt["height_at_stumps_cm"] = p_st[2] * 100
        gt["hitting"] = abs(p_st[0]) <= edge and p_st[2] <= stump_height + r
    if seen.padded:
        ti, pi, _ = seen.pad
        gt["t_impact"] = ti
        gt["impact"] = list(pi)
        gt["impact_line_cm"] = off * pi[0] * 100
        # LBW truth (no shot offered = False).
        pitching_ok = (not free.bounced) or off * free.bounce[1][0] >= -edge
        impact_in_line = abs(pi[0]) <= edge
        gt["lbw"] = {
            "pitching": "full toss" if not free.bounced else (
                "outside leg" if off * free.bounce[1][0] < -edge else
                ("outside off" if off * free.bounce[1][0] > edge else "in line")),
            "impact": "in line" if impact_in_line else (
                "outside off" if off * pi[0] > edge else "outside leg"),
            "wickets": "hitting" if gt.get("hitting") else "missing",
            "decision": "OUT" if (pitching_ok and impact_in_line and gt.get("hitting")) else "NOT OUT",
            # Distance of each criterion from its boundary (m): small = close call.
            "margins_m": {
                "pitching": (off * free.bounce[1][0] + edge) if free.bounced else 1.0,
                "impact": edge - abs(pi[0]),
                "wickets": (min(edge - abs(p_st[0]), stump_height + r - p_st[2])
                            if p_st is not None else -1.0),
            },
        }
    return seen, gt
