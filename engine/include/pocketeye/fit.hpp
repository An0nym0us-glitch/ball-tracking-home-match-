#pragma once

#include <Eigen/Core>
#include <string>
#include <vector>

#include "pocketeye/ball.hpp"
#include "pocketeye/flight.hpp"
#include "pocketeye/geometry.hpp"

namespace pe {

// One ball detection in one frame.
struct Observation {
  double t = 0.0;         // capture time (s)
  Vec2 uv = Vec2::Zero();  // ball centre (px)
  double radius_px = 0.0;  // apparent radius (px); <= 0 when unknown
};

// Parameter layouts.
// Bounce:    [xb, yb, tb, vin_x, vin_y, vin_z, a_lat, vout_x, vout_y, vout_z, log_k]
//            (bounce point on the ground plane z = ball radius, velocities just before/after)
// Full toss: [x, y, z, vx, vy, vz, a_lat, log_k] at time t_ref
namespace theta {
enum Bounce { XB = 0, YB, TB, VIX, VIY, VIZ, ALAT, VOX, VOY, VOZ, LOGK_B, kBounceSize };
enum FullToss { X = 0, Y, Z, VX, VY, VZ, ALAT_F, LOGK_F, kFullTossSize };
}  // namespace theta

struct ModelSpec {
  bool has_bounce = false;
  double t_ref = 0.0;    // full toss reference time
  double t_begin = 0.0;  // integration range
  double t_end = 0.0;
  double ball_radius = 0.0335;
};

Trajectory buildTrajectory(const ModelSpec& spec, const Eigen::VectorXd& theta,
                           double max_step = 1.0 / 250.0);

struct FitOptions {
  BallSpec ball;
  double sigma_px = 1.0;          // centre measurement noise
  double radius_sigma_px = 0.8;   // apparent radius noise: abs + rel * radius
  double radius_sigma_rel = 0.25;
  double pitch_length = 20.12;    // used to bound the initial search
  int max_outliers = 3;
  int max_iterations = 60;
  Eigen::VectorXd initial_theta;  // warm start (same layout); empty = grid-search initialisation
};

struct FitResult {
  bool ok = false;
  std::string error;
  ModelSpec spec;
  Eigen::VectorXd theta;
  Eigen::MatrixXd covariance;
  double rms_px = 0.0;
  int n_pre = 0;   // inlier observations before the bounce (or all, for a full toss)
  int n_post = 0;  // inlier observations after the bounce
  int n_outliers = 0;
  std::vector<double> outlier_times;  // capture times of rejected observations
  double cost = 0.0;
};

// pre: observations before the bounce; post: after the bounce. An empty `post`
// fits a full toss to `pre`.
FitResult fitTrajectory(const Camera& cam, const std::vector<Observation>& pre,
                        const std::vector<Observation>& post, const FitOptions& opt);

}  // namespace pe
