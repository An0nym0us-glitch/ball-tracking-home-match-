// dart:ffi bindings to the pocketeye C API (engine/include/pocketeye/pocketeye_c.h).
// Usable from any isolate: each isolate opens the library itself.
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

typedef _VersionC = Pointer<Utf8> Function();
typedef _TextOutC = Int32 Function(Pointer<Utf8>, Pointer<Utf8>, Int32);
typedef _TextOutD = int Function(Pointer<Utf8>, Pointer<Utf8>, int);
typedef _CreateC = Pointer<Void> Function(Pointer<Utf8>, Pointer<Utf8>, Int32);
typedef _CreateD = Pointer<Void> Function(Pointer<Utf8>, Pointer<Utf8>, int);
typedef _EngineVoidC = Void Function(Pointer<Void>);
typedef _EngineVoidD = void Function(Pointer<Void>);
typedef _EngineTextC = Int32 Function(Pointer<Void>, Pointer<Utf8>, Int32);
typedef _EngineTextD = int Function(Pointer<Void>, Pointer<Utf8>, int);
typedef _PushPlaneC = Int32 Function(Pointer<Void>, Pointer<Uint8>, Int32, Int32, Int32, Double);
typedef _PushPlaneD = int Function(Pointer<Void>, Pointer<Uint8>, int, int, int, double);
typedef _PushJpegC = Int32 Function(Pointer<Void>, Pointer<Uint8>, Int32, Double);
typedef _PushJpegD = int Function(Pointer<Void>, Pointer<Uint8>, int, double);
typedef _PushYuvC = Int32 Function(Pointer<Void>, Pointer<Uint8>, Pointer<Uint8>, Pointer<Uint8>, Int32, Int32,
    Int32, Int32, Int32, Double);
typedef _PushYuvD = int Function(
    Pointer<Void>, Pointer<Uint8>, Pointer<Uint8>, Pointer<Uint8>, int, int, int, int, int, double);
typedef _LastJpegC = Int32 Function(Pointer<Void>, Int32, Int32, Pointer<Uint8>, Int32);
typedef _LastJpegD = int Function(Pointer<Void>, int, int, Pointer<Uint8>, int);
typedef _YuvJpegC = Int32 Function(Pointer<Uint8>, Pointer<Uint8>, Pointer<Uint8>, Int32, Int32, Int32, Int32,
    Int32, Int32, Pointer<Uint8>, Int32);
typedef _YuvJpegD = int Function(
    Pointer<Uint8>, Pointer<Uint8>, Pointer<Uint8>, int, int, int, int, int, int, Pointer<Uint8>, int);
typedef _AutoCalC = Int32 Function(Pointer<Utf8>, Pointer<Uint8>, Int32, Pointer<Utf8>, Int32);
typedef _AutoCalD = int Function(Pointer<Utf8>, Pointer<Uint8>, int, Pointer<Utf8>, int);
typedef _BenchC = Int32 Function(Int32, Int32, Int32, Pointer<Utf8>, Int32);
typedef _BenchD = int Function(int, int, int, Pointer<Utf8>, int);
typedef _AnalyzeC = Int32 Function(
    Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>, Pointer<Int32>, Pointer<Utf8>, Int32);
typedef _AnalyzeD = int Function(
    Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>, Pointer<Utf8>, Pointer<Int32>, Pointer<Utf8>, int);
typedef _VideoOpenC = Pointer<Void> Function(Pointer<Utf8>);
typedef _VideoSeekC = Int32 Function(Pointer<Void>, Double);
typedef _VideoSeekD = int Function(Pointer<Void>, double);
typedef _VideoReadC = Int32 Function(Pointer<Void>, Int32, Int32, Pointer<Uint8>, Int32, Pointer<Double>);
typedef _VideoReadD = int Function(Pointer<Void>, int, int, Pointer<Uint8>, int, Pointer<Double>);

