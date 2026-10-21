// Phone -> laptop streaming protocol (shared with camera_lite, the Android 5+
// camera app; see docs/protocol.md).
//
// The phone connects to the laptop over TCP and sends one JSON line:
//   {"hello":"pocketeye-cam","token":"123456","width":1280,"height":720,"fps":30,
//    "hfov":62.0,"device":"SM-G531F"}\n
// then binary messages, big-endian:
//   'PEF1' | int64 capture time (us, phone clock) | int32 length | JPEG bytes   (frame)
//   'PEP1' | int64 laptop time echoed (us) | int64 phone time (us)              (pong)
// The laptop sends JSON lines back:
//   {"type":"hello_ok"}
//   {"type":"ping","t":<laptop us>}
//   {"type":"result","lines":["Speed 92 km/h", ...],"decision":"OUT"}
import 'dart:typed_data';

const int defaultPort = 47800;
const List<int> frameMagic = [0x50, 0x45, 0x46, 0x31]; // PEF1
const List<int> pongMagic = [0x50, 0x45, 0x50, 0x31]; // PEP1

/// Incremental parser for the phone's byte stream.
class StreamParser {
  final _buf = BytesBuilder(copy: false);
  Uint8List _data = Uint8List(0);
  int _pos = 0;
  bool helloDone = false;

  void add(Uint8List chunk) {
    if (_pos > 0) {
      _data = Uint8List.sublistView(_data, _pos);
      _pos = 0;
    }
    _buf.add(_data);
    _buf.add(chunk);
    _data = _buf.takeBytes();
  }

  int get _available => _data.length - _pos;

  /// The hello line, once complete.
  String? takeHello() {
    if (helloDone) return null;
    for (var i = _pos; i < _data.length; ++i) {
      if (_data[i] == 0x0A) {
        final line = String.fromCharCodes(_data.sublist(_pos, i));
        _pos = i + 1;
        helloDone = true;
        return line;
      }
    }
    if (_available > 8192) throw const FormatException('no hello line');
    return null;
  }

  /// Next message: ('frame', time_us, jpeg) or ('pong', laptop_us, phone_us).
  (String, int, Object)? next() {
    if (_available < 4) return null;
    final bd = ByteData.sublistView(_data, _pos);
    final m = [_data[_pos], _data[_pos + 1], _data[_pos + 2], _data[_pos + 3]];
    if (_eq(m, frameMagic)) {
      if (_available < 16) return null;
      final t = bd.getInt64(4);
      final len = bd.getInt32(12);
      if (len <= 0 || len > 20 * 1024 * 1024) throw FormatException('bad frame length $len');
      if (_available < 16 + len) return null;
      final jpeg = Uint8List.fromList(Uint8List.sublistView(_data, _pos + 16, _pos + 16 + len));
      _pos += 16 + len;
      return ('frame', t, jpeg);
    }
    if (_eq(m, pongMagic)) {
      if (_available < 20) return null;
      final laptop = bd.getInt64(4);
      final phone = bd.getInt64(12);
      _pos += 20;
      return ('pong', laptop, phone);
    }
    throw const FormatException('stream out of sync');
  }

  static bool _eq(List<int> a, List<int> b) => a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/// Builds a frame message (used by tests and the desktop test sender).
Uint8List encodeFrame(int timeUs, Uint8List jpeg) {
  final out = Uint8List(16 + jpeg.length);
  out.setAll(0, frameMagic);
  final bd = ByteData.sublistView(out);
  bd.setInt64(4, timeUs);
  bd.setInt32(12, jpeg.length);
  out.setAll(16, jpeg);
  return out;
}
