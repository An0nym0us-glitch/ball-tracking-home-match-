// Background isolates: the engine never runs on the UI thread.
import 'dart:async';
import 'dart:collection';
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';
import 'dart:math';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import '../model/delivery.dart';
import '../net/protocol.dart';
import 'native.dart';

/// Whole-video analysis in a worker isolate, with progress (0..1).
class VideoAnalysisJob {
  VideoAnalysisJob(this.configJson, this.videoPath, this.outJsonPath, this.overlayPath);
  final String configJson, videoPath, outJsonPath, overlayPath;
  final _progress = calloc<Int32>();

  double get progress => _progress.value / 1000.0;

  /// Returns the result JSON; throws with the engine's message on failure.
  Future<Map<String, dynamic>> run() async {
    _progress.value = 0;
    final addr = _progress.address;
    final cfg = configJson, vid = videoPath, out = outJsonPath, ovl = overlayPath;
    try {
      final err = await Isolate.run(
          () => PocketEyeNative.instance.analyzeVideo(cfg, vid, out, ovl, Pointer<Int32>.fromAddress(addr)));
      if (err != null) throw StateError(err);
      return jsonDecode(await File(out).readAsString()) as Map<String, dynamic>;
    } finally {
      calloc.free(_progress);
    }
  }
}

/// Messages from the workers to the UI.
sealed class WorkerEvent {}

class StatusEvent extends WorkerEvent {
  StatusEvent(this.text, {this.connected = false, this.hfovDeg});
  final String text;
  final bool connected;
  final double? hfovDeg; // camera field of view reported by the phone
}

class PreviewEvent extends WorkerEvent {
  PreviewEvent(this.jpeg, this.width, this.height, {this.snapshot = false, this.ball, this.debug});
  final Uint8List jpeg;
  final int width, height;
  final bool snapshot;
  final Map<String, dynamic>? ball; // the ball followed in this picture (see pe_live_ball_json)
  final Map<String, dynamic>? debug; // what the detector sees (see pe_debug_json), when asked for
}

class StatsEvent extends WorkerEvent {
  StatsEvent(this.stats);
  final Map<String, dynamic> stats;
}

class DeliveryEvent extends WorkerEvent {
  DeliveryEvent(this.json, this.frames);
  final Map<String, dynamic> json;
  final List<ReplayFrame> frames;
}

/// What the detector sees (phone-only debug view): small picture + detector JSON.
class DebugEvent extends WorkerEvent {
  DebugEvent(this.jpeg, this.info);
  final Uint8List jpeg;
  final Map<String, dynamic> info; // see pe_debug_json; plus 'thumb_width'
}

/// A test clip has been written (frames + timestamps.txt in [dir]).
class RecordedEvent extends WorkerEvent {
  RecordedEvent(this.dir, this.frames);
  final String dir;
  final int frames;
}

class ErrorEvent extends WorkerEvent {
  ErrorEvent(this.message);
  final String message;
}

WorkerEvent _decode(Map msg) {
  switch (msg['type']) {
    case 'status':
      return StatusEvent(msg['text'] as String,
          connected: msg['connected'] == true, hfovDeg: (msg['hfov'] as num?)?.toDouble());
    case 'preview':
      return PreviewEvent((msg['jpeg'] as TransferableTypedData).materialize().asUint8List(), msg['w'] as int,
          msg['h'] as int,
          snapshot: msg['snapshot'] == true,
          ball: msg['ball'] == null ? null : jsonDecode(msg['ball'] as String) as Map<String, dynamic>,
          debug: msg['debug'] == null ? null : jsonDecode(msg['debug'] as String) as Map<String, dynamic>);
    case 'stats':
      return StatsEvent((msg['stats'] as Map).cast<String, dynamic>());
    case 'delivery':
      final frames = <ReplayFrame>[];
      final times = msg['times'] as List;
      final jpegs = msg['frames'] as List;
      for (var i = 0; i < times.length; ++i) {
        frames.add(ReplayFrame((jpegs[i] as TransferableTypedData).materialize().asUint8List(), times[i] as double));
      }
      return DeliveryEvent(jsonDecode(msg['json'] as String) as Map<String, dynamic>, frames);
    case 'debug':
      return DebugEvent((msg['jpeg'] as TransferableTypedData).materialize().asUint8List(),
          (jsonDecode(msg['json'] as String) as Map<String, dynamic>)..['thumb_width'] = msg['tw']);
    case 'recorded':
      return RecordedEvent(msg['dir'] as String, msg['frames'] as int);
    default:
      return ErrorEvent(msg['text'] as String? ?? 'unknown worker message');
  }
}

