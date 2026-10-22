#pragma once

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <vector>

#include "pocketeye/calibration.hpp"

namespace pe {

// One set of three stumps found in the picture. Taps use the calibration
// names ("left"/"right" = outer stumps as seen in the picture).
struct StumpSetCandidate {
  double score = 0.0;  // bar contrast (grey levels) minus penalties
  bool light = true;   // stumps lighter than the background
  bool base_visible = true;
  std::vector<TapPoint> taps;
};

struct AutoCalibration {
  bool ok = false;           // batter's stumps found (and, if used, consistent with the bowler's)
  std::string message;       // for the user
  std::vector<TapPoint> taps;
  bool batter_found = false;
  bool bowler_found = false;
  bool length_estimated = false;  // pitch length measured from the two stump sets
  bool length_uncertain = false;  // ... but not precisely enough: ask the user to measure it
  CalibrationResult calibration;  // when ok and enough taps for a solve
};

// Batter-end stumps: three thin, evenly spaced vertical bars, as tall as the
// stump height / spacing ratio says. Best first.
std::vector<StumpSetCandidate> findBatterStumps(const cv::Mat& gray, const Pitch& pitch, int max_candidates = 4);
// Bowler-end stumps: close to the camera, large, usually cut off by the bottom
// of the picture (only their tops are tapped then). Best first.
std::vector<StumpSetCandidate> findBowlerStumps(const cv::Mat& gray, const Pitch& pitch, int max_candidates = 4);

// Finds both stump sets and checks them against each other with a full
// calibration. `in` gives the image size, stump sizes, field of view and, if
// known, the pitch length and camera measurements (its taps are ignored).
AutoCalibration autoCalibrate(const cv::Mat& image, const CalibrationInput& in);

nlohmann::json autoCalibrationToJson(const AutoCalibration& a);

}  // namespace pe
