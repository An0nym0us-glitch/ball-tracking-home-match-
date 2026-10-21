#include <gtest/gtest.h>

#include "pocketeye/lbw.hpp"

using namespace pe;

namespace {
LbwInput clearOut() {
  LbwInput in;
  in.p_hit = 1.0;
  return in;
}
}  // namespace

TEST(Lbw, ClearOut) {
  const LbwDecision d = decideLbw(clearOut());
  EXPECT_EQ(d.decision, "OUT");
  EXPECT_EQ(d.pitching.status, "in line");
  EXPECT_EQ(d.impact.status, "in line");
  EXPECT_EQ(d.wickets.status, "hitting");
}

TEST(Lbw, PitchedOutsideLegIsNotOut) {
  LbwInput in = clearOut();
  in.p_pitch_outside_leg = 0.97;
  const LbwDecision d = decideLbw(in);
  EXPECT_EQ(d.pitching.status, "outside leg");
  EXPECT_EQ(d.decision, "NOT OUT");
}

TEST(Lbw, PitchedOutsideOffIsFine) {
  LbwInput in = clearOut();
  in.p_pitch_outside_off = 0.99;
  const LbwDecision d = decideLbw(in);
  EXPECT_EQ(d.pitching.status, "outside off");
  EXPECT_EQ(d.decision, "OUT");
}

TEST(Lbw, ImpactOutsideOffDependsOnShot) {
  LbwInput in = clearOut();
  in.p_impact_outside_off = 0.98;
  EXPECT_EQ(decideLbw(in).decision, "NOT OUT");
  in.no_shot_offered = true;
  const LbwDecision d = decideLbw(in);
  EXPECT_EQ(d.impact.status, "outside off");
  EXPECT_EQ(d.decision, "OUT");
}

TEST(Lbw, ImpactOutsideLegAlwaysNotOut) {
  LbwInput in = clearOut();
  in.no_shot_offered = true;
  in.p_impact_outside_leg = 0.95;
  EXPECT_EQ(decideLbw(in).decision, "NOT OUT");
}

TEST(Lbw, MissingAndUmpiresCall) {
  LbwInput in = clearOut();
  in.p_hit = 0.02;
  EXPECT_EQ(decideLbw(in).wickets.status, "missing");
  EXPECT_EQ(decideLbw(in).decision, "NOT OUT");
  in.p_hit = 0.55;
  EXPECT_EQ(decideLbw(in).wickets.status, "umpire's call");
  EXPECT_EQ(decideLbw(in).decision, "UMPIRE'S CALL");
  // A definite failure beats umpire's call.
  in.p_pitch_outside_leg = 1.0;
  EXPECT_EQ(decideLbw(in).decision, "NOT OUT");
}

TEST(Lbw, FullTossSkipsPitching) {
  LbwInput in = clearOut();
  in.full_toss = true;
  in.p_pitch_outside_leg = 1.0;  // ignored without a bounce
  const LbwDecision d = decideLbw(in);
  EXPECT_EQ(d.pitching.status, "full toss");
  EXPECT_EQ(d.decision, "OUT");
}

TEST(Lbw, OffSideConvention) {
  // Camera behind the bowler: a right-hander's off side is on the camera's left (-x).
  EXPECT_EQ(offSign(Hand::Right), -1.0);
  EXPECT_EQ(offSign(Hand::Left), 1.0);
}

TEST(Zones, LengthScalesWithPitch) {
  EXPECT_EQ(lengthZone(1.0, 20.12), "yorker");
  EXPECT_EQ(lengthZone(4.0, 20.12), "full");
  EXPECT_EQ(lengthZone(7.0, 20.12), "good");
  EXPECT_EQ(lengthZone(9.0, 20.12), "short");
  // Same 4 m from the stumps on a 12 m terrace pitch is a good length.
  EXPECT_EQ(lengthZone(4.0, 12.0), "good");
}

TEST(Zones, Line) {
  Pitch p;
  const double r = 0.0335;
  EXPECT_EQ(lineZone(0.0, p, r), "middle stump");
  EXPECT_EQ(lineZone(0.08, p, r), "off stump");
  EXPECT_EQ(lineZone(-0.08, p, r), "leg stump");
  EXPECT_EQ(lineZone(0.2, p, r), "outside off");
  EXPECT_EQ(lineZone(0.6, p, r), "wide outside off");
  EXPECT_EQ(lineZone(-0.2, p, r), "outside leg");
}
