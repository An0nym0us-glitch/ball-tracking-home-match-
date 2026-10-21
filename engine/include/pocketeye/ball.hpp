#pragma once

#include <optional>
#include <string>

namespace pe {

enum class BallKind { Tennis, Plastic, Rubber, Leather };

// Physical defaults per ball type. Restitution, friction and drag are used as
// soft priors in the trajectory fit, not as hard constants.
struct BallSpec {
  BallKind kind = BallKind::Tennis;
  std::string name = "tennis";
  double radius = 0.0335;      // m
  double restitution = 0.70;   // vertical speed kept at the bounce (|vz_out| / |vz_in|)
  double friction = 0.80;      // along-pitch speed kept at the bounce (vy_out / vy_in)
  double drag_k = 0.020;       // quadratic drag: a = -k |v| v  (1/m)
};

BallSpec ballSpec(BallKind kind);
std::optional<BallKind> parseBallKind(const std::string& name);

}  // namespace pe
