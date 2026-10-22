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

/// Runs the stump search in a worker isolate. A top-level function on purpose:
/// a closure created inside the State would carry the State (and its zone)
/// along, which cannot be sent to another isolate.
Future<Map<String, dynamic>> _autoCalibrateInBackground(String configJson, Uint8List jpeg) =>
    Isolate.run(() => PocketEyeNative.instance.autoCalibrate(configJson, jpeg));

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
  bool _userTapped = false; // the user started tapping: an automatic result arriving later is not applied
  double? _typedLength; // measurements entered here (m)
  double? _typedCamHeight;
  double? _typedCamDistance;
  final _lengthCtl = TextEditingController();
  late final _camHCtl = TextEditingController(text: _cm(widget.session.cameraHeight));
  late final _camDCtl = TextEditingController(text: _cm(widget.session.cameraDistance));

  /// The points to tap: without stumps at the bowler's end only the batter's.
  late final List<TapSpec> _specs =
      widget.session.bowlerStumps ? tapSpecs : [for (final t in tapSpecs) if (t.required) t];

  static String _cm(double? m) => m == null ? '' : (m * 100).toStringAsFixed(0);

  /// Uncertainty above which a pitch length from the picture is not used without asking (m).
  static const _lengthSigmaOk = 0.4;

  @override
  void initState() {
    super.initState();
    _step = _firstMissing();
    if (widget.session.lengthMeasured) _lengthCtl.text = widget.session.pitchLength.toString();
    _findStumps();
  }

  Future<void> _findStumps() async {
    _userTapped = false;
    setState(() => _autoRunning = true);
    Map<String, dynamic> r;
    try {
      r = await _autoCalibrateInBackground(widget.session.toConfigJson(), widget.jpeg);
    } catch (e) {
      r = {'ok': false, 'message': 'Automatic calibration failed: $e'};
    }
    if (!mounted) return;
    setState(() {
      _autoRunning = false;
      _auto = r;
      final taps = r['taps'] as Map<String, dynamic>? ?? {};
      if (r['batter_found'] == true && taps.isNotEmpty && !_userTapped) {
        _taps
          ..clear()
          ..addAll(taps.map((k, v) => MapEntry(k, [for (final x in v as List) (x as num).toDouble()])));
        _autoUsed = true;
        _step = _specs.length;
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

  int get _bowlerTaps => _taps.keys.where((k) => k.startsWith('bowler')).length;

  /// Camera height and distance are needed when the bowler's stumps are not in the picture.
  bool get _needCamera =>
      _bowlerTaps < 2 &&
      (widget.session.cameraHeight == null || widget.session.cameraDistance == null) &&
      (_typedCamHeight == null || _typedCamDistance == null);

  /// The pitch length is known well enough: typed in (setup or here), or
  /// measured from the picture precisely.
  bool get _lengthOk {
    if (widget.session.lengthMeasured || _typedLength != null) return true;
    if (_bowlerTaps < 2) return false; // only the bowler's stumps let the picture give the length
    final sigma = _check?['pitch_length_sigma_m'];
    return _check?['ok'] == true && sigma is num && sigma <= _lengthSigmaOk;
  }

  double? _num(TextEditingController c) => double.tryParse(c.text.trim().replaceAll(',', '.'));

  void _useMeasurements() {
    String? problem;
    if (!_lengthOk || _typedLength != null || _lengthCtl.text.trim().isNotEmpty) {
      final v = _num(_lengthCtl);
      if (v == null || v < 5 || v > 25) {
        problem = 'Enter the pitch length in metres (5 to 25)';
      } else {
        _typedLength = v;
      }
    }
    if (problem == null && (_needCamera || _typedCamHeight != null)) {
      final h = _num(_camHCtl), d = _num(_camDCtl);
      if (h == null || h < 20 || h > 400 || d == null || d < 0 || d > 1000) {
        problem = 'Enter the camera height (20-400 cm) and its distance behind the stumps (0-1000 cm)';
      } else {
        _typedCamHeight = h / 100;
        _typedCamDistance = d / 100;
      }
    }
    if (problem != null) {
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(problem)));
      return;
    }
    setState(() {});
    _runCheck();
  }

  int _firstMissing() {
    final i = _specs.indexWhere((t) => !_taps.containsKey(t.name));
    return i < 0 ? _specs.length : i;
  }

  void _tap(TapUpDetails d) {
    _userTapped = true;
    setState(() {
      // A tap on an automatic result or a finished set starts the tapping over.
      if (_autoUsed || _step >= _specs.length) {
        _taps.clear();
        _autoUsed = false;
        _step = 0;
      }
      _taps[_specs[_step].name] = [d.localPosition.dx, d.localPosition.dy];
      _check = null;
      _step = _nextStep(_step);
    });
  }

  int _nextStep(int from) {
    for (var i = from + 1; i < _specs.length; ++i) {
      if (!_taps.containsKey(_specs[i].name)) return i;
    }
    return _specs.length;
  }

  void _skip() => setState(() => _step = _nextStep(_step));

  void _undo() {
    setState(() {
      _check = null;
      for (var i = _specs.length - 1; i >= 0; --i) {
        if (_taps.remove(_specs[i].name) != null) {
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
    if (_typedCamHeight != null && _typedCamDistance != null) {
      s.cameraHeight = _typedCamHeight;
      s.cameraDistance = _typedCamDistance;
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
    w.cameraHeight = s.cameraHeight;
    w.cameraDistance = s.cameraDistance;
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
    final pointsReady = requiredDone && !_autoRunning && _step >= _specs.length;
    final askLength = lengthFromPicture && pointsReady && !_lengthOk;
    final askCamera = pointsReady && _needCamera;
    final String banner;
    if (_autoRunning) {
      banner = 'Looking for the stumps...';
    } else if (_autoUsed) {
      banner = 'Stumps found automatically. Check that the yellow lines sit on the stumps, then Save. '
          "If they don't, tap the picture (or 'Tap them myself') to place the points yourself.";
    } else if (_step < _specs.length) {
      banner = '${_auto != null && _auto!['batter_found'] != true ? '${_auto!['message']}\n' : ''}'
          'Tap: ${_specs[_step].label}\nPinch / scroll to zoom in. Tap the centre of the stump.';
    } else {
      banner = 'All points done. Press Check. (Tapping the picture starts the points again.)';
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
                        '${lengthFromPicture && estLength != null ? '\nPitch length from the picture: ${estLength.toStringAsFixed(1)} m (+/- ${(sigma ?? 0).toStringAsFixed(1)} m).' : ''}'
                        '${warnings.isEmpty ? '' : '\nNote: ${warnings.join('\nNote: ')}'}'
                    : 'Calibration failed: ${_check!['error']}',
              ),
            ),
          if (askLength || askCamera)
            Container(
              width: double.infinity,
              color: Colors.orange.shade50,
              padding: const EdgeInsets.fromLTRB(12, 8, 12, 8),
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text(
                  askCamera && !widget.session.bowlerStumps
                      ? "Stumps only at the batter's end: the app needs three tape measurements."
                      : askCamera
                      ? "The bowler's stumps are not in the picture, so the app needs three tape measurements "
                          "(or tap the bowler's stump tops if you can see them)."
                      : estLength != null
                          ? 'The pitch length from the picture is not exact enough. Measure it with a tape.'
                          : 'Enter the pitch length.',
                ),
                Wrap(spacing: 12, crossAxisAlignment: WrapCrossAlignment.end, children: [
                  SizedBox(
                    width: 200,
                    child: TextField(
                      controller: _lengthCtl,
                      keyboardType: const TextInputType.numberWithOptions(decimal: true),
                      decoration: InputDecoration(
                          labelText: widget.session.bowlerStumps
                              ? 'Pitch length, stumps to stumps (m)'
                              : "Pitch length, bowling mark to stumps (m)"),
                    ),
                  ),
                  if (askCamera) ...[
                    SizedBox(
                      width: 200,
                      child: TextField(
                        controller: _camHCtl,
                        keyboardType: const TextInputType.numberWithOptions(decimal: true),
                        decoration: const InputDecoration(labelText: 'Camera lens height (cm)'),
                      ),
                    ),
                    SizedBox(
                      width: 240,
                      child: TextField(
                        controller: _camDCtl,
                        keyboardType: const TextInputType.numberWithOptions(decimal: true),
                        decoration: InputDecoration(
                            labelText: widget.session.bowlerStumps
                                ? "Camera behind bowler's stumps (cm)"
                                : 'Camera behind the bowling mark (cm)'),
                      ),
                    ),
                  ],
                  FilledButton.tonal(onPressed: _useMeasurements, child: const Text('Use')),
                ]),
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
                if (_step < _specs.length && !_specs[_step].required)
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
