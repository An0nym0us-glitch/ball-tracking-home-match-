import 'dart:async';

import 'package:flutter/material.dart';

import '../model/delivery.dart';

/// Plays the stored frames of a delivery with the tracked ball, the fitted
/// path, the bounce and pad markers and the projection to the stumps drawn on top.
class ReplayPlayer extends StatefulWidget {
  const ReplayPlayer({super.key, required this.delivery, required this.imageWidth, required this.imageHeight});
  final Delivery delivery;
  final int imageWidth, imageHeight; // calibration image size (uv coordinates)

  @override
  State<ReplayPlayer> createState() => _ReplayPlayerState();
}

class _ReplayPlayerState extends State<ReplayPlayer> {
  int _i = 0;
  Timer? _timer;
  double _speed = 0.25; // slow motion by default
  final _zoom = TransformationController();
  bool _zoomed = false;
  Size _view = Size.zero;

  /// Where to zoom: the pad impact, else the end of the tracked path.
  Offset get _focus {
    final d = widget.delivery;
    if (d.projection.isNotEmpty) return Offset(d.projection.first.u, d.projection.first.v);
    if (d.path.isNotEmpty) return Offset(d.path.last.u, d.path.last.v);
    return Offset(widget.imageWidth / 2, widget.imageHeight / 2);
  }

  void _toggleZoom() {
    setState(() {
      _zoomed = !_zoomed;
      if (!_zoomed || _view.isEmpty) {
        _zoom.value = Matrix4.identity();
        return;
      }
      const k = 4.0;
      final p = Offset(_focus.dx * _view.width / widget.imageWidth, _focus.dy * _view.height / widget.imageHeight);
      _zoom.value = Matrix4.identity()
        ..translateByDouble(_view.width / 2 - k * p.dx, _view.height / 2 - k * p.dy, 0, 1)
        ..scaleByDouble(k, k, 1, 1);
    });
  }

  List<ReplayFrame> get _frames => widget.delivery.frames;

  @override
  void initState() {
    super.initState();
    if (_frames.isNotEmpty) _play();
  }

  void _play() {
    _timer?.cancel();
    if (_i >= _frames.length - 1) _i = 0;
    _schedule();
  }

  void _schedule() {
    if (_i >= _frames.length - 1) {
      setState(() => _timer = null);
      return;
    }
    final dt = (_frames[_i + 1].t - _frames[_i].t).clamp(0.005, 0.2) / _speed;
    _timer = Timer(Duration(microseconds: (dt * 1e6).round()), () {
      if (!mounted) return;
      setState(() => _i++);
      _schedule();
    });
  }

  void _pause() {
    _timer?.cancel();
    setState(() => _timer = null);
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (_frames.isEmpty) {
      return const AspectRatio(aspectRatio: 16 / 9, child: Center(child: Text('No frames stored for this ball')));
    }
    final f = _frames[_i];
    return Column(children: [
      Center(
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxHeight: 540),
          child: AspectRatio(
            aspectRatio: widget.imageWidth / widget.imageHeight,
            child: LayoutBuilder(builder: (context, c) {
              _view = Size(c.maxWidth, c.maxHeight);
              return ClipRect(
                child: InteractiveViewer(
                  transformationController: _zoom,
                  maxScale: 10,
                  child: Stack(fit: StackFit.expand, children: [
                    Image.memory(f.jpeg, fit: BoxFit.fill, gaplessPlayback: true),
                    CustomPaint(painter: _OverlayPainter(widget.delivery, f.t, widget.imageWidth, widget.imageHeight)),
                  ]),
                ),
              );
            }),
          ),
        ),
      ),
      Row(children: [
        IconButton(
          icon: Icon(_timer == null ? Icons.play_arrow : Icons.pause),
          onPressed: _timer == null ? _play : _pause,
        ),
        Expanded(
          child: Slider(
            value: _i.toDouble(),
            max: (_frames.length - 1).toDouble(),
            onChanged: (v) {
              _pause();
              setState(() => _i = v.round());
            },
          ),
        ),
        IconButton(
          tooltip: _zoomed ? 'Whole picture' : 'Zoom on batter',
          icon: Icon(_zoomed ? Icons.zoom_out_map : Icons.zoom_in),
          onPressed: _toggleZoom,
        ),
        DropdownButton<double>(
          value: _speed,
          items: const [
            DropdownMenuItem(value: 0.1, child: Text('0.1x')),
            DropdownMenuItem(value: 0.25, child: Text('0.25x')),
            DropdownMenuItem(value: 0.5, child: Text('0.5x')),
            DropdownMenuItem(value: 1.0, child: Text('1x')),
          ],
          onChanged: (v) => setState(() => _speed = v!),
        ),
      ]),
    ]);
  }
}

class _OverlayPainter extends CustomPainter {
  _OverlayPainter(this.d, this.t, this.w, this.h);
  final Delivery d;
  final double t;
  final int w, h;

  @override
  void paint(Canvas canvas, Size size) {
    final sx = size.width / w, sy = size.height / h;
    Offset uv(double u, double v) => Offset(u * sx, v * sy);

    final ballPaint = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.5;
    for (final o in d.obs) {
      if (o.t > t + 1e-6) break;
      ballPaint.color = switch (o.label) {
        2 => Colors.purpleAccent,
        -1 => Colors.grey,
        _ => Colors.yellowAccent,
      };
      canvas.drawCircle(uv(o.u, o.v), (o.r * sx).clamp(3.0, 30.0) + 2, ballPaint);
    }
    final pathPaint = Paint()
      ..color = Colors.orange
      ..strokeWidth = 3
      ..style = PaintingStyle.stroke;
    final path = Path();
    var started = false;
    for (final p in d.path) {
      if (p.t > t) break;
      final o = uv(p.u, p.v);
      if (!started) {
        path.moveTo(o.dx, o.dy);
        started = true;
      } else {
        path.lineTo(o.dx, o.dy);
      }
    }
    canvas.drawPath(path, pathPaint);
    final tb = d.tBounce;
    if (tb != null && t >= tb) {
      final p = d.path.firstWhere((p) => p.t >= tb, orElse: () => d.path.last);
      canvas.drawCircle(uv(p.u, p.v), 8, Paint()
        ..color = Colors.greenAccent
        ..style = PaintingStyle.stroke
        ..strokeWidth = 3);
    }
    final ti = d.tImpact;
    if (ti != null && t >= ti && d.projection.isNotEmpty) {
      final proj = Paint()
        ..color = Colors.cyanAccent
        ..strokeWidth = 3;
      for (var i = 1; i < d.projection.length; i += 2) {
        final a = d.projection[i - 1], b = d.projection[i];
        canvas.drawLine(uv(a.u, a.v), uv(b.u, b.v), proj);
      }
      final first = d.projection.first;
      canvas.drawCircle(uv(first.u, first.v), 7, Paint()..color = Colors.purpleAccent);
    }
  }

  @override
  bool shouldRepaint(covariant _OverlayPainter old) => old.t != t || old.d != d;
}
