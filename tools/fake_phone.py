#!/usr/bin/env python3
"""Pretend to be the phone camera app: stream a video file to the laptop app.

Useful to test the laptop's Live mode without a phone:

    python3 tools/fake_phone.py --video clip.mp4 --host 127.0.0.1 --code 123456
    (the code is shown under the QR code in the laptop app)

Frames are sent in real time with their capture times (from --timestamps or
the video's frame rate), using the same protocol as camera_lite.
"""
import argparse
import json
import socket
import struct
import threading
import time

import cv2


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--video", required=True)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=47800)
    ap.add_argument("--code", required=True, help="pairing code shown by the laptop")
    ap.add_argument("--timestamps", help="capture time per frame (s), one per line")
    ap.add_argument("--quality", type=int, default=85)
    ap.add_argument("--loops", type=int, default=1)
    ap.add_argument("--tail", type=float, default=2.0, help="seconds of still frames to send at the end")
    a = ap.parse_args()

    cap = cv2.VideoCapture(a.video)
    fps = cap.get(cv2.CAP_PROP_FPS) or 30.0
    w, h = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH)), int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    ts = [float(x) for x in open(a.timestamps)] if a.timestamps else None

    s = socket.create_connection((a.host, a.port))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    hello = {"hello": "pocketeye-cam", "token": a.code, "width": w, "height": h, "fps": round(fps),
             "hfov": 65.0, "device": "fake_phone.py"}
    s.sendall((json.dumps(hello) + "\n").encode())
    lock = threading.Lock()
    t_start = time.monotonic()

    def receiver():
        buf = b""
        while True:
            data = s.recv(4096)
            if not data:
                return
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                msg = json.loads(line)
                if msg.get("type") == "ping":
                    phone_us = int((time.monotonic() - t_start) * 1e6)
                    with lock:
                        s.sendall(b"PEP1" + struct.pack(">qq", msg["t"], phone_us))
                elif msg.get("type") == "result":
                    print("RESULT CARD:", " | ".join(msg["lines"]))
                else:
                    print("laptop:", msg)

    threading.Thread(target=receiver, daemon=True).start()
    clock = 0.0
    last = None
    for loop in range(a.loops):
        cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
        i = 0
        base = clock
        while True:
            ok, frame = cap.read()
            if not ok:
                break
            t = base + (ts[i] if ts and i < len(ts) else i / fps)
            clock = t + 1.0 / fps
            ok, jpeg = cv2.imencode(".jpg", frame, [cv2.IMWRITE_JPEG_QUALITY, a.quality])
            last = jpeg.tobytes()
            # Pace to real time.
            delay = t_start + t - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            with lock:
                s.sendall(b"PEF1" + struct.pack(">qi", int(t * 1e6), len(last)) + last)
            i += 1
    for k in range(int(a.tail * fps)):
        t = clock + k / fps
        time.sleep(max(0.0, t_start + t - time.monotonic()))
        with lock:
            s.sendall(b"PEF1" + struct.pack(">qi", int(t * 1e6), len(last)) + last)
    time.sleep(1.0)
    s.close()


if __name__ == "__main__":
    main()