/// Base for the two live workers: spawn, send commands, receive events.
abstract class _LiveWorker {
  Isolate? _isolate;
  SendPort? _send;
  final _events = StreamController<WorkerEvent>.broadcast();
  final _ready = Completer<void>();

  Stream<WorkerEvent> get events => _events.stream;

  Future<void> _start(void Function(List<Object>) entry, Object arg) async {
    final rp = ReceivePort();
    rp.listen((msg) {
      if (msg is SendPort) {
        _send = msg;
        _ready.complete();
      } else if (msg is Map) {
        _onMessage(msg);
        _events.add(_decode(msg));
      }
    });
    // An unexpected error in one message must not stop the worker for good:
    // keep it running and show the error.
    final errors = ReceivePort();
    errors.listen((e) {
      if (!_events.isClosed) _events.add(ErrorEvent('Worker error: ${e is List && e.isNotEmpty ? e.first : e}'));
      _onWorkerError();
    });
    _isolate = await Isolate.spawn(entry, [rp.sendPort, arg], errorsAreFatal: false, onError: errors.sendPort);
    await _ready.future;
  }

  void _onMessage(Map msg) {}
  void _onWorkerError() {}

  void send(Map<String, Object?> cmd) => _send?.send(cmd);

  /// Starts (or restarts) the engine with a session config.
  void configure(String configJson) => send({'cmd': 'config', 'json': configJson});

  Future<void> stop() async {
    send({'cmd': 'stop'});
    await Future<void>.delayed(const Duration(milliseconds: 200));
    _isolate?.kill(priority: Isolate.immediate);
    await _events.close();
  }
}

// ---------------------------------------------------------------- laptop ----

/// Laptop mode: receives the phone's stream, runs the engine, sends result
/// cards back to the phone.
class LiveServer extends _LiveWorker {
  static Future<LiveServer> start({required String token, int port = defaultPort}) async {
    final s = LiveServer();
    await s._start(_serverMain, {'token': token, 'port': port});
    return s;
  }

  void requestSnapshot() => send({'cmd': 'snapshot'});

  /// Adds what the detector sees (watched area, ball candidates) to the pictures.
  void showDebug(bool on) => send({'cmd': 'debug', 'on': on});
}

void _serverMain(List<Object> args) {
  final toUi = args[0] as SendPort;
  final opts = args[1] as Map;
  final rp = ReceivePort();
  toUi.send(rp.sendPort);
  _ServerState(toUi, opts['token'] as String, opts['port'] as int).run(rp);
}

class _ServerState {
  _ServerState(this.toUi, this.token, this.port);
  final SendPort toUi;
  final String token;
  final int port;

  NativeEngine? engine;
  String? lastConfig;
  Socket? phone;
  int? w, h;
  int? t0Us;
  final ring = ListQueue<(double, Uint8List)>(); // last seconds of frames for replays
  Uint8List? lastJpeg;
  int frames = 0, dropped = 0, sizeMismatch = 0;
  final _sw = Stopwatch()..start();
  double clockOffsetUs = 0; // phone clock - laptop clock (from ping/pong)
  final latencies = <double>[];
  double avgEngineMs = 0;
  DateTime lastPreview = DateTime(0);
  DateTime lastStats = DateTime(0);
  bool debugOn = false;

  int get nowUs => _sw.elapsedMicroseconds;

