#pragma once

#include <Eigen/Core>
#include <vector>

#include "pocketeye/detector.hpp"

namespace pe {

struct TrackPoint {
  int frame = 0;
  double t = 0.0;
  Vec2 uv = Vec2::Zero();
  double radius_px = 0.0;
  double strength = 0.0;
};

struct TrackerConfig {
  double jerk_density = 2e9;    // constant-acceleration process noise (px^2 / s^5)
  double meas_sigma_px = 1.5;
  double gate_sigma = 4.0;      // Mahalanobis gate
  double min_gate_px = 6.0;
  double rescue_gate_px = 30.0; // second-chance gate (bounce / pad impact kinks)
  double max_jump_frac = 0.15;  // first association, fraction of image width
  int max_misses = 3;
  int max_tracks = 64;
};

// 1-D constant-acceleration Kalman filter: state [position, velocity, acceleration].
struct Kalman1D {
  Eigen::Vector3d x = Eigen::Vector3d::Zero();
  Eigen::Matrix3d P = Eigen::Matrix3d::Identity();
  void predict(double dt, double q);
  void update(double z, double r);
};

struct TrackState {
  int id = 0;
  std::vector<TrackPoint> points;
  Kalman1D ku, kv;
  double t_last = 0.0;
  int misses = 0;
};

// Multi-target image-space tracker. Tracks end after `max_misses` frames without
// a matching candidate; finished tracks are handed to the trajectory analysis.
class Tracker {
 public:
  explicit Tracker(int image_width, TrackerConfig cfg = {});
  void update(int frame, double t, const std::vector<Candidate>& candidates);
  void flush();
  std::vector<std::vector<TrackPoint>> takeFinished(size_t min_points);
  const std::vector<TrackState>& active() const { return tracks_; }

 private:
  void startFilter(TrackState& tr);
  int width_;
  TrackerConfig cfg_;
  int next_id_ = 1;
  std::vector<TrackState> tracks_;
  std::vector<std::vector<TrackPoint>> finished_;
};

}  // namespace pe
