import 'dart:typed_data';

/// Number from engine JSON; NaN when missing (the engine writes null for values it could not compute).
double _d(dynamic v) => v is num ? v.toDouble() : double.nan;

/// A value with its 90 % range, as reported by the engine.
class Est {
  const Est(this.value, this.lo, this.hi);
  final double value;
  final double lo;
  final double hi;

  static Est? parse(dynamic j) {
    if (j is! Map || j['value'] is! num) return null;
    final v = _d(j['value']), lo = _d(j['lo']), hi = _d(j['hi']);
    return Est(v, lo.isNaN ? v : lo, hi.isNaN ? v : hi);
  }

  String fmt(int digits, [String unit = '']) =>
      '${value.toStringAsFixed(digits)}$unit  (${lo.toStringAsFixed(digits)} to ${hi.toStringAsFixed(digits)})';
}

class PathPoint {
  const PathPoint(this.t, this.x, this.y, this.z, this.u, this.v);
  final double t, x, y, z, u, v;

  static List<PathPoint> parse(dynamic j) {
    if (j is! List) return const [];
    return [
      for (final p in j)
        if (p['p'] is List && p['uv'] is List)
          PathPoint(_d(p['t']), _d(p['p'][0]), _d(p['p'][1]), _d(p['p'][2]), _d(p['uv'][0]), _d(p['uv'][1]))
    ];
  }
}

class ObsPoint {
  const ObsPoint(this.t, this.u, this.v, this.r, this.label);
  final double t, u, v, r;
  final int label; // 0 before bounce, 1 after bounce, 2 after impact, -1 outlier
}

/// A frame kept for the replay (JPEG bytes, capture time).
class ReplayFrame {
  const ReplayFrame(this.jpeg, this.t);
  final Uint8List jpeg;
  final double t;
}

/// One delivery as reported by the engine (see engine/src/result_json.cpp).
class Delivery {
  Delivery(this.json, {required this.index}) {
    final m = json['metrics'] as Map<String, dynamic>? ?? {};
    speed = Est.parse(m['release_speed_kmh']);
    avgSpeed = Est.parse(m['average_speed_kmh']);
    swing = Est.parse(m['swing_cm']);
    turn = Est.parse(m['turn_deg']);
    bounceLine = Est.parse(m['bounce_line_cm']);
    bounceLength = Est.parse(m['bounce_length_m']);
    heightAtStumps = Est.parse(m['height_at_stumps_cm']);
    lineAtStumps = Est.parse(m['line_at_stumps_cm']);
    impactLine = Est.parse(m['impact_line_cm']);
    impactHeight = Est.parse(m['impact_height_cm']);
    swingType = m['swing_type'] as String? ?? '';
    turnType = m['turn_type'] as String? ?? '';
    path = PathPoint.parse(json['path']);
    projection = PathPoint.parse(json['projection']);
    obs = [
      for (final o in (json['observations'] as List? ?? []))
        ObsPoint(_d(o['t']), _d(o['uv'][0]), _d(o['uv'][1]), _d(o['r']), (o['label'] as num?)?.toInt() ?? -1)
    ];
  }

  final Map<String, dynamic> json;
  final int index;
  List<ReplayFrame> frames = [];
  bool noShot = false;

  late final Est? speed, avgSpeed, swing, turn, bounceLine, bounceLength;
  late final Est? heightAtStumps, lineAtStumps, impactLine, impactHeight;
  late final String swingType, turnType;
  late final List<PathPoint> path, projection;
  late final List<ObsPoint> obs;

  Map<String, dynamic> get lbw => json['lbw'] as Map<String, dynamic>? ?? {};
  bool get lbwApplicable => lbw['applicable'] == true;
  String get impactKind => lbw['impact_kind'] as String? ?? 'none';
  bool get hasBounce => json['has_bounce'] == true;
  String get lengthZone => json['length_zone'] as String? ?? '';
  String get lineZone => json['line_zone'] as String? ?? '';
  double get tFirst => _d(json['time_s']?[0]);
  double get tLast => _d(json['time_s']?[1]);
  double? get tBounce => (json['t_bounce'] as num?)?.toDouble();
  double? get tImpact => (json['t_impact'] as num?)?.toDouble();
  List<String> get warnings => [for (final w in (json['quality']?['warnings'] as List? ?? [])) w as String];

  double _p(String k) => (lbw[k] as num?)?.toDouble() ?? 0.0;

  /// LBW decision, recomputed so the "no shot offered" switch works after the
  /// fact (same rules as engine/src/lbw.cpp).
  LbwView get decision {
    final sure = (lbw['sure'] as num?)?.toDouble() ?? 0.9;
    String pitching;
    String pVerdict;
    if (lbw['full_toss'] == true) {
      pitching = 'full toss';
      pVerdict = 'pass';
    } else {
      final pLeg = _p('p_pitched_outside_leg');
      final pOff = _p('p_pitched_outside_off');
      if (pLeg >= sure) {
        pitching = 'outside leg';
        pVerdict = 'fail';
      } else if (pLeg > 1 - sure) {
        pitching = "umpire's call";
        pVerdict = 'marginal';
      } else {
        pitching = (1 - pLeg - pOff) >= pOff ? 'in line' : 'outside off';
        pVerdict = 'pass';
      }
    }
    final iLeg = _p('p_impact_outside_leg'), iOff = _p('p_impact_outside_off');
    final iIn = 1 - iLeg - iOff;
    var impactLabel = 'in line';
    if (iLeg > iIn && iLeg >= iOff) impactLabel = 'outside leg';
    if (iOff > iIn && iOff > iLeg) impactLabel = 'outside off';
    final pFail = noShot ? iLeg : iLeg + iOff;
    String impact, iVerdict;
    if (pFail >= sure) {
      impact = impactLabel;
      iVerdict = 'fail';
    } else if (pFail > 1 - sure) {
      impact = "umpire's call";
      iVerdict = 'marginal';
    } else {
      impact = impactLabel;
      iVerdict = 'pass';
    }
    final pHit = _p('p_hitting');
    final String wickets, wVerdict;
    if (pHit >= sure) {
      wickets = 'hitting';
      wVerdict = 'pass';
    } else if (pHit <= 1 - sure) {
      wickets = 'missing';
      wVerdict = 'fail';
    } else {
      wickets = "umpire's call";
      wVerdict = 'marginal';
    }
    final verdicts = [pVerdict, iVerdict, wVerdict];
    final decision = verdicts.contains('fail')
        ? 'NOT OUT'
        : verdicts.contains('marginal')
            ? "UMPIRE'S CALL"
            : 'OUT';
    return LbwView(pitching, impact, wickets, lbwApplicable ? decision : null, pHit);
  }

  /// Short text for the phone's result card.
  List<String> cardLines() {
    final d = decision;
    return [
      if (speed != null) 'Speed ${speed!.value.toStringAsFixed(0)} km/h',
      'Length: $lengthZone${lineZone.isEmpty ? '' : ', $lineZone'}',
      if (lbwApplicable) ...[
        'Pitching: ${d.pitching}',
        'Impact: ${d.impact}',
        'Wickets: ${d.wickets}',
        'LBW: ${d.decision}',
      ] else ...[
        if (impactKind == 'stumps') 'Hit the stumps: BOWLED',
        'Stumps: ${d.wickets}',
      ],
    ];
  }
}

class LbwView {
  const LbwView(this.pitching, this.impact, this.wickets, this.decision, this.pHit);
  final String pitching, impact, wickets;
  final String? decision; // null when no pad impact was seen
  final double pHit;
}