  void status(String text, {bool connected = false, double? hfov}) =>
      toUi.send({'type': 'status', 'text': text, 'connected': connected, 'hfov': ?hfov});

  Future<void> run(ReceivePort commands) async {
    commands.listen((msg) => _command(msg as Map));
    final ServerSocket server;
    try {
      server = await ServerSocket.bind(InternetAddress.anyIPv4, port);
    } catch (e) {
      toUi.send({'type': 'error', 'text': 'Cannot open port $port: $e'});
      return;
    }
    status('Waiting for the phone on port $port');
    Timer.periodic(const Duration(milliseconds: 500), (_) => _ping());
    await for (final sock in server) {
      if (phone != null) {
        sock.destroy(); // one camera at a time
        continue;
      }
      _handle(sock);
    }
  }

  void _command(Map msg) {
    switch (msg['cmd']) {
      case 'config':
        try {
          engine?.dispose();
          lastConfig = msg['json'] as String;
          engine = NativeEngine.create(_jpegStream(lastConfig!));
          status('Tracking: bowl when ready', connected: phone != null);
          _sendLock();
        } catch (e) {
          engine = null;
          toUi.send({'type': 'error', 'text': 'Engine: $e'});
        }
      case 'snapshot':
        if (lastJpeg != null) {
          toUi.send({
            'type': 'preview',
            'jpeg': TransferableTypedData.fromList([lastJpeg!]),
            'w': w ?? 0,
            'h': h ?? 0,
            'snapshot': true,
          });
        }
      case 'debug':
        debugOn = msg['on'] == true;
      case 'stop':
        phone?.destroy();
        engine?.dispose();
        engine = null;
        Isolate.exit();
    }
  }

  void _handle(Socket sock) {
    phone = sock;
    sock.setOption(SocketOption.tcpNoDelay, true);
    final parser = StreamParser();
    status('Phone connecting from ${sock.remoteAddress.address}');
    sock.listen((chunk) {
      try {
        parser.add(chunk);
        if (!parser.helloDone) {
          final line = parser.takeHello();
          if (line == null) return;
          final hello = jsonDecode(line) as Map<String, dynamic>;
          if (hello['token']?.toString() != token) {
            sock.write('${jsonEncode({'type': 'error', 'text': 'wrong pairing code'})}\n');
            sock.destroy();
            return;
          }
          w = (hello['width'] as num).toInt();
          h = (hello['height'] as num).toInt();
          sock.write('${jsonEncode({'type': 'hello_ok'})}\n');
          status('Phone connected: ${hello['device'] ?? ''} ${w}x$h @ ${hello['fps']} fps',
              connected: true, hfov: (hello['hfov'] as num?)?.toDouble());
          if (engine != null) _sendLock();
        }
        for (var m = parser.next(); m != null; m = parser.next()) {
          if (m.$1 == 'frame') {
            _frame(m.$2, m.$3 as Uint8List);
          } else {
            _pong(m.$2, m.$3 as int);
          }
        }
      } catch (e) {
        toUi.send({'type': 'error', 'text': 'Stream error: $e'});
        sock.destroy();
      }
    }, onDone: _disconnected, onError: (_) => _disconnected());
  }

  /// The phone sends every frame as its own JPEG: the engine has a detector
  /// setting for that (see forJpegStream in the engine).
  static String _jpegStream(String configJson) =>
      jsonEncode((jsonDecode(configJson) as Map<String, dynamic>)..['source'] = 'jpeg_stream');

  /// Tracking is on: the phone meters brightness and focus on the batter's
  /// stumps and then keeps them fixed (automatic exposure jumps when the bowler
  /// runs past the camera and hides the ball).
  void _sendLock() {
    final p = phone, cfg = lastConfig;
    if (p == null || cfg == null || w == null || h == null) return;
    var x = 0.5, y = 0.5;
    try {
      final taps = (jsonDecode(cfg) as Map<String, dynamic>)['taps'] as Map<String, dynamic>?;
      final a = taps?['batter_left_base'] as List?, b = taps?['batter_right_top'] as List?;
      if (a != null && b != null) {
        x = (((a[0] as num) + (b[0] as num)) / 2 / w!).clamp(0.0, 1.0);
        y = (((a[1] as num) + (b[1] as num)) / 2 / h!).clamp(0.0, 1.0);
      }
    } catch (_) {}
    p.write('${jsonEncode({'type': 'lock', 'x': x, 'y': y})}\n');
  }