class PocketEyeNative {
  PocketEyeNative._(DynamicLibrary lib)
      : _version = lib.lookupFunction<_VersionC, _VersionC>('pe_version'),
        _calibrate = lib.lookupFunction<_TextOutC, _TextOutD>('pe_calibrate'),
        _create = lib.lookupFunction<_CreateC, _CreateD>('pe_create'),
        _destroy = lib.lookupFunction<_EngineVoidC, _EngineVoidD>('pe_destroy'),
        _finish = lib.lookupFunction<_EngineVoidC, _EngineVoidD>('pe_finish'),
        _pollResult = lib.lookupFunction<_EngineTextC, _EngineTextD>('pe_poll_result'),
        _stats = lib.lookupFunction<_EngineTextC, _EngineTextD>('pe_stats_json'),
        _pushLuma = lib.lookupFunction<_PushPlaneC, _PushPlaneD>('pe_push_luma'),
        _pushJpeg = lib.lookupFunction<_PushJpegC, _PushJpegD>('pe_push_jpeg'),
        _pushYuv = lib.lookupFunction<_PushYuvC, _PushYuvD>('pe_push_yuv420'),
        _lastJpeg = lib.lookupFunction<_LastJpegC, _LastJpegD>('pe_last_frame_jpeg'),
        _yuvJpeg = lib.lookupFunction<_YuvJpegC, _YuvJpegD>('pe_yuv420_to_jpeg'),
        _benchmark = lib.lookupFunction<_BenchC, _BenchD>('pe_benchmark'),
        _autoCal = lib.lookupFunction<_AutoCalC, _AutoCalD>('pe_auto_calibrate'),
        _analyze = lib.lookupFunction<_AnalyzeC, _AnalyzeD>('pe_analyze_video'),
        _videoOpen = lib.lookupFunction<_VideoOpenC, _VideoOpenC>('pe_video_open'),
        _videoInfo = lib.lookupFunction<_EngineTextC, _EngineTextD>('pe_video_info'),
        _videoSeek = lib.lookupFunction<_VideoSeekC, _VideoSeekD>('pe_video_seek'),
        _videoRead = lib.lookupFunction<_VideoReadC, _VideoReadD>('pe_video_read_jpeg'),
        _videoClose = lib.lookupFunction<_EngineVoidC, _EngineVoidD>('pe_video_close');

  static PocketEyeNative? _instance;

  /// Loads the engine library bundled with the app (once per isolate).
  static PocketEyeNative get instance => _instance ??= PocketEyeNative._(_open());

  static DynamicLibrary _open() {
    if (Platform.isWindows) return DynamicLibrary.open('pocketeye_c.dll');
    if (Platform.isMacOS) return DynamicLibrary.open('libpocketeye_c.dylib');
    final override = Platform.environment['POCKETEYE_LIB'];
    return DynamicLibrary.open(override ?? 'libpocketeye_c.so');
  }

  final _VersionC _version;
  final _TextOutD _calibrate;
  final _CreateD _create;
  final _EngineVoidD _destroy;
  final _EngineVoidD _finish;
  final _EngineTextD _pollResult;
  final _EngineTextD _stats;
  final _PushPlaneD _pushLuma;
  final _PushJpegD _pushJpeg;
  final _PushYuvD _pushYuv;
  final _LastJpegD _lastJpeg;
  final _YuvJpegD _yuvJpeg;
  final _BenchD _benchmark;
  final _AutoCalD _autoCal;
  final _AnalyzeD _analyze;
  final _VideoOpenC _videoOpen;
  final _EngineTextD _videoInfo;
  final _VideoSeekD _videoSeek;
  final _VideoReadD _videoRead;
  final _EngineVoidD _videoClose;

  String version() => _version().toDartString();

  /// Runs a C function that writes text into a buffer, growing it as needed.
  static String _text(int Function(Pointer<Utf8> buf, int len) call, {int initial = 1 << 16}) {
    var size = initial;
    while (true) {
      final buf = calloc<Uint8>(size).cast<Utf8>();
      try {
        final n = call(buf, size);
        if (n < size) return n <= 0 ? '' : buf.toDartString(length: n);
        size = n + 1;
      } finally {
        calloc.free(buf);
      }
    }
  }

  Map<String, dynamic> calibrate(String configJson) {
    final cfg = configJson.toNativeUtf8();
    try {
      return jsonDecode(_text((b, n) => _calibrate(cfg, b, n))) as Map<String, dynamic>;
    } finally {
      calloc.free(cfg);
    }
  }

