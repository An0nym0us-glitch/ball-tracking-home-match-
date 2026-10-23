#pragma once

#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "pocketeye/ball.hpp"
#include "pocketeye/fit.hpp"
#include "pocketeye/lbw.hpp"
#include "pocketeye/tracker.hpp"

namespace pe {

// A measured value with a 90 % interval (5th..95th percentile of the Monte-Carlo samples).
struct Estimate {
  double value = std::numeric_limits<double>::quiet_NaN();
  double lo = std::numeric_limits<double>::quiet_NaN();
  double hi = std::numeric_limits<double>::quiet_NaN();
  bool valid = false;
};

struct AnalysisConfig {
  Camera camera;
  Pitch pitch;
  BallSpec ball;
  Hand batter = Hand::Right;
  // false: bowling at empty stumps (practice). Nothing can stop the ball before
  // the stumps, so no LBW; the result is whether it would hit them.
  bool batter_present = true;
  bool no_shot_offered = false;
  double release_offset_m = 0.8;  // release point distance in front of the bowler's stumps
  double sigma_px = 1.0;          // detection noise assumed by the fit
  int mc_samples = 300;
  double decision_probability = 0.9;  // probability needed for a definite LBW component
  // Extra lateral / height uncertainty at the stumps from calibration error,
  // added in the Monte-Carlo (the fit covariance alone does not know about it).
  double calib_sigma_x_m = 0.012;
  double calib_sigma_z_m = 0.02;
  // The fit covariance assumes a perfect model; real errors (calibration, drag
  // and swing modelling, blob centre bias) are larger. Scale measured on the
  // synthetic evaluation so that the 90 % intervals cover the truth ~90 % of
  // the time; re-check on real recordings.
  double uncertainty_scale = 1.8;
  unsigned seed = 12345;
};

enum class ImpactKind { None, Pad, Lost, Stumps };
const char* impactKindName(ImpactKind k);

enum ObsLabel { kPreBounce = 0, kPostBounce = 1, kAfterImpact = 2, kOutlier = -1 };

struct DeliveryResult {
  bool valid = false;
  bool batter_present = true;  // analysed with a batter (LBW) or without (practice)
  std::string reject_reason;
  int frame_first = 0;
  int frame_last = 0;
  double t_first = 0.0;
  double t_last = 0.0;

  std::vector<Observation> observations;
  std::vector<int> labels;  // ObsLabel per observation

  bool has_bounce = false;
  double t_bounce = std::numeric_limits<double>::quiet_NaN();
  Estimate bounce_x_off_cm;  // + = off side
  Estimate length_m;         // bounce distance from the batter's stumps
  std::string length_zone;
  std::string line_zone;

  Estimate release_speed_kmh;
  Estimate average_speed_kmh;
  Estimate swing_cm;  // signed, + = towards off side
  std::string swing_type;
  Estimate turn_deg;  // signed, + = towards off side
  std::string turn_type;

  bool reaches_stumps = false;
  Estimate stumps_x_off_cm;
  Estimate stumps_height_cm;  // ball centre height at the stumps (bounce height at stumps)

  ImpactKind impact = ImpactKind::None;
  double t_impact = std::numeric_limits<double>::quiet_NaN();
  Vec3 impact_point = Vec3::Constant(std::numeric_limits<double>::quiet_NaN());
  Estimate impact_x_off_cm;
  Estimate impact_height_cm;
  double projection_m = std::numeric_limits<double>::quiet_NaN();

  bool lbw_applicable = false;
  LbwInput lbw_input;
  LbwDecision lbw;

  double rms_px = 0.0;
  int n_pre = 0;
  int n_post = 0;
  int n_after_impact = 0;
  int n_outliers = 0;
  std::vector<std::string> warnings;

  // Fitted path (t, world point) from release to impact/stumps, and the projected
  // path from the impact to the stumps (LBW only).
  std::vector<std::pair<double, Vec3>> path;
  std::vector<std::pair<double, Vec3>> projection;

  FitResult fit;
  std::vector<std::pair<std::string, double>> timings_ms;
};

DeliveryResult analyzeObservations(const std::vector<Observation>& obs, const AnalysisConfig& cfg);
DeliveryResult analyzeTrack(const std::vector<TrackPoint>& track, const AnalysisConfig& cfg);

}  // namespace pe