  void _disconnected() {
    // Flush deliveries still waiting for more frames.
    final e = engine;
    if (e != null) {
      e.finish();
      for (final res in e.pollResults()) {
        _delivery(res);
      }
      // A finished engine accepts no more frames: start a fresh one for the next connection.
      if (lastConfig != null) {
        e.dispose();
        engine = NativeEngine.create(_jpegStream(lastConfig!));
      }
    }
    phone = null;
    t0Us = null;
    status('Phone disconnected. Waiting on port $port');
  }

  void _ping() {
    phone?.write('${jsonEncode({'type': 'ping', 't': nowUs})}\n');
    // Analyses run in the background: pick up results even while no frames arrive.
    final e = engine;
    if (e != null) {
      try {
        for (final res in e.pollResults()) {
          _delivery(res);
        }
      } catch (err) {
        toUi.send({'type': 'error', 'text': 'Result error: $err'});
      }
    }
  }

  void _pong(int laptopSentUs, int phoneUs) {
    final now = nowUs;
    final rtt = now - laptopSentUs;
    if (rtt < 0 || rtt > 2000000) return;
    // Phone time corresponds to the middle of the round trip.
    final offset = phoneUs - (laptopSentUs + rtt / 2);
    clockOffsetUs = clockOffsetUs == 0 ? offset : 0.8 * clockOffsetUs + 0.2 * offset;
  }

  void _frame(int captureUs, Uint8List jpeg) {
    final received = nowUs;
    frames++;
    lastJpeg = jpeg;
    t0Us ??= captureUs;
    final t = (captureUs - t0Us!) / 1e6;
    ring.addLast((t, jpeg));
    while (ring.isNotEmpty && ring.first.$1 < t - 8.0) {
      ring.removeFirst();
    }
    if (clockOffsetUs != 0) {
      latencies.add((received - (captureUs - clockOffsetUs)) / 1000.0);
      if (latencies.length > 60) latencies.removeAt(0);
    }
    final e = engine;
    var processMs = 0.0;
    if (e != null) {
      final sw = Stopwatch()..start();
      final rc = e.pushJpeg(jpeg, t);
      processMs = sw.elapsedMicroseconds / 1000.0;
      avgEngineMs = avgEngineMs == 0 ? processMs : 0.95 * avgEngineMs + 0.05 * processMs;
      if (rc == -2) sizeMismatch++;
      for (final res in e.pollResults()) {
        try {
          _delivery(res);
        } catch (err) {
          toUi.send({'type': 'error', 'text': 'Could not show a delivery: $err'});
        }
      }
    }
    final now = DateTime.now();
    // About 15 pictures a second: smooth enough to watch the marker follow the ball.
    if (now.difference(lastPreview).inMilliseconds >= 60) {
      lastPreview = now;
      String? ball, debug;
      if (e != null) {
        try {
          ball = jsonEncode(e.liveBall());
          if (debugOn) debug = jsonEncode(e.debug());
        } catch (_) {}
      }
      toUi.send({
        'type': 'preview',
        'jpeg': TransferableTypedData.fromList([jpeg]),
        'w': w ?? 0,
        'h': h ?? 0,
        'ball': ?ball,
        'debug': ?debug,
      });
    }
    if (now.difference(lastStats).inMilliseconds > 1000) {
      lastStats = now;
      latencies.sort();
      toUi.send({
        'type': 'stats',
        'stats': {
          'frames': frames,
          'size_mismatch': sizeMismatch,
          'engine_ms': avgEngineMs,
          if (latencies.isNotEmpty) 'stream_latency_ms': latencies[latencies.length ~/ 2],
          if (e != null) ...e.stats(),
        }
      });
    }
  }