  /// Finds the stumps in a still picture (see pe_auto_calibrate). Takes ~1 s:
  /// call it from a worker isolate.
  Map<String, dynamic> autoCalibrate(String configJson, Uint8List jpeg) {
    final cfg = configJson.toNativeUtf8();
    final img = calloc<Uint8>(jpeg.length);
    try {
      img.asTypedList(jpeg.length).setAll(0, jpeg);
      return jsonDecode(_text((b, n) => _autoCal(cfg, img, jpeg.length, b, n))) as Map<String, dynamic>;
    } finally {
      calloc.free(cfg);
      calloc.free(img);
    }
  }

  Map<String, dynamic> benchmark({int width = 1280, int height = 720, int frames = 180}) =>
      jsonDecode(_text((b, n) => _benchmark(width, height, frames, b, n))) as Map<String, dynamic>;

  /// Whole-file analysis (desktop). Blocks: call it from a worker isolate.
  /// [progress] is native memory (0..1000) that another isolate may read.
  String? analyzeVideo(String configJson, String videoPath, String outJsonPath, String overlayPath,
      Pointer<Int32> progress) {
    final cfg = configJson.toNativeUtf8();
    final vid = videoPath.toNativeUtf8();
    final out = outJsonPath.toNativeUtf8();
    final ovl = overlayPath.toNativeUtf8();
    final err = calloc<Uint8>(1024).cast<Utf8>();
    try {
      final rc = _analyze(cfg, vid, out, ovl, progress, err, 1024);
      return rc == 0 ? null : err.toDartString();
    } finally {
      for (final p in [cfg, vid, out, ovl, err]) {
        calloc.free(p);
      }
    }
  }

  Uint8List? yuv420ToJpeg(Uint8List y, Uint8List u, Uint8List v, int width, int height, int yStride,
      int uvStride, int uvPixelStride,
      {int quality = 90}) {
    final py = _copy(y), pu = _copy(u), pv = _copy(v);
    final cap = width * height * 3 + 4096;
    final out = calloc<Uint8>(cap);
    try {
      final n = _yuvJpeg(py, pu, pv, width, height, yStride, uvStride, uvPixelStride, quality, out, cap);
      return n > 0 ? Uint8List.fromList(out.asTypedList(n)) : null;
    } finally {
      for (final p in [py, pu, pv, out]) {
        calloc.free(p);
      }
    }
  }

  static Pointer<Uint8> _copy(Uint8List data) {
    final p = calloc<Uint8>(data.length + 16);
    p.asTypedList(data.length).setAll(0, data);
    return p;
  }
}

/// A running engine (one session: calibration + frames -> deliveries).
class NativeEngine {
  NativeEngine._(this._n, this._h);

  /// Throws [StateError] with the engine's message if the config is invalid.
  factory NativeEngine.create(String configJson) {
    final n = PocketEyeNative.instance;
    final cfg = configJson.toNativeUtf8();
    final err = calloc<Uint8>(1024).cast<Utf8>();
    try {
      final h = n._create(cfg, err, 1024);
      if (h == nullptr) throw StateError(err.toDartString());
      return NativeEngine._(n, h);
    } finally {
      calloc.free(cfg);
      calloc.free(err);
    }
  }

  final PocketEyeNative _n;
  Pointer<Void> _h;
  Pointer<Uint8> _buf = nullptr;
  int _bufSize = 0;
  Pointer<Uint8> _jpegBuf = nullptr;
  int _jpegSize = 0;

  Pointer<Uint8> _staging(int size) {
    if (size > _bufSize) {
      if (_buf != nullptr) calloc.free(_buf);
      _bufSize = size;
      _buf = calloc<Uint8>(size);
    }
    return _buf;
  }

  int pushJpeg(Uint8List jpeg, double t) {
    final p = _staging(jpeg.length);
    p.asTypedList(jpeg.length).setAll(0, jpeg);
    return _n._pushJpeg(_h, p, jpeg.length, t);
  }

