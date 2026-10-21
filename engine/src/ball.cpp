#include "pocketeye/ball.hpp"

namespace pe {

BallSpec ballSpec(BallKind kind) {
  BallSpec s;
  s.kind = kind;
  switch (kind) {
    case BallKind::Tennis:  // taped or plain tennis ball, 57 g, 6.7 cm
      s.name = "tennis";
      s.radius = 0.0335;
      s.restitution = 0.70;
      s.friction = 0.80;
      s.drag_k = 0.020;
      break;
    case BallKind::Plastic:  // light hollow plastic ball, high drag
      s.name = "plastic";
      s.radius = 0.0350;
      s.restitution = 0.55;
      s.friction = 0.80;
      s.drag_k = 0.030;
      break;
    case BallKind::Rubber:  // solid rubber "cork" ball
      s.name = "rubber";
      s.radius = 0.0350;
      s.restitution = 0.70;
      s.friction = 0.82;
      s.drag_k = 0.012;
      break;
    case BallKind::Leather:  // season ball, 160 g, 7.2 cm
      s.name = "leather";
      s.radius = 0.0360;
      s.restitution = 0.50;
      s.friction = 0.88;
      s.drag_k = 0.0065;
      break;
  }
  return s;
}

std::optional<BallKind> parseBallKind(const std::string& name) {
  if (name == "tennis" || name == "tape") return BallKind::Tennis;
  if (name == "plastic") return BallKind::Plastic;
  if (name == "rubber") return BallKind::Rubber;
  if (name == "leather" || name == "season") return BallKind::Leather;
  return std::nullopt;
}

}  // namespace pe