  void _delivery(Map<String, dynamic> res) {
    final d = Delivery(res, index: 0);
    final from = d.tFirst - 0.4, to = d.tLast + 0.6;
    final sel = ring.where((f) => f.$1 >= from && f.$1 <= to).toList();
    toUi.send({
      'type': 'delivery',
      'json': jsonEncode(res),
      'times': [for (final f in sel) f.$1],
      'frames': [for (final f in sel) TransferableTypedData.fromList([f.$2])],
    });
    if (d.json['valid'] == true) {
      phone?.write('${jsonEncode({'type': 'result', 'lines': d.cardLines(), 'decision': d.decision.decision})}\n');
    }
  }
}

// ------------------------------------------------------------ phone only ----

/// Phone-only mode: camera frames from the UI isolate are analysed here.
class PhoneEngineWorker extends _LiveWorker {
  static Future<PhoneEngineWorker> start() async {
    final w = PhoneEngineWorker();
    await w._start(_phoneMain, const {});
    return w;
  }

  /// Frames sent and not yet answered. Two in flight absorb timing jitter
  /// without building up a delay.
  int _inFlight = 0;
  static const _maxInFlight = 2;
  final _sinceAnswer = Stopwatch();

  @override
  void _onMessage(Map msg) {
    if (msg['type'] == 'stats' && _inFlight > 0) {
      _inFlight--;
      _sinceAnswer.reset();
    }
  }

  @override
  void _onWorkerError() => _inFlight = 0;

  /// Sends a camera frame if the worker has room; returns false if dropped.
  bool offer(Uint8List y, Uint8List u, Uint8List v, int width, int height, int yStride, int uvStride,
      int uvPixelStride, double t,
      {int rotation = 0}) {
    if (_send == null) return false;
    // Safety net: never stay blocked if an answer got lost.
    if (_inFlight > 0 && _sinceAnswer.elapsedMilliseconds > 3000) _inFlight = 0;
    if (_inFlight >= _maxInFlight) return false;
    if (_inFlight == 0) {
      _sinceAnswer
        ..reset()
        ..start();
    }
    _inFlight++;
    send({
      'cmd': 'frame',
      'y': TransferableTypedData.fromList([y]),
      'u': TransferableTypedData.fromList([u]),
      'v': TransferableTypedData.fromList([v]),
      'w': width,
      'h': height,
      'ys': yStride,
      'uvs': uvStride,
      'uvps': uvPixelStride,
      'rot': rotation,
      't': t,
    });
    return true;
  }

  /// Live view of what the detector sees (a few pictures per second).
  void showDebug(bool on) => send({'cmd': 'debug', 'on': on});

  /// Saves the next [seconds] of frames, exactly as the engine gets them, to [dir].
  void record(String dir, double seconds) => send({'cmd': 'record', 'dir': dir, 'seconds': seconds});
}

