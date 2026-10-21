#include "pocketeye/lbw.hpp"

namespace pe {

const char* verdictName(Verdict v) {
  switch (v) {
    case Verdict::Pass: return "pass";
    case Verdict::Fail: return "fail";
    case Verdict::Marginal: return "marginal";
  }
  return "?";
}

namespace {

constexpr const char* kUmpiresCall = "umpire's call";

LbwComponent pitching(const LbwInput& in) {
  if (in.full_toss) return {"full toss", Verdict::Pass};
  const double p_leg = in.p_pitch_outside_leg;
  if (p_leg >= in.sure) return {"outside leg", Verdict::Fail};
  if (p_leg > 1.0 - in.sure) return {kUmpiresCall, Verdict::Marginal};
  const double p_in = 1.0 - in.p_pitch_outside_leg - in.p_pitch_outside_off;
  return {p_in >= in.p_pitch_outside_off ? "in line" : "outside off", Verdict::Pass};
}

LbwComponent impact(const LbwInput& in) {
  const double p_leg = in.p_impact_outside_leg;
  const double p_off = in.p_impact_outside_off;
  const double p_in = 1.0 - p_leg - p_off;
  const char* label = "in line";
  if (p_leg > p_in && p_leg >= p_off) label = "outside leg";
  if (p_off > p_in && p_off > p_leg) label = "outside off";
  // Outside off only counts when the batter offered no shot.
  const double p_fail = in.no_shot_offered ? p_leg : p_leg + p_off;
  if (p_fail >= in.sure) return {label, Verdict::Fail};
  if (p_fail > 1.0 - in.sure) return {kUmpiresCall, Verdict::Marginal};
  return {label, Verdict::Pass};
}

LbwComponent wickets(const LbwInput& in) {
  if (in.p_hit >= in.sure) return {"hitting", Verdict::Pass};
  if (in.p_hit <= 1.0 - in.sure) return {"missing", Verdict::Fail};
  return {kUmpiresCall, Verdict::Marginal};
}

}  // namespace

LbwDecision decideLbw(const LbwInput& in) {
  LbwDecision d;
  d.pitching = pitching(in);
  d.impact = impact(in);
  d.wickets = wickets(in);
  bool fail = false, marginal = false;
  for (const LbwComponent* c : {&d.pitching, &d.impact, &d.wickets}) {
    fail |= c->verdict == Verdict::Fail;
    marginal |= c->verdict == Verdict::Marginal;
  }
  d.decision = fail ? "NOT OUT" : (marginal ? "UMPIRE'S CALL" : "OUT");
  return d;
}

std::string lengthZone(double len, double pitch_length) {
  if (len < 0) return "beyond stumps";
  const double f = len / pitch_length;
  if (f < 0.10) return "yorker";
  if (f < 0.30) return "full";
  if (f < 0.40) return "good";
  return "short";
}

std::string lineZone(double x_off, const Pitch& pitch, double r) {
  const double edge = 0.5 * pitch.stump_width + r;  // any part of the ball in line with a stump
  const double third = pitch.stump_width / 6.0;
  if (x_off > edge + 0.30) return "wide outside off";
  if (x_off > edge) return "outside off";
  if (x_off >= third) return "off stump";
  if (x_off > -third) return "middle stump";
  if (x_off >= -edge) return "leg stump";
  return "outside leg";
}

}  // namespace pe
