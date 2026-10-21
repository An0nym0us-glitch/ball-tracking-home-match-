# Phone → laptop streaming protocol

Used by `camera_lite` (the camera app for old phones), the laptop app's Live mode (`app/lib/engine/workers.dart`, `app/lib/net/protocol.dart`) and `tools/fake_phone.py`.

## Pairing

The laptop shows a QR code containing:

```json
{"app":"pocketeye","hosts":["192.168.1.20","10.0.0.5"],"port":47800,"token":"123456"}
```

The phone tries each host in turn. The same values are shown as text for typing in by hand.

## Connection

The phone opens a TCP connection to `host:47800` and sends one JSON line:

```json
{"hello":"pocketeye-cam","token":"123456","width":1280,"height":720,"fps":30,"hfov":62.0,"device":"SM-G531F"}
```

The laptop answers `{"type":"hello_ok"}`, or `{"type":"error","text":"wrong pairing code"}` and then closes.
Only one phone is served at a time.

## Phone → laptop (binary, big-endian)

| Message | Layout |
|---|---|
| Frame | `'PEF1'` · int64 capture time (µs, phone clock) · int32 length · JPEG bytes |
| Pong | `'PEP1'` · int64 laptop time from the ping (µs) · int64 phone time (µs) |

Capture times come from the camera callback (`SystemClock.elapsedRealtimeNanos`). The engine only uses differences between them, so the phone clock does not need to match the laptop clock.

## Laptop → phone (JSON lines)

| Message | Meaning |
|---|---|
| `{"type":"ping","t":<laptop µs>}` | Once a second. The pongs give the clock offset and the phone-to-laptop latency shown in the Live screen. |
| `{"type":"result","lines":["LBW: OUT", "Speed 106 km/h", ...],"decision":"OUT"}` | Result card for the phone to show. |

When the phone disconnects, the laptop finishes the delivery in progress and starts a fresh engine with the same calibration, ready for the next connection.