void _phoneMain(List<Object> args) {
  final toUi = args[0] as SendPort;
  final rp = ReceivePort();
  toUi.send(rp.sendPort);
  NativeEngine? engine;
  final ring = ListQueue<(double, Uint8List)>();
  var frames = 0;
  var lastStats = -10.0;
  var engineMs = 0.0;
  var debugOn = false;
  var lastDebug = -10.0;
  // Test clip being recorded: folder, end time, frame times.
  String? recDir;
  var recUntil = 0.0;
  final recTimes = <double>[];

  void handleFrame(Map msg) {
    final e = engine;
    if (e == null) return;
    final t = msg['t'] as double;
    final y = (msg['y'] as TransferableTypedData).materialize().asUint8List();
    final u = (msg['u'] as TransferableTypedData).materialize().asUint8List();
    final v = (msg['v'] as TransferableTypedData).materialize().asUint8List();
    final sw = Stopwatch()..start();
    final rc = e.pushYuv420(y, u, v, msg['w'] as int, msg['h'] as int, msg['ys'] as int, msg['uvs'] as int,
        msg['uvps'] as int, t,
        rotation: msg['rot'] as int? ?? 0);
    engineMs = sw.elapsedMicroseconds / 1000.0;
    if (rc == -2) {
      toUi.send({'type': 'error', 'text': 'Camera size changed: calibrate again.'});
      return;
    }
    final jpeg = e.lastFrameJpeg(maxWidth: 400, quality: 60);
    if (jpeg != null) ring.addLast((t, jpeg));
    if (debugOn && jpeg != null && t - lastDebug > 0.25) {
      lastDebug = t;
      toUi.send({
        'type': 'debug',
        'jpeg': TransferableTypedData.fromList([jpeg]),
        'json': jsonEncode(e.debug()),
        'tw': 400,
      });
    }
    final dir = recDir;
    if (dir != null) {
      if (recTimes.isEmpty) recUntil += t; // the clip starts with this frame
      final full = e.lastFrameJpeg(maxWidth: 0, quality: 88);
      if (full != null) {
        File('$dir/frame_${recTimes.length.toString().padLeft(5, '0')}.jpg').writeAsBytesSync(full);
        recTimes.add(t);
      }
      if (t >= recUntil) {
        File('$dir/timestamps.txt').writeAsStringSync('${recTimes.map((x) => (x - recTimes.first).toStringAsFixed(6)).join('\n')}\n');
        toUi.send({'type': 'recorded', 'dir': dir, 'frames': recTimes.length});
        recDir = null;
      }
    }
    while (ring.isNotEmpty && ring.first.$1 < t - 8.0) {
      ring.removeFirst();
    }
    for (final res in e.pollResults()) {
      try {
        final d = Delivery(res, index: 0);
        final sel = ring.where((f) => f.$1 >= d.tFirst - 0.4 && f.$1 <= d.tLast + 0.6).toList();
        toUi.send({
          'type': 'delivery',
          'json': jsonEncode(res),
          'times': [for (final f in sel) f.$1],
          'frames': [for (final f in sel) TransferableTypedData.fromList([f.$2])],
        });
      } catch (err) {
        toUi.send({'type': 'error', 'text': 'Could not show a delivery: $err'});
      }
    }
  }

  rp.listen((m) {
    final msg = m as Map;
    switch (msg['cmd']) {
      case 'config':
        try {
          engine?.dispose();
          engine = NativeEngine.create(msg['json'] as String);
          ring.clear();
          toUi.send({'type': 'status', 'text': 'Tracking: bowl when ready', 'connected': true});
        } catch (e) {
          engine = null;
          toUi.send({'type': 'error', 'text': 'Engine: $e'});
        }
      case 'frame':
        final t = msg['t'] as double;
        try {
          handleFrame(msg);
        } catch (err) {
          toUi.send({'type': 'error', 'text': 'Frame error: $err'});
        } finally {
          frames++;
          // Always answer: it also tells the UI the worker has room again.
          Map<String, dynamic> extra = const {};
          if (t - lastStats > 1.0 && engine != null) {
            try {
              extra = engine!.stats();
            } catch (_) {}
            lastStats = t;
          }
          toUi.send({
            'type': 'stats',
            'stats': {'frames': frames, 'engine_ms': engineMs, ...extra},
          });
        }
      case 'debug':
        debugOn = msg['on'] == true;
      case 'record':
        recDir = msg['dir'] as String;
        recUntil = (msg['seconds'] as num).toDouble();
        recTimes.clear();
      case 'stop':
        engine?.dispose();
        Isolate.exit();
    }
  });
}

/// Random 6-digit pairing code.
String newPairingCode() => (Random.secure().nextInt(900000) + 100000).toString();

/// Local IPv4 addresses (for the QR code).
Future<List<String>> localAddresses() async {
  final out = <String>[];
  for (final ni in await NetworkInterface.list(type: InternetAddressType.IPv4)) {
    for (final a in ni.addresses) {
      if (!a.isLoopback) out.add(a.address);
    }
  }
  return out;
}
