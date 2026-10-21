import 'dart:typed_data';

import 'package:flutter/material.dart';

import '../engine/native.dart';
import '../model/session.dart';

/// Tap the stumps on a still frame; the engine solves the camera from them.
class CalibrationScreen extends StatefulWidget {
  const CalibrationScreen(
      {super.key, required this.session, required this.jpeg, required this.width, required this.height});
  final Session session;
  final Uint8List jpeg;
  final int width, height;

  @override
  State<CalibrationScreen> createState() => _CalibrationScreenState();
}

class _CalibrationScreenState extends State<CalibrationScreen> {
  final _viewer = TransformationController();
  late final Map<String, List<double>> _taps =
      widget.session.matchesSize(widget.width, widget.height) ? {...widget.session.taps} : {};
  int _step = 0;
  Map<String, dynamic>? _check;
  bool _fitted = false;

  @override
  void initState() {
    super.initState();
    _step = _firstMissing();
  }

  int _firstMissing() {
    final i = tapSpecs.indexWhere((t) => !_taps.containsKey(t.name));
    return i < 0 ? tapSpecs.length : i;
  }

  void _tap(TapUpDetails d) {
    if (_step >= tapSpecs.length) return;
    setState(() {
      _taps[tapSpecs[_step].name] = [d.localPosition.dx, d.localPosition.dy];
      _check = null;
      _step = _nextStep(_step);
    });
  }

  int _nextStep(int from) {
    for (var i = from + 1; i < tapSpecs.length; ++i) {
      if (!_taps.containsKey(tapSpecs[i].name)) return i;
    }
    return tapSpecs.length;
  }

  void _skip() => setState(() => _step = _nextStep(_step));

  void _undo() {
    setState(() {
      _check = null;
      for (var i = tapSpecs.length - 1; i >= 0; --i) {
        if (_taps.remove(tapSpecs[i].name) != null) {
          _step = i;
          return;
        }
      }
      _step = 0;
    });
  }

  Session _candidate() {
    final s = Session.fromJson(widget.session.toJson());
    s.taps = {..._taps};
    s.imageWidth = widget.width;
    s.imageHeight = widget.height;
    return s;
  }

  void _runCheck() {
    final result = PocketEyeNative.instance.calibrate(_candidate().toConfigJson());
    setState(() => _check = result);
  }

  Future<void> _save() async {
    final s = _candidate();
    final w = widget.session;
    w.taps = s.taps;
    w.imageWidth = s.imageWidth;
    w.imageHeight = s.imageHeight;
    await w.save();
    if (mounted) Navigator.pop(context, true);
  }

  void _fit(BoxConstraints c) {
    if (_fitted) return;
    _fitted = true;
    final scale = (c.maxWidth / widget.width).clamp(0.05, 10.0).toDouble();
    final s2 = (c.maxHeight / widget.height).clamp(0.05, 10.0).toDouble();
    final s = scale < s2 ? scale : s2;
    _viewer.value = Matrix4.identity()..scaleByDouble(s, s, 1, 1);
  }

  @override
  Widget build(BuildContext context) {
    final requiredDone = tapSpecs.where((t) => t.required).every((t) => _taps.containsKey(t.name));
    final ok = _check?['ok'] == true;
    final warnings = [for (final w in (_check?['warnings'] as List? ?? [])) w.toString()];
    return Scaffold(
      appBar: AppBar(title: const Text('Calibrate: tap the stumps')),
      body: Column(
        children: [
          Container(
            width: double.infinity,
            color: Theme.of(context).colorScheme.primaryContainer,
            padding: const EdgeInsets.all(12),
            child: Text(
              _step < tapSpecs.length
                  ? 'Tap: ${tapSpecs[_step].label}\nPinch / scroll to zoom in. Tap the centre of the stump.'
                  : 'All points done. Press Check.',
              style: Theme.of(context).textTheme.titleMedium,
            ),
          ),
          Expanded(
            child: LayoutBuilder(builder: (context, c) {
              _fit(c);
              return InteractiveViewer(
                transformationController: _viewer,
                constrained: false,
                minScale: 0.05,
                maxScale: 20,
                boundaryMargin: const EdgeInsets.all(200),
                child: GestureDetector(
                  onTapUp: _tap,
                  child: SizedBox(
                    width: widget.width.toDouble(),
                    height: widget.height.toDouble(),
                    child: Stack(children: [
                      Positioned.fill(child: Image.memory(widget.jpeg, fit: BoxFit.fill, gaplessPlayback: true)),
                      Positioned.fill(
                        child: CustomPaint(painter: _TapPainter(_taps, _check?['overlay'] as List?, _viewer)),
                      ),
                    ]),
                  ),
                ),
              );
            }),
          ),
          if (_check != null)
            Container(
              width: double.infinity,
              padding: const EdgeInsets.all(12),
              color: ok ? Colors.green.shade50 : Colors.red.shade50,
              child: Text(
                ok
                    ? 'Calibration OK: tap error ${(_check!['rms_px'] as num).toStringAsFixed(1)} px. '
                        'Camera ${(_check!['camera_position_m'][2] as num).toStringAsFixed(2)} m high, '
                        '${(-(_check!['camera_position_m'][1] as num)).toStringAsFixed(2)} m behind the bowler\'s stumps. '
                        'The yellow lines must sit on the stumps.'
                        '${warnings.isEmpty ? '' : '\nWarning: ${warnings.join('\nWarning: ')}'}'
                    : 'Calibration failed: ${_check!['error']}',
              ),
            ),
          Padding(
            padding: const EdgeInsets.all(8),
            child: Wrap(spacing: 8, runSpacing: 8, children: [
              OutlinedButton.icon(onPressed: _taps.isEmpty ? null : _undo, icon: const Icon(Icons.undo), label: const Text('Undo')),
              if (_step < tapSpecs.length && !tapSpecs[_step].required)
                OutlinedButton(onPressed: _skip, child: const Text('Skip (not visible)')),
              FilledButton.tonal(onPressed: requiredDone ? _runCheck : null, child: const Text('Check')),
              FilledButton(onPressed: ok ? _save : null, child: const Text('Save calibration')),
            ]),
          ),
        ],
      ),
    );
  }
}

class _TapPainter extends CustomPainter {
  _TapPainter(this.taps, this.overlay, this.viewer) : super(repaint: viewer);
  final Map<String, List<double>> taps;
  final List? overlay;
  final TransformationController viewer;

  @override
  void paint(Canvas canvas, Size size) {
    // Keep markers a constant size on screen whatever the zoom.
    final scale = viewer.value.getMaxScaleOnAxis();
    final px = 1 / (scale <= 0 ? 1 : scale);
    if (overlay != null) {
      final line = Paint()
        ..color = Colors.yellowAccent
        ..strokeWidth = 1.5 * px;
      for (final l in overlay!) {
        final v = (l as List).map((e) => (e as num).toDouble()).toList();
        canvas.drawLine(Offset(v[0], v[1]), Offset(v[2], v[3]), line);
      }
    }
    final mark = Paint()
      ..color = Colors.redAccent
      ..strokeWidth = 1.5 * px
      ..style = PaintingStyle.stroke;
    for (final e in taps.entries) {
      final o = Offset(e.value[0], e.value[1]);
      canvas.drawCircle(o, 6 * px, mark);
      canvas.drawLine(o - Offset(10 * px, 0), o + Offset(10 * px, 0), mark);
      canvas.drawLine(o - Offset(0, 10 * px), o + Offset(0, 10 * px), mark);
    }
  }

  @override
  bool shouldRepaint(covariant _TapPainter old) => old.taps != taps || old.overlay != overlay;
}
