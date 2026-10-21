#pragma once

#include <string>

#include "pocketeye/geometry.hpp"

namespace pe {

enum class Hand { Right, Left };

// +1 when world +x is the batter's off side. The camera looks from the bowler's
// end, so a right-hander's off side is on the camera's left (-x).
inline double offSign(Hand h) { return h == Hand::Right ? -1.0 : 1.0; }

enum class Verdict { Pass, Fail, Marginal };

// Probabilities come from Monte-Carlo sampling of the trajectory fit.
struct LbwInput {
  bool full_toss = false;          // no bounce before the impact
  bool no_shot_offered = false;    // allows an impact outside off stump
  double p_pitch_outside_leg = 0;  // whole ball pitched outside leg stump
  double p_pitch_outside_off = 0;
  double p_impact_outside_leg = 0;
  double p_impact_outside_off = 0;
  double p_hit = 0;                // projected path hits the stumps
  double sure = 0.9;               // probability needed for a definite call
};

struct LbwComponent {
  std::string status;  // e.g. "in line", "outside leg", "hitting", "umpire's call"
  Verdict verdict = Verdict::Pass;
};

struct LbwDecision {
  LbwComponent pitching;
  LbwComponent impact;
  LbwComponent wickets;
  std::string decision;  // "OUT", "NOT OUT", "UMPIRE'S CALL"
};

LbwDecision decideLbw(const LbwInput& in);
const char* verdictName(Verdict v);

// Zone labels. Lengths are scaled to the actual pitch length (home pitches are short):
// yorker < 10 %, full < 30 %, good < 40 %, short >= 40 % of the pitch length from the
// batter's stumps (on a 22-yard pitch: 2 m, 6 m, 8 m).
std::string lengthZone(double length_from_stumps_m, double pitch_length_m);
// x_off: lateral position on the batter's off side (+) / leg side (-), metres.
std::string lineZone(double x_off, const Pitch& pitch, double ball_radius);

}  // namespace pe
