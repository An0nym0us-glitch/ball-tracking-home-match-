import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:qr_flutter/qr_flutter.dart';

import '../engine/ai.dart';
import '../engine/workers.dart';
import '../model/delivery.dart';
import '../model/session.dart';
import '../net/protocol.dart';
import '../widgets/batter_switch.dart';
import '../widgets/engine_overlay.dart';
import '../widgets/result_card.dart';
import 'calibration_screen.dart';
import 'delivery_screen.dart';

/// Laptop: receives the old phone's camera stream (camera_lite app), tracks
/// every delivery automatically and sends the result card back to the phone.
class LiveScreen extends StatefulWidget {
  const LiveScreen({super.key, required this.session});
  final Session session;

  @override
  State<LiveScreen> createState() => _LiveScreenState();
}

class _LiveScreenState extends State<LiveScreen> {
  LiveServer? _server;
  StreamSubscription<WorkerEvent>? _sub;
  final String _code = newPairingCode();
  List<String> _ips = [];
  String _status = 'Starting...';
  bool _connected = false;
  Uint8List? _preview;
  int _w = 0, _h = 0;
  Map<String, dynamic> _stats = {};
  final _deliveries = <Delivery>[];
  bool _engineOn = false;
  Completer<PreviewEvent>? _snapshot;
  Map<String, dynamic>? _ball; // the ball followed in the shown picture
  Map<String, dynamic>? _debug;
  bool _debugView = false;
  String? _engineError; // the engine could not start: no tracking until fixed

  Session get s => widget.session;

  @override
  void initState() {
    super.initState();
    _start();
  }

  Future<void> _start() async {
    _ips = await localAddresses();
    final server = await LiveServer.start(token: _code);
    _sub = server.events.listen(_onEvent);
    setState(() => _server = server);
  }

  void _onEvent(WorkerEvent e) {
    if (!mounted) return;
    switch (e) {
      case StatusEvent():
        setState(() {
          _status = e.text;
          if (e.text.startsWith('Tracking:')) _engineError = null;
          _connected = e.connected;
        });
        // The camera app reports its field of view (older Android API: roughly right).
        final fov = e.hfovDeg;
        if (fov != null && fov >= 30 && fov <= 120 && s.hfovSigmaDeg == null) {
          s.hfovDeg = fov;
          s.hfovSigmaDeg = 3.0;
          s.save();
        }
      case DebugEvent():
      case RecordedEvent():
        break;
      case PreviewEvent():
        if (e.snapshot) _snapshot?.complete(e);
        final sizeChanged = e.width != _w || e.height != _h;
        setState(() {
          _preview = e.jpeg;
          _w = e.width;
          _h = e.height;
          if (!e.snapshot) {
            _ball = e.ball;
            _debug = _debugView ? e.debug : null;
          }
        });
        if (sizeChanged || !_engineOn) _maybeStartEngine();
      case StatsEvent():
        setState(() => _stats = e.stats);
      case DeliveryEvent():
        if (e.json['valid'] != true) return;
        final d = Delivery(e.json, index: _deliveries.length + 1)..frames = e.frames;
        setState(() => _deliveries.insert(0, d));
      case ErrorEvent():
        setState(() {
          _status = e.message;
          if (e.message.startsWith('Engine:')) _engineError = e.message.substring(7).trim();
        });
    }
  }

  void _maybeStartEngine() {
    if (_w > 0 && s.isCalibrated && s.matchesSize(_w, _h)) {
      _server?.configure(s.toConfigJson());
      _engineOn = true;
    } else {
      _engineOn = false;
    }
  }

  /// New settings take effect for the next ball (the engine restarts).
  void _settingsChanged() {
    s.save();
    _maybeStartEngine();
  }