  int pushLuma(Uint8List data, int width, int height, int stride, double t) {
    final p = _staging(data.length);
    p.asTypedList(data.length).setAll(0, data);
    return _n._pushLuma(_h, p, width, height, stride, t);
  }

  int pushYuv420(Uint8List y, Uint8List u, Uint8List v, int width, int height, int yStride, int uvStride,
      int uvPixelStride, double t) {
    final total = y.length + u.length + v.length + 32;
    final p = _staging(total);
    final py = p;
    final pu = p + y.length + 8;
    final pv = pu + u.length + 8;
    py.asTypedList(y.length).setAll(0, y);
    pu.asTypedList(u.length).setAll(0, u);
    pv.asTypedList(v.length).setAll(0, v);
    return _n._pushYuv(_h, py, pu, pv, width, height, yStride, uvStride, uvPixelStride, t);
  }

  /// Small JPEG of the last frame for the replay buffer.
  Uint8List? lastFrameJpeg({int maxWidth = 480, int quality = 70}) {
    if (_jpegSize == 0) {
      _jpegSize = 1 << 20;
      _jpegBuf = calloc<Uint8>(_jpegSize);
    }
    final n = _n._lastJpeg(_h, maxWidth, quality, _jpegBuf, _jpegSize);
    if (n <= 0) return null;
    return Uint8List.fromList(_jpegBuf.asTypedList(n));
  }

  void finish() => _n._finish(_h);

  /// All deliveries finished since the last call.
  List<Map<String, dynamic>> pollResults() {
    final out = <Map<String, dynamic>>[];
    while (true) {
      final s = PocketEyeNative._text((b, n) => _n._pollResult(_h, b, n));
      if (s.isEmpty) break;
      out.add(jsonDecode(s) as Map<String, dynamic>);
    }
    return out;
  }

  Map<String, dynamic> stats() =>
      jsonDecode(PocketEyeNative._text((b, n) => _n._stats(_h, b, n), initial: 1024)) as Map<String, dynamic>;

  void dispose() {
    if (_h != nullptr) _n._destroy(_h);
    _h = nullptr;
    if (_buf != nullptr) calloc.free(_buf);
    if (_jpegBuf != nullptr) calloc.free(_jpegBuf);
    _buf = nullptr;
    _jpegBuf = nullptr;
  }
}

/// Frame reader for video files (desktop builds).
class NativeVideo {
  NativeVideo._(this._n, this._h, this.info);

  static NativeVideo? open(String path) {
    final n = PocketEyeNative.instance;
    final p = path.toNativeUtf8();
    try {
      final h = n._videoOpen(p);
      if (h == nullptr) return null;
      final info = jsonDecode(PocketEyeNative._text((b, len) => n._videoInfo(h, b, len), initial: 512))
          as Map<String, dynamic>;
      return NativeVideo._(n, h, info);
    } finally {
      calloc.free(p);
    }
  }

  final PocketEyeNative _n;
  final Pointer<Void> _h;
  final Map<String, dynamic> info;
  Pointer<Uint8> _buf = nullptr;
  int _size = 0;
  final Pointer<Double> _t = calloc<Double>();

  int get width => (info['width'] as num).toInt();
  int get height => (info['height'] as num).toInt();
  double get duration => (info['duration_s'] as num?)?.toDouble() ?? 0;

  void seek(double t) => _n._videoSeek(_h, t);

  /// Next frame as JPEG with its time, or null at the end.
  (Uint8List, double)? readJpeg({int maxWidth = 0, int quality = 85}) {
    if (_size == 0) {
      _size = width * height * 3 + 4096;
      _buf = calloc<Uint8>(_size);
    }
    var n = _n._videoRead(_h, maxWidth, quality, _buf, _size, _t);
    if (n < 0) {
      calloc.free(_buf);
      _size = -n + 4096;
      _buf = calloc<Uint8>(_size);
      n = _n._videoRead(_h, maxWidth, quality, _buf, _size, _t);
    }
    if (n <= 0) return null;
    return (Uint8List.fromList(_buf.asTypedList(n)), _t.value);
  }

  void close() {
    _n._videoClose(_h);
    if (_buf != nullptr) calloc.free(_buf);
    calloc.free(_t);
  }
}
