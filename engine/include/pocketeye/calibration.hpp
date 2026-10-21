#pragma once

#include <optional>
#include <string>
#include <vector>

#include "pocketeye/geometry.hpp"

namespace pe {

// A user tap on the calibration frame. Names (left/right as seen from the camera):
//   batter_left_base, batter_left_top, batter_right_base, batter_right_top   (required)
//   bowler_left_base, bowler_left_top, bowler_right_base, bowler_right_top   (any visible)
//   corner_bowler_left, corner_bowler_right, corner_batter_left, corner_batter_right (optional,
//   only for a marked pitch; uses Pitch::width)
// "left"/"right" stumps are the outer stumps of each set.
struct TapPoint {
  std::string name;
  Vec2 uv;
};

struct CalibrationInput {
  int width = 0;
  int height = 0;
  Pitch pitch;
  bool length_known = true;  // false: pitch.length is only a starting guess and gets estimated
  std::vector<TapPoint> taps;
  std::optional<double> hfov_deg;  // horizontal field of view reported by the phone, if known
  double tap_sigma_px = 1.0;
};

struct CalibrationResult {
  bool ok = false;
  std::string error;
  Camera camera;
  double pitch_length = 0.0;
  double pitch_length_sigma = 0.0;  // 0 when the length was given
  double rms_px = 0.0;
  double max_px = 0.0;
  std::vector<std::string> warnings;
};

std::optional<Vec3> tapWorldPoint(const std::string& name, const Pitch& pitch);
CalibrationResult calibrateFromTaps(const CalibrationInput& input);

}  // namespace pe
