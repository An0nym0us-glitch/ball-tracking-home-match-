import 'dart:math';

import 'package:flutter/material.dart';

/// Draws on top of a camera picture of [width] x [height] pixels (stretched to
/// the painted size):
/// - [debug] (pe_debug_json): the area watched for the ball (green) and the
///   ball candidates of the last second (yellow, the newest brightest);
/// - [ball] (pe_live_ball_json): the ball being followed, with a box around it
///   like a camera following a face, and its trail.
class EngineOverlayPainter extends CustomPainter {
  EngineOverlayPainter({required this.width, required this.height, this.debug, this.ball});
  final double width, height;
  final Map<String, dynamic>? debug;
  final Map<String, dynamic>? ball;

  @override
  void paint(Canvas canvas, Size size) {
    final sx = size.width / width, sy = size.height / height;
    Offset at(List p) => Offset((p[0] as num) * sx, (p[1] as num) * sy);
    final d = debug;
    if (d != null) {
      final corridor = d['corridor'] as List? ?? const [];
      if (corridor.length > 2) {
        final path = Path()..moveTo(at(corridor[0] as List).dx, at(corridor[0] as List).dy);
        for (final p in corridor.skip(1)) {
          path.lineTo(at(p as List).dx, at(p).dy);
        }
        path.close();
        canvas.drawPath(
            path,
            Paint()
              ..color = Colors.greenAccent
              ..style = PaintingStyle.stroke
              ..strokeWidth = 1.5);
      }
      final now = (d['t'] as num?)?.toDouble() ?? 0;
      for (final c in d['candidates'] as List? ?? const []) {
        final v = c as List;
        final age = (now - (v[0] as num)).clamp(0.0, 1.0);
        canvas.drawCircle(
            Offset((v[1] as num) * sx, (v[2] as num) * sy),
            ((v[3] as num) * sx).clamp(2.0, 12.0) + 2,
            Paint()
              ..color = Colors.yellowAccent.withValues(alpha: 1.0 - 0.8 * age)
              ..style = PaintingStyle.stroke
              ..strokeWidth = 2);
      }
    }

    final b = ball;
    if (b == null || b['visible'] != true) return;
    final trail = [for (final p in b['trail'] as List? ?? const []) at(p as List)];
    for (var i = 1; i < trail.length; ++i) {
      final a = i / trail.length;
      canvas.drawLine(
          trail[i - 1],
          trail[i],
          Paint()
            ..color = Colors.orangeAccent.withValues(alpha: 0.15 + 0.75 * a)
            ..strokeWidth = 1.5 + 2.5 * a
            ..strokeCap = StrokeCap.round);
    }
    final c = at(b['uv'] as List);
    final r = max(14.0, 3.0 * ((b['r'] as num?)?.toDouble() ?? 0) * sx);
    final predicted = b['predicted'] == true;
    final box = Paint()
      ..color = (predicted ? Colors.orangeAccent : Colors.redAccent).withValues(alpha: predicted ? 0.6 : 1.0)
      ..style = PaintingStyle.stroke
      ..strokeWidth = 2.5;
    // Corner brackets around the ball.
    final k = r * 0.45;
    for (final (dx, dy) in const [(-1.0, -1.0), (1.0, -1.0), (1.0, 1.0), (-1.0, 1.0)]) {
      final corner = c + Offset(dx * r, dy * r);
      canvas.drawLine(corner, corner - Offset(dx * k, 0), box);
      canvas.drawLine(corner, corner - Offset(0, dy * k), box);
    }
    canvas.drawCircle(c, 2.5, Paint()..color = box.color);
  }

  @override
  bool shouldRepaint(covariant EngineOverlayPainter old) =>
      old.debug != debug || old.ball != ball || old.width != width || old.height != height;
}
