import 'dart:isolate';
import 'dart:typed_data';

import 'package:flutter/material.dart';

import '../engine/native.dart';
import '../model/session.dart';

/// Calibration on a still frame: the engine finds the stumps by itself (the
/// user checks the result), or the user taps them. The camera is solved from
/// the stump points; the pitch length is measured from them when possible.
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
  bool _autoRunning = false;
  Map<String, dynamic>? _auto; // automatic result
  bool _autoUsed = false; // the current points came from the automatic search
  double? _typedLength; // pitch length entered here
  final _lengthCtl = TextEditingController();

  /// Uncertainty above which a pitch length from the picture is not used without asking (m).
  static const _lengthSigmaOk = 0.4;

  @override
  void initState() {
    super.initState();
    _step = _firstMissing();
    _findStumps();
  }

  Future<void> _findStumps() async {
    setState(() => _autoRunning = true);
    final cfg = widget.session.toConfigJson();
    final jpeg = widget.jpeg;
    Map<String, dynamic> r;
    try {
      r = await Isolate.run(() => PocketEyeNative.instance.autoCalibrate(cfg, jpeg));
    } catch (e) {
      r = {'ok': false, 'message': 'Automatic calibration failed: $e'};
    }
    if (!mounted) return;
    setState(() {
      _autoRunning = false;
      _auto = r;
      final taps = r['taps'] as Map<String, dynamic>? ?? {};
      if (r['batter_found'] == true && taps.isNotEmpty) {
        _taps
          ..clear()
          ..addAll(taps.map((k, v) => MapEntry(k, [for (final x in v as List) (x as num).toDouble()])));
        _autoUsed = true;
        _step = tapSpecs.length;
        final len = r['pitch_length_m'];
        if (len is num && _lengthCtl.text.isEmpty) _lengthCtl.text = len.toStringAsFixed(1);
      }
    });
    if (_autoUsed) _runCheck();
  }

  void _tapManually() {
    setState(() {
      _taps.clear();
      _check = null;
      _autoUsed = false;
      _step = 0;
    });
  }

  /// The pitch length is known well enough: typed in (setup or here), or
  /// measured from the picture precisely.
  bool get _lengthOk {
    if (widget.session.lengthMeasured || _typedLength != null) return true;
    final sigma = _check?['pitch_length_sigma_m'];
    return _check?['ok'] == true && sigma is num && sigma <= _lengthSigmaOk;
  }

  void _useTypedLength() {
    final v = double.tryParse(_lengthCtl.text.replaceAll(',', '.'));
    if (v == null || v < 5 || v > 25) {
      ScaffoldMessenger.of(context)
          .showSnackBar(const SnackBar(content: Text('Enter the pitch length in metres (5 to 25)')));
      return;
    }
    setState(() => _typedLength = v);
    _runCheck();
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
    if (_typedLength != null) {
      s.pitchLength = _typedLength!;
      s.lengthMeasured = true;
    }
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
    w.lengthMeasured = s.lengthMeasured;
    w.pitchLength = s.lengthMeasured
        ? s.pitchLength
        : ((_check?['pitch_length_m'] as num?)?.toDouble() ?? s.pitchLength); // measured from the picture
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
    final lengthFromPicture = !widget.session.lengthMeasured && _typedLength == null;
    final sigma = (_check?['pitch_length_sigma_m'] as num?)?.toDouble();
    final estLength = (_check?['pitch_length_m'] as num?)?.toDouble();
    // Ask for the length when the picture cannot give it (precisely enough).
    final askLength = lengthFromPicture && (_check != null || (_auto != null && !_autoRunning)) && !_lengthOk;
    final String banner;
    if (_autoRunning) {
      banner = 'Looking for the stumps...';
    } else if (_autoUsed) {
      banner = 'Stumps found automatically. Check that the yellow lines sit on the stumps, then Save. '
          "If they don't, press 'Tap them myself'.";
    } else if (_step < tapSpecs.length) {
      banner = '${_auto != null && _auto!['batter_found'] != true ? '${_auto!['message']}\n' : ''}'
          'Tap: ${tapSpecs[_step].label}\nPinch / scroll to zoom in. Tap the centre of the stump.';
    } else {
      banner = 'All points done. Press Check.';
    }
    final warnings = [for (final w in (_check?['warnings'] as List? ?? [])) w.toString()];
    return Scaffold(
      appBar: AppBar(title: const Text('Calibrate')),
      body: Column(
        children: [
          Container(
            width: double.infinity,
            color: Theme.of(context).colorScheme.primaryContainer,
            padding: const EdgeInsets.all(12),
            child: Text(banner, style: Theme.of(context).textTheme.titleMedium),
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
                  onTapUp: _autoRunning || _autoUsed ? null : _tap,
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
                        '${lengthFromPicture && estLength != null ? '\nPitch length from the picture: ${estLength.toStringAsFixed(1)} m (+/- ${(sigma ?? 0).toStringAsFixed(1)} m).' : ''}'
                        '${warnings.isEmpty ? '' : '\nNote: ${warnings.join('\nNote: ')}'}'
                    : 'Calibration failed: ${_check!['error']}',
              ),
            ),
          if (askLength)
            Container(
              width: double.infinity,
              color: Colors.orange.shade50,
              padding: const EdgeInsets.fromLTRB(12, 8, 12, 8),
              child: Row(children: [
                Expanded(
                  child: TextField(
                    controller: _lengthCtl,
                    keyboardType: const TextInputType.numberWithOptions(decimal: true),
                    decoration: InputDecoration(
                      labelText: 'Pitch length, stumps to stumps (m)',
                      helperText: estLength != null
                          ? 'The picture is not exact enough for this. Measure it with a tape if you can.'
                          : "The bowler's stumps are not in the picture: enter the pitch length.",
                      helperMaxLines: 2,
                    ),
                  ),
                ),
                const SizedBox(width: 8),
                FilledButton.tonal(onPressed: _useTypedLength, child: const Text('Use')),
              ]),
            ),
          Padding(
            padding: const EdgeInsets.all(8),
            child: Wrap(spacing: 8, runSpacing: 8, children: [
              if (_autoUsed)
                OutlinedButton.icon(
                    onPressed: _tapManually, icon: const Icon(Icons.touch_app), label: const Text('Tap them myself'))
              else ...[
                OutlinedButton.icon(
                    onPressed: _taps.isEmpty ? null : _undo, icon: const Icon(Icons.undo), label: const Text('Undo')),
                if (_step < tapSpecs.length && !tapSpecs[_step].required)
                  OutlinedButton(onPressed: _skip, child: const Text('Skip (not visible)')),
                OutlinedButton.icon(
                    onPressed: _autoRunning ? null : _findStumps,
                    icon: const Icon(Icons.auto_fix_high),
                    label: const Text('Find automatically')),
                FilledButton.tonal(onPressed: requiredDone ? _runCheck : null, child: const Text('Check')),
              ],
              FilledButton(onPressed: ok && _lengthOk ? _save : null, child: const Text('Save calibration')),
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
