"""Renders a delivery as seen by a phone behind the bowler's stumps.

Scene: textured ground with a pitch strip and creases, stumps at both ends, a
batter (pads, body, bat) who sways slightly, a bowler who hides the release,
the ball with motion blur over the exposure time, sensor noise and finally
H.264 compression through ffmpeg (like a phone stream).
"""
from __future__ import annotations

import math
import subprocess

import cv2
import numpy as np


class Camera:
    def __init__(self, width, height, hfov_deg, eye, target, roll_deg=0.0):
        self.width, self.height = width, height
        self.f = 0.5 * width / math.tan(math.radians(hfov_deg) / 2)
        self.cx, self.cy = width / 2, height / 2
        eye = np.asarray(eye, float)
        fwd = np.asarray(target, float) - eye
        fwd /= np.linalg.norm(fwd)
        right = np.cross(fwd, [0, 0, 1.0])
        right /= np.linalg.norm(right)
        down = np.cross(fwd, right)
        if roll_deg:
            a = math.radians(roll_deg)
            right, down = (math.cos(a) * right + math.sin(a) * down,
                           -math.sin(a) * right + math.cos(a) * down)
        self.R = np.stack([right, down, fwd])
        self.t = -self.R @ eye
        self.C = eye

    def to_cam(self, p):
        return self.R @ np.asarray(p, float) + self.t

    def project(self, p):
        c = self.to_cam(p)
        return np.array([self.f * c[0] / c[2] + self.cx, self.f * c[1] / c[2] + self.cy]), c[2]

    def ball_radius_px(self, p, r):
        c = self.to_cam(p)
        d = np.linalg.norm(c)
        return self.f * r / math.sqrt(max(d * d - r * r, 1e-9))


