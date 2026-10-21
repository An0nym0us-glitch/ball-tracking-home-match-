#pragma once

#include <optional>
#include <vector>

#include "pocketeye/geometry.hpp"

namespace pe {

// Forces acting on the ball in flight: gravity, quadratic air drag (a = -k|v|v)
// and a constant sideways (x) acceleration that models swing / drift.
struct FlightParams {
  double drag_k = 0.0;
  double lateral_accel = 0.0;
};

Vec3 flightAcceleration(const Vec3& v, const FlightParams& fp);

// Ballistic path integrated with RK4 from a known state (p0, v0) at t0, covering
// [t_begin, t_end]. Queries in between use cubic Hermite interpolation.
class FlightPath {
 public:
  FlightPath() = default;
  FlightPath(const Vec3& p0, const Vec3& v0, double t0, double t_begin, double t_end,
             const FlightParams& fp, double max_step = 1.0 / 400.0);

  Vec3 position(double t) const;
  Vec3 velocity(double t) const;
  double tBegin() const { return ts_.empty() ? 0.0 : ts_.front(); }
  double tEnd() const { return ts_.empty() ? 0.0 : ts_.back(); }
  bool empty() const { return ts_.empty(); }
  // First time in [t_from, t_to] at which y crosses y_target.
  std::optional<double> timeAtY(double y_target, double t_from, double t_to) const;

 private:
  void evaluate(double t, Vec3* p, Vec3* v) const;
  std::vector<double> ts_;
  std::vector<Vec3> ps_;
  std::vector<Vec3> vs_;
  FlightParams fp_;
};

// Full delivery path: one flight before the bounce and one after it (or a single
// flight for a full toss).
struct Trajectory {
  bool has_bounce = false;
  double t_bounce = 0.0;
  FlightPath pre;   // whole flight for a full toss
  FlightPath post;  // after the bounce

  const FlightPath& pathAt(double t) const {
    return (has_bounce && t > t_bounce) ? post : pre;
  }
  Vec3 position(double t) const { return pathAt(t).position(t); }
  Vec3 velocity(double t) const { return pathAt(t).velocity(t); }
  double tBegin() const { return pre.tBegin(); }
  double tEnd() const { return has_bounce ? post.tEnd() : pre.tEnd(); }
  std::optional<double> timeAtY(double y_target, double t_from) const;
};

}  // namespace pe