  /// AI assist: on/off, which processor, and what it is doing.
  Widget _aiControls(TextTheme t) {
    final ai = _stats['ai'] as Map<String, dynamic>?;
    final device = ai?['device'] as String? ?? '';
    final note = ai?['note'] as String? ?? '';
    final error = ai?['error'] as String? ?? '';
    final seen = ai?['batter_seen'] as num?;
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Wrap(crossAxisAlignment: WrapCrossAlignment.center, spacing: 12, children: [
        Row(mainAxisSize: MainAxisSize.min, children: [
          Switch(
            value: s.aiEnabled,
            onChanged: (v) {
              setState(() => s.aiEnabled = v);
              _settingsChanged();
            },
          ),
          const Text('AI assist (YOLO)'),
        ]),
        if (s.aiEnabled)
          DropdownButton<String>(
            value: s.aiDevice,
            items: [for (final e in aiDevices.entries) DropdownMenuItem(value: e.key, child: Text(e.value))],
            onChanged: (v) {
              if (v == null) return;
              setState(() => s.aiDevice = v);
              _settingsChanged();
            },
          ),
      ]),
      if (s.aiEnabled)
        Text(
          error.isNotEmpty
              ? error
              : device.isEmpty
                  ? 'AI assist starting...'
                  : 'AI on $device: people ${(ai?['person_ms'] as num? ?? 0).toStringAsFixed(0)} ms, '
                      'ball check ${(ai?['check_ms'] as num? ?? 0).toStringAsFixed(0)} ms'
                      '${seen == null || seen < 0 ? '' : seen > 0 ? '  |  batter seen' : '  |  no batter seen'}'
                      '${note.isNotEmpty ? '\n$note' : ''}',
          style: t.bodySmall!.copyWith(color: error.isNotEmpty ? Colors.red.shade700 : null),
        ),
    ]);
  }

  void _toggleDebug(bool on) {
    _server?.showDebug(on);
    setState(() {
      _debugView = on;
      if (!on) _debug = null;
    });
  }

  Future<void> _calibrate() async {
    final c = Completer<PreviewEvent>();
    _snapshot = c;
    _server?.requestSnapshot();
    final snap = await c.future.timeout(const Duration(seconds: 3), onTimeout: () => PreviewEvent(Uint8List(0), 0, 0));
    if (snap.jpeg.isEmpty || !mounted) return;
    final saved = await Navigator.push<bool>(
        context,
        MaterialPageRoute(
            builder: (_) => CalibrationScreen(session: s, jpeg: snap.jpeg, width: snap.width, height: snap.height)));
    if (saved == true) _maybeStartEngine();
    setState(() {});
  }

  @override
  void dispose() {
    _sub?.cancel();
    _server?.stop();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final qr = jsonEncode({'app': 'pocketeye', 'hosts': _ips, 'port': defaultPort, 'token': _code});
    final ready = _connected && s.isCalibrated && s.matchesSize(_w, _h) && _engineError == null;
    final t = Theme.of(context).textTheme;
    return Scaffold(
      appBar: AppBar(title: const Text('Live: phone camera to this laptop')),
      body: LayoutBuilder(builder: (context, c) {
        final wide = c.maxWidth > 900;
        final left = Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          if (!_connected) ...[
            Text('1. Open "PocketEye Camera" on the phone and scan this code.', style: t.titleMedium),
            const SizedBox(height: 8),
            Center(child: QrImageView(data: qr, size: 220, backgroundColor: Colors.white)),
            Text('Or type: ${_ips.join(' / ')}   port $defaultPort   code $_code', style: t.bodyMedium),
            const Text('Phone and laptop must be on the same Wi-Fi.'),
          ],
          const SizedBox(height: 8),
          Text(_status, style: t.titleSmall),
          if (_engineError != null)
            Text('Tracking is OFF: $_engineError. Press Calibrate and check the points and measurements.',
                style: t.titleSmall!.copyWith(color: Colors.red.shade700)),
          if (_preview != null)
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 8),
              child: AspectRatio(
                aspectRatio: _w > 0 ? _w / _h : 16 / 9,
                child: Stack(fit: StackFit.expand, children: [
                  Image.memory(_preview!, fit: BoxFit.fill, gaplessPlayback: true),
                  if (_w > 0)
                    CustomPaint(
                        painter: EngineOverlayPainter(
                            width: _w.toDouble(), height: _h.toDouble(), ball: _ball, debug: _debug)),
                ]),
              ),
            ),
          if (ready)
            BatterSwitch(
              value: s.aiActive && s.batterAuto ? 'auto' : s.batter,
              autoAvailable: s.aiActive,
              onChanged: (v) {
                setState(() {
                  s.batterAuto = v == 'auto';
                  if (v != 'auto') s.batter = v;
                  if (v == 'auto' && s.batter == 'none') s.batter = 'right';
                });
                _settingsChanged();
              },
            ),
          if (ready && aiModelPath() != null) _aiControls(t),
          if (ready)
            SwitchListTile(
              dense: true,
              contentPadding: EdgeInsets.zero,
              title: const Text('Show what the engine sees'),
              subtitle: const Text('Red box: the ball being followed. Green: area watched for the ball. '
                  'Yellow: possible balls in the last second (a thrown ball leaves a line of them). '
                  'With the AI: orange box the bowler, blue box the batter, grey other people.'),
              value: _debugView,
              onChanged: _toggleDebug,
            ),
          if (_connected)
            Wrap(spacing: 8, children: [
              FilledButton.icon(
                onPressed: _calibrate,
                icon: const Icon(Icons.straighten),
                label: Text(ready ? 'Re-calibrate' : 'Calibrate'),
              ),
            ]),
          if (_connected && !ready)
            const Padding(
              padding: EdgeInsets.only(top: 8),
              child: Text('Calibrate once with the phone in its final position. It must not move afterwards.'),
            ),
          if (_stats.isNotEmpty)
            Padding(
              padding: const EdgeInsets.only(top: 8),
              child: Text(
                'Frames ${_stats['frames']}  |  engine ${(_stats['engine_ms'] as num? ?? 0).toStringAsFixed(1)} ms/frame'
                '${_stats['stream_latency_ms'] != null ? '  |  phone to laptop ${(_stats['stream_latency_ms'] as num).toStringAsFixed(0)} ms' : ''}'
                '${(_stats['size_mismatch'] as num? ?? 0) > 0 ? '\nCamera size changed: calibrate again.' : ''}'
                '${_stats.containsKey('deliveries') ? '\nBalls found ${_stats['deliveries']}  |  other movement ignored ${_stats['rejected'] ?? 0}' : ''}'
                '${(_stats['last_reject'] as String? ?? '').isNotEmpty ? ' (last: ${_stats['last_reject']})' : ''}',
                style: t.bodySmall,
              ),
            ),
        ]);
        final right = Column(children: [
          if (_deliveries.isEmpty)
            Padding(
              padding: const EdgeInsets.all(16),
              child: Text(ready ? 'Ready. Each delivery appears here automatically.' : 'No deliveries yet.'),
            ),
          for (final d in _deliveries)
            ResultCard(
              delivery: d,
              onNoShotChanged: (v) => setState(() => d.noShot = v),
              onTap: () => Navigator.push(
                  context, MaterialPageRoute(builder: (_) => DeliveryScreen(delivery: d, session: s))),
            ),
        ]);
        return SingleChildScrollView(
          padding: const EdgeInsets.all(12),
          child: wide
              ? Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
                  Expanded(child: left),
                  const SizedBox(width: 16),
                  Expanded(child: right),
                ])
              : Column(children: [left, right]),
        );
      }),
    );
  }
}