def ground_background(cam: Camera, L: float, strip_w: float, rng, surface: str):
    """Image of the ground plane (z = 0) and the background above the horizon."""
    h, w = cam.height, cam.width
    img = np.zeros((h, w, 3), np.float32)
    # Background: wall / sky gradient.
    top = np.array(rng.uniform(150, 230, 3), np.float32)
    bottom = np.array(rng.uniform(80, 160, 3), np.float32)
    ramp = np.linspace(0, 1, h, dtype=np.float32)[:, None, None]
    img[:] = top * (1 - ramp) + bottom * ramp

    # Rays for every pixel; intersect with the ground.
    us, vs = np.meshgrid(np.arange(w, dtype=np.float64), np.arange(h, dtype=np.float64))
    d_cam = np.stack([(us - cam.cx) / cam.f, (vs - cam.cy) / cam.f, np.ones_like(us)], -1)
    d_world = d_cam @ cam.R  # R^T * d for row vectors
    dz = d_world[..., 2]
    hit = dz < -1e-4
    s = np.where(hit, -cam.C[2] / np.where(hit, dz, -1), 0)
    gx = cam.C[0] + s * d_world[..., 0]
    gy = cam.C[1] + s * d_world[..., 1]
    hit &= (gy < L + 25) & (np.abs(gx) < 25)

    # Procedural ground texture in world coordinates.
    tex_res = 100.0  # px per metre
    x0, y0, tw, th = -6.0, -4.0, 12.0, L + 12.0
    tex_h, tex_w = int(th * tex_res), int(tw * tex_res)
    if surface == "grass":
        base = np.array([60, 140, 70], np.float32)
        strip = np.array([120, 170, 190], np.float32)
    else:  # concrete terrace / cement pitch
        base = np.array(rng.uniform(110, 160, 3), np.float32)
        strip = base * rng.uniform(0.75, 0.9)
    small = rng.normal(0, 1, (tex_h // 8 + 1, tex_w // 8 + 1)).astype(np.float32)
    noise = cv2.resize(small, (tex_w, tex_h), interpolation=cv2.INTER_LINEAR)[..., None]
    tex = base + 10 * noise + rng.normal(0, 4, (tex_h, tex_w, 1)).astype(np.float32)
    sx0 = int((-strip_w / 2 - x0) * tex_res)
    sx1 = int((strip_w / 2 - x0) * tex_res)
    sy0 = int((-1.2 - y0) * tex_res)
    sy1 = int((L + 1.2 - y0) * tex_res)
    tex[sy0:sy1, sx0:sx1] = strip + 8 * noise[sy0:sy1, sx0:sx1] + \
        rng.normal(0, 3, (sy1 - sy0, sx1 - sx0, 1)).astype(np.float32)
    # Creases (white) at both stumps lines and popping creases 1.22 m in front.
    for yc in (0.0, L, 1.22, L - 1.22):
        r0 = int((yc - 0.025 - y0) * tex_res)
        tex[max(r0, 0):r0 + 5, sx0:sx1] = 225
    tex = np.clip(tex, 0, 255).astype(np.float32)

    map_x = ((gx - x0) * tex_res).astype(np.float32)
    map_y = ((gy - y0) * tex_res).astype(np.float32)
    ground = cv2.remap(tex, map_x, map_y, cv2.INTER_LINEAR, borderMode=cv2.BORDER_REPLICATE)
    # Far ground fades into haze.
    haze = np.clip((s - 15) / 40, 0, 0.6)[..., None].astype(np.float32)
    ground = ground * (1 - haze) + bottom * haze
    img[hit] = ground[hit]
    return img


def draw_box(img, cam, x0, x1, y, z0, z1, color):
    """Axis-aligned rectangle in the plane y = const (facing the camera)."""
    pts = []
    for x, z in ((x0, z0), (x1, z0), (x1, z1), (x0, z1)):
        uv, depth = cam.project((x, y, z))
        if depth < 0.2:
            return
        pts.append(uv)
    cv2.fillConvexPoly(img, np.round(np.array(pts) * 16).astype(np.int32), color,
                       lineType=cv2.LINE_AA, shift=4)


def draw_stumps(img, cam, y, stump_h, stump_w, diam=0.035):
    xs = 0.5 * (stump_w - diam)
    for x in (-xs, 0.0, xs):
        draw_box(img, cam, x - diam / 2, x + diam / 2, y, 0.0, stump_h, (200, 225, 235))
    draw_box(img, cam, -xs - diam / 2, xs + diam / 2, y, stump_h - 0.012, stump_h + 0.008, (170, 200, 215))


def draw_batter(img, cam, x, y, t, colors, rng_phase):
    sway = 0.03 * math.sin(2 * math.pi * 0.8 * t + rng_phase)
    x = x + sway
    pad = (235, 238, 240)
    # Back leg (behind), front leg, body, head, bat.
    draw_box(img, cam, x + 0.05, x + 0.21, y + 0.25, 0.0, 0.9, pad)
    draw_box(img, cam, x - 0.09, x + 0.09, y, 0.0, 0.95, pad)
    draw_box(img, cam, x - 0.22, x + 0.22, y + 0.15, 0.9, 1.55, colors[0])
    draw_box(img, cam, x - 0.11, x + 0.11, y + 0.15, 1.55, 1.8, colors[1])
    bat_x = x - 0.25 + 0.05 * math.sin(2 * math.pi * 1.3 * t + rng_phase)
    draw_box(img, cam, bat_x - 0.05, bat_x + 0.05, y - 0.05, 0.25, 0.85, (90, 160, 200))


def draw_bowler(img, cam, release, t, t_release):
    dt = t - t_release
    if dt < -0.6 or dt > 0.5:
        return
    # Runs up to the crease, releases, then follows through to the side.
    x = release[0] + 0.25 + (0.0 if dt < 0.1 else 3.0 * (dt - 0.1))
    y = release[1] - 0.35 + 0.8 * min(max(dt + 0.6, 0), 0.7)
    draw_box(img, cam, x - 0.25, x + 0.25, y, 0.0, 1.55, (40, 40, 60))
    draw_box(img, cam, x - 0.11, x + 0.11, y, 1.55, 1.8, (60, 80, 110))
    # Bowling arm: rotates over the top and reaches the release point at t_release.
    ang = math.pi * (0.5 + 2.5 * dt)
    sh = np.array([x - 0.15, y, 1.5])
    hand = sh + 0.7 * np.array([0.0, math.cos(ang) * 0.5, math.sin(ang)])
    a, da = cam.project(sh)
    b, db = cam.project(hand)
    if da > 0.2 and db > 0.2:
        width = max(2, int(cam.f * 0.09 / da))
        cv2.line(img, tuple(np.round(a).astype(int)), tuple(np.round(b).astype(int)), (50, 50, 80),
                 width, cv2.LINE_AA)


def draw_ball(img, cam, positions, radius_m, color):
    """Motion-blurred ball: average of sub-frame discs over the exposure."""
    uvs, rads = [], []
    for p in positions:
        uv, depth = cam.project(p)
        if depth < 0.3:
            return
        uvs.append(uv)
        rads.append(cam.ball_radius_px(p, radius_m))
    uvs = np.array(uvs)
    rmax = max(rads)
    x0 = int(math.floor(uvs[:, 0].min() - rmax - 2))
    x1 = int(math.ceil(uvs[:, 0].max() + rmax + 2))
    y0 = int(math.floor(uvs[:, 1].min() - rmax - 2))
    y1 = int(math.ceil(uvs[:, 1].max() + rmax + 2))
    h, w = img.shape[:2]
    if x1 < 0 or y1 < 0 or x0 >= w or y0 >= h:
        return
    x0c, y0c, x1c, y1c = max(x0, 0), max(y0, 0), min(x1, w), min(y1, h)
    yy, xx = np.mgrid[y0c:y1c, x0c:x1c].astype(np.float32)
    alpha = np.zeros_like(xx)
    shade = np.zeros_like(xx)
    for uv, r in zip(uvs, rads):
        d = np.sqrt((xx - uv[0]) ** 2 + (yy - uv[1]) ** 2)
        cov = np.clip(r + 0.5 - d, 0, 1)
        alpha += cov
        # Simple lighting: brighter top-left.
        shade += cov * np.clip(1.15 - 0.25 * ((xx - uv[0]) + (yy - uv[1])) / max(r, 0.5) * 0.5, 0.7, 1.2)
    n = len(uvs)
    alpha /= n
    shade = np.where(alpha > 0, shade / np.maximum(alpha * n, 1e-6), 1.0)
    col = np.array(color, np.float32)[None, None, :] * shade[..., None]
    roi = img[y0c:y1c, x0c:x1c]
    roi[:] = roi * (1 - alpha[..., None]) + np.clip(col, 0, 255) * alpha[..., None]


class Encoder:
    def __init__(self, path, width, height, fps, crf):
        self.proc = subprocess.Popen(
            ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "bgr24",
             "-s", f"{width}x{height}", "-r", f"{fps}", "-i", "-", "-c:v", "libx264",
             "-preset", "veryfast", "-crf", str(crf), "-g", str(int(fps)), "-bf", "0",
             "-pix_fmt", "yuv420p", path],
            stdin=subprocess.PIPE)

    def write(self, frame_bgr_uint8):
        self.proc.stdin.write(frame_bgr_uint8.tobytes())

    def close(self):
        self.proc.stdin.close()
        if self.proc.wait() != 0:
            raise RuntimeError("ffmpeg failed")
