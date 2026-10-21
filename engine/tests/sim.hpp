#pragma once

#include <limits>
#include <random>
#include <vector>

#include "pocketeye/ball.hpp"
#include "pocketeye/fit.hpp"
#include "pocketeye/geometry.hpp"

namespace pe::test {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// A delivery described the way a bowler would: speed, release point, where it
// pitches, swing, turn, and optionally where it hits the pad.
struct SimDelivery {
  double speed = 25.0;                 // release speed (m/s)
  Vec3 release = Vec3(0.15, 0.8, 2.0);  // release point (m)
  double bounce_x = 0.0;               // aimed bounce point (m); NaN bounce_y = full toss
  double bounce_y = 12.0;
  double full_toss_z_at_stumps = 0.4;  // full toss: aimed height at the batter's stumps
  double lateral_accel = 0.0;          // swing (m/s^2, world x)
  double turn_deg = 0.0;               // heading change at the bounce (world, + = towards +x)
  double pad_y = kNaN;                 // impact on the pad at this y (NaN = no pad)
  double fps = 30.0;
  double noise_px = 0.5;
  double t_release = 1.0;
  double hidden_after_release_s = 0.06;  // bowler's body hides the ball at release
};

struct SimTruth {
  bool has_bounce = false;
  double t_bounce = kNaN;
  Vec3 bounce = Vec3::Constant(kNaN);
  double release_speed = kNaN;
  double swing = kNaN;  // world x deviation at bounce (or end) from the release line
  double turn_deg = kNaN;
  Vec3 at_stumps = Vec3::Constant(kNaN);  // pad-free path at y = L
  double t_impact = kNaN;
  Vec3 impact = Vec3::Constant(kNaN);
};

struct SimOutput {
  std::vector<Observation> obs;
  SimTruth truth;
};

Camera standardCamera(double pitch_length, int width = 1280, int height = 720, double hfov_deg = 65.0);

SimOutput simulate(const SimDelivery& d, const Camera& cam, const Pitch& pitch, const BallSpec& ball,
                   std::mt19937& rng);

}  // namespace pe::test
