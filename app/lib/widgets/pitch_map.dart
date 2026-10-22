import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../model/delivery.dart';

/// Top view (line and length) and side view (height) of the fitted path, in
/// metres, with the stumps and the projection to the stumps.
class PitchMap extends StatelessWidget {
  const PitchMap({super.key, required this.delivery, required this.pitchLength, required this.stumpWidth,
      required this.stumpHeight, required this.batter});
  final Delivery delivery;
  final double pitchLength, stumpWidth, stumpHeight;
  final String batter;

  @override
  Widget build(BuildContext context) {
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Text('Top view (batter at the top)', style: Theme.of(context).textTheme.titleSmall),
      SizedBox(height: 260, child: CustomPaint(painter: _TopView(delivery, pitchLength, stumpWidth, batter), size: Size.infinite)),
      const SizedBox(height: 12),
      Text('Side view (bowler on the left)', style: Theme.of(context).textTheme.titleSmall),
      SizedBox(height: 140, child: CustomPaint(painter: _SideView(delivery, pitchLength, stumpHeight), size: Size.infinite)),
    ]);
  }
}

class _TopView extends CustomPainter {
  _TopView(this.d, this.L, this.sw, this.batter);
  final Delivery d;
  final double L, sw;
  final String batter;

  @override
  void paint(Canvas canvas, Size size) {
    // Show the last 60 % of the pitch, 1.6 m wide, x across, y up the screen.
    const halfW = 0.8;
    final y0 = L * 0.4;
    final sx = size.width / (2 * halfW), sy = size.height / (L - y0 + 0.6);
    final s = math.min(sx, sy);
    final cx = size.width / 2;
    Offset p(double x, double y) => Offset(cx + x * s, size.height - (y - y0) * s);

    canvas.drawRect(Rect.fromPoints(p(-0.75, y0), p(0.75, L + 0.5)), Paint()..color = const Color(0xFFD8C08A));
    final zone = Paint()..color = Colors.black12;
    canvas.drawRect(Rect.fromPoints(p(-sw / 2, y0), p(sw / 2, L)), zone);
    final stump = Paint()
      ..color = Colors.brown.shade800
      ..strokeWidth = 4;
    canvas.drawLine(p(-sw / 2, L), p(sw / 2, L), stump);
    final label = TextPainter(
        text: TextSpan(
            text: batter == 'left' ? 'leg side <- | -> off side' : 'off side <- | -> leg side',
            style: const TextStyle(fontSize: 11, color: Colors.black87)),
        textDirection: TextDirection.ltr)
      ..layout();
    label.paint(canvas, p(0, y0) - Offset(label.width / 2, label.height + 2));

    _line(canvas, [for (final q in d.path) if (q.y >= y0) p(q.x, q.y)], Colors.orange, 3);
    _line(canvas, [for (final q in d.projection) p(q.x, q.y)], Colors.cyan.shade700, 3, dashed: true);
    final tb = d.tBounce;
    if (tb != null) {
      final b = d.path.firstWhere((q) => q.t >= tb, orElse: () => d.path.last);
      canvas.drawCircle(p(b.x, b.y), 6, Paint()..color = Colors.green.shade700);
    }
    if (d.projection.isNotEmpty) {
      final i = d.projection.first;
      canvas.drawCircle(p(i.x, i.y), 6, Paint()..color = Colors.purple);
    }
  }

  @override
  bool shouldRepaint(covariant _TopView old) => old.d != d;
}

class _SideView extends CustomPainter {
  _SideView(this.d, this.L, this.sh);
  final Delivery d;
  final double L, sh;

  @override
  void paint(Canvas canvas, Size size) {
    const zMax = 2.4;
    final sx = size.width / (L + 0.8), sz = size.height / zMax;
    Offset p(double y, double z) => Offset(y * sx, size.height - z * sz);
    canvas.drawLine(p(0, 0), p(L + 0.8, 0), Paint()
      ..color = Colors.brown
      ..strokeWidth = 2);
    final stump = Paint()
      ..color = Colors.brown.shade800
      ..strokeWidth = 3;
    canvas.drawLine(p(0, 0), p(0, sh), stump);
    canvas.drawLine(p(L, 0), p(L, sh), stump);
    _line(canvas, [for (final q in d.path) p(q.y, q.z)], Colors.orange, 2.5);
    _line(canvas, [for (final q in d.projection) p(q.y, q.z)], Colors.cyan.shade700, 2.5, dashed: true);
  }

  @override
  bool shouldRepaint(covariant _SideView old) => old.d != d;
}

void _line(Canvas canvas, List<Offset> pts, Color color, double width, {bool dashed = false}) {
  final paint = Paint()
    ..color = color
    ..strokeWidth = width
    ..strokeCap = StrokeCap.round;
  for (var i = 1; i < pts.length; ++i) {
    if (dashed && i.isOdd) continue;
    canvas.drawLine(pts[i - 1], pts[i], paint);
  }
}
