import 'dart:convert';
import 'dart:io';

import 'package:path_provider/path_provider.dart';

/// Stump tap points, in the order the calibration screen asks for them.
/// Left/right are as seen in the picture.
class TapSpec {
  const TapSpec(this.name, this.label, {this.required = false});
  final String name;
  final String label;
  final bool required;
}

const tapSpecs = <TapSpec>[
  TapSpec('batter_left_base', "Batter's end: LEFT stump, bottom", required: true),
  TapSpec('batter_left_top', "Batter's end: LEFT stump, top", required: true),
  TapSpec('batter_right_base', "Batter's end: RIGHT stump, bottom", required: true),
  TapSpec('batter_right_top', "Batter's end: RIGHT stump, top", required: true),
  TapSpec('bowler_left_top', "Bowler's end: LEFT stump, top (skip if not visible)"),
  TapSpec('bowler_right_top', "Bowler's end: RIGHT stump, top (skip if not visible)"),
  TapSpec('bowler_left_base', "Bowler's end: LEFT stump, bottom (skip if not visible)"),
  TapSpec('bowler_right_base', "Bowler's end: RIGHT stump, bottom (skip if not visible)"),
];

const ballTypes = ['tennis', 'plastic', 'rubber', 'leather'];

/// Everything the engine needs for one setup (pitch, stumps, camera taps, ball).
class Session {
  double pitchLength = 18.0; // stumps to stumps, metres
  /// True when the user measured the pitch length; otherwise it is measured
  /// from the picture (both sets of stumps visible) and pitchLength is that estimate.
  bool lengthMeasured = false;
  double stumpHeight = 0.71;
  double stumpWidth = 0.23; // outer edge to outer edge
  String ball = 'tennis';
  String batter = 'right';
  double? cameraHeight; // only needed if the bowler's stumps are out of frame
  double? cameraDistance;
  double hfovDeg = 65; // phone field of view (hint)
  /// How well hfovDeg is known (degrees): set from the phone's lens data; null = a rough guess.
  double? hfovSigmaDeg;
  int imageWidth = 0;
  int imageHeight = 0;
  Map<String, List<double>> taps = {};

  bool get isCalibrated =>
      imageWidth > 0 && tapSpecs.where((t) => t.required).every((t) => taps.containsKey(t.name));

  bool matchesSize(int w, int h) => w == imageWidth && h == imageHeight;

  /// Engine session config (see engine/include/pocketeye/config.hpp). Without a
  /// measured length the engine measures it from the bowler's and batter's stumps.
  String toConfigJson() => jsonEncode({
        'image': {'width': imageWidth, 'height': imageHeight},
        'pitch': {
          if (lengthMeasured) 'length_m': pitchLength,
          'stump_height_m': stumpHeight,
          'stump_width_m': stumpWidth,
        },
        'taps': taps,
        'camera_hint': {'hfov_deg': hfovDeg, if (hfovSigmaDeg != null) 'hfov_sigma_deg': hfovSigmaDeg},
        if (cameraHeight != null) 'camera_height_m': cameraHeight,
        if (cameraDistance != null) 'camera_distance_m': cameraDistance,
        'ball': ball,
        'batter': batter,
      });

  Map<String, dynamic> toJson() => {
        'pitchLength': pitchLength,
        'lengthMeasured': lengthMeasured,
        'stumpHeight': stumpHeight,
        'stumpWidth': stumpWidth,
        'ball': ball,
        'batter': batter,
        'cameraHeight': cameraHeight,
        'cameraDistance': cameraDistance,
        'hfovDeg': hfovDeg,
        'hfovSigmaDeg': hfovSigmaDeg,
        'imageWidth': imageWidth,
        'imageHeight': imageHeight,
        'taps': taps,
      };

  static Session fromJson(Map<String, dynamic> j) {
    final s = Session();
    double d(String k, double def) => (j[k] as num?)?.toDouble() ?? def;
    s.pitchLength = d('pitchLength', s.pitchLength);
    // Settings saved before the length could be measured from the picture were typed in.
    s.lengthMeasured = (j['lengthMeasured'] as bool?) ?? j.containsKey('pitchLength');
    s.stumpHeight = d('stumpHeight', s.stumpHeight);
    s.stumpWidth = d('stumpWidth', s.stumpWidth);
    s.ball = (j['ball'] as String?) ?? s.ball;
    s.batter = (j['batter'] as String?) ?? s.batter;
    s.cameraHeight = (j['cameraHeight'] as num?)?.toDouble();
    s.cameraDistance = (j['cameraDistance'] as num?)?.toDouble();
    s.hfovDeg = d('hfovDeg', s.hfovDeg);
    s.hfovSigmaDeg = (j['hfovSigmaDeg'] as num?)?.toDouble();
    s.imageWidth = (j['imageWidth'] as num?)?.toInt() ?? 0;
    s.imageHeight = (j['imageHeight'] as num?)?.toInt() ?? 0;
    final taps = j['taps'] as Map<String, dynamic>? ?? {};
    s.taps = taps.map((k, v) => MapEntry(k, (v as List).map((e) => (e as num).toDouble()).toList()));
    return s;
  }

  static Future<File> _file() async {
    final dir = await getApplicationSupportDirectory();
    return File('${dir.path}${Platform.pathSeparator}session.json');
  }

  static Future<Session> load() async {
    try {
      final f = await _file();
      if (await f.exists()) return fromJson(jsonDecode(await f.readAsString()) as Map<String, dynamic>);
    } catch (_) {
      // Corrupt or unreadable settings: start fresh.
    }
    return Session();
  }

  Future<void> save() async {
    final f = await _file();
    await f.parent.create(recursive: true);
    await f.writeAsString(jsonEncode(toJson()));
  }
}
