#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>

#include "pocketeye/analysis.hpp"
#include "sim.hpp"

using namespace pe;

namespace {

AnalysisConfig makeConfig(double L, BallKind kind = BallKind::Tennis, Hand hand = Hand::Right) {
  AnalysisConfig cfg;
  cfg.pitch.length = L;
  cfg.camera = test::standardCamera(L);
  cfg.ball = ballSpec(kind);
  cfg.batter = hand;
  cfg.sigma_px = 0.8;
  cfg.mc_samples = 200;
  return cfg;
}

DeliveryResult runDelivery(const test::SimDelivery& d, const AnalysisConfig& cfg, unsigned seed,
                           test::SimOutput* sim_out = nullptr) {
  std::mt19937 rng(seed);
  const test::SimOutput s = test::simulate(d, cfg.camera, cfg.pitch, cfg.ball, rng);
  if (sim_out) *sim_out = s;
  return analyzeObservations(s.obs, cfg);
}

// Right-hander: off side is world -x, leg side is world +x.
test::SimDelivery straightOne(double L) {
  test::SimDelivery d;
  d.speed = 22.0;
  d.release = Vec3(0.1, 0.8, 1.8);
  d.bounce_x = 0.0;
  d.bounce_y = 0.78 * L;  // full of a length: stays below stump height
  d.pad_y = L - 1.0;
  d.noise_px = 0.6;
  return d;
}

}  // namespace

TEST(Analysis, PlumbLbwIsOut) {
  const AnalysisConfig cfg = makeConfig(17.0, BallKind::Leather);
  test::SimOutput s;
  const DeliveryResult r = runDelivery(straightOne(17.0), cfg, 1, &s);
  ASSERT_LT(s.truth.at_stumps.z(), 0.6);
  ASSERT_LT(std::abs(s.truth.at_stumps.x()), 0.06);
  ASSERT_TRUE(r.valid) << r.reject_reason;
  EXPECT_TRUE(r.has_bounce);
  EXPECT_EQ(r.impact, ImpactKind::Pad);
  ASSERT_TRUE(r.lbw_applicable);
  EXPECT_EQ(r.lbw.pitching.status, "in line");
  EXPECT_EQ(r.lbw.impact.status, "in line");
  EXPECT_EQ(r.lbw.wickets.status, "hitting");
  EXPECT_EQ(r.lbw.decision, "OUT");
  EXPECT_NEAR(r.release_speed_kmh.value, 22.0 * 3.6, 0.06 * 22.0 * 3.6);
  EXPECT_NEAR(r.length_m.value, 17.0 - s.truth.bounce.y(), 0.35);
  EXPECT_GT(r.n_after_impact, 0);
}

TEST(Analysis, PitchedOutsideLegIsNotOut) {
  const AnalysisConfig cfg = makeConfig(17.0);
  test::SimDelivery d = straightOne(17.0);
  d.bounce_x = 0.35;  // leg side for a right-hander
  d.turn_deg = -4.0;  // spins back towards the stumps
  const DeliveryResult r = runDelivery(d, cfg, 2);
  ASSERT_TRUE(r.valid) << r.reject_reason;
  EXPECT_EQ(r.lbw.pitching.status, "outside leg");
  EXPECT_EQ(r.lbw.decision, "NOT OUT");
  EXPECT_LT(r.bounce_x_off_cm.value, -25.0);  // negative = leg side
}

TEST(Analysis, BouncingOverTheStumpsIsMissing) {
  const AnalysisConfig cfg = makeConfig(17.0);
  test::SimDelivery d = straightOne(17.0);
  d.speed = 27.0;
  d.bounce_y = 0.45 * 17.0;  // short and steep
  d.pad_y = 17.0 - 0.8;
  const DeliveryResult r = runDelivery(d, cfg, 3);
  ASSERT_TRUE(r.valid) << r.reject_reason;
  EXPECT_GT(r.stumps_height_cm.value, 80.0);
  EXPECT_EQ(r.lbw.wickets.status, "missing");
  EXPECT_EQ(r.lbw.decision, "NOT OUT");
}

TEST(Analysis, ImpactOutsideOffNeedsNoShot) {
  AnalysisConfig cfg = makeConfig(17.0);
  test::SimDelivery d = straightOne(17.0);
  d.release = Vec3(-0.45, 0.8, 1.8);
  d.bounce_x = -0.45;   // well outside off for a right-hander
  d.turn_deg = 7.0;     // big off-break back in towards the stumps
  d.pad_y = 17.0 - 2.4; // batter strides out: pad outside off, in front
  test::SimOutput s;
  DeliveryResult r = runDelivery(d, cfg, 4, &s);
  ASSERT_TRUE(r.valid) << r.reject_reason;
  ASSERT_LT(s.truth.impact.x(), -(0.5 * cfg.pitch.stump_width + cfg.ball.radius) - 0.05);
  EXPECT_EQ(r.lbw.impact.status, "outside off");
  EXPECT_EQ(r.lbw.decision, "NOT OUT");
  cfg.no_shot_offered = true;
  r = runDelivery(d, cfg, 4);
  ASSERT_TRUE(r.valid);
  EXPECT_NE(r.lbw.decision, "NOT OUT") << r.lbw.wickets.status;
}

TEST(Analysis, FullTossOnThePad) {
  const AnalysisConfig cfg = makeConfig(16.0, BallKind::Rubber);
  test::SimDelivery d;
  d.speed = 21.0;
  d.release = Vec3(0.05, 0.8, 1.9);
  d.bounce_y = test::kNaN;
  d.full_toss_z_at_stumps = 0.35;
  d.bounce_x = 0.0;
  d.pad_y = 16.0 - 1.0;
  d.noise_px = 0.6;
  const DeliveryResult r = runDelivery(d, cfg, 5);
  ASSERT_TRUE(r.valid) << r.reject_reason;
  EXPECT_FALSE(r.has_bounce);
  EXPECT_EQ(r.lbw.pitching.status, "full toss");
  EXPECT_EQ(r.lbw.decision, "OUT");
}

TEST(Analysis, LeftHanderMirrorsSides) {
  const AnalysisConfig cfg = makeConfig(17.0, BallKind::Tennis, Hand::Left);
  test::SimDelivery d = straightOne(17.0);
  d.bounce_x = 0.35;  // +x is the OFF side for a left-hander
  d.turn_deg = -4.0;
  const DeliveryResult r = runDelivery(d, cfg, 2);
  ASSERT_TRUE(r.valid) << r.reject_reason;
  EXPECT_EQ(r.lbw.pitching.status, "outside off");
  EXPECT_GT(r.bounce_x_off_cm.value, 25.0);
}

TEST(Analysis, RejectsNonDeliveries) {
  const AnalysisConfig cfg = makeConfig(17.0);
  std::vector<Observation> obs;
  for (int i = 0; i < 12; ++i) obs.push_back({i / 30.0, Vec2(300 + 3 * i, 200 + 0.5 * i), 3.0});
  const DeliveryResult r = analyzeObservations(obs, cfg);
  EXPECT_FALSE(r.valid);
}

// Monte-Carlo intervals should contain the truth most of the time.
TEST(Analysis, IntervalsCoverTruth) {
  std::mt19937 rng(77);
  std::uniform_real_distribution<double> U(0, 1);
  int n = 0, covered_speed = 0, covered_stumps_x = 0;
  for (int i = 0; i < 20; ++i) {
    const double L = 15.0 + 5.0 * U(rng);
    AnalysisConfig cfg = makeConfig(L);
    cfg.calib_sigma_x_m = 0.0;
    cfg.calib_sigma_z_m = 0.0;
    test::SimDelivery d = straightOne(L);
    d.speed = 18.0 + 10.0 * U(rng);
    d.bounce_x = -0.15 + 0.3 * U(rng);
    d.bounce_y = L * (0.55 + 0.25 * U(rng));
    d.lateral_accel = -1.5 + 3.0 * U(rng);
    d.turn_deg = -3.0 + 6.0 * U(rng);
    d.pad_y = test::kNaN;
    test::SimOutput s;
    const DeliveryResult r = runDelivery(d, cfg, 100 + i, &s);
    if (!r.valid || !r.stumps_x_off_cm.valid) continue;
    ++n;
    const double v = 3.6 * s.truth.release_speed;
    covered_speed += (v >= r.release_speed_kmh.lo - 0.5 && v <= r.release_speed_kmh.hi + 0.5);
    const double sx = -100.0 * s.truth.at_stumps.x();
    covered_stumps_x += (sx >= r.stumps_x_off_cm.lo - 0.5 && sx <= r.stumps_x_off_cm.hi + 0.5);
  }
  ASSERT_GE(n, 15);
  EXPECT_GE(covered_speed, static_cast<int>(0.7 * n));
  EXPECT_GE(covered_stumps_x, static_cast<int>(0.7 * n));
}

TEST(Analysis, NoBatterNeverGivesLbw) {
  // Practice at empty stumps: a deflection short of the stumps is not a pad.
  AnalysisConfig cfg = makeConfig(18.0);
  cfg.batter_present = false;
  const DeliveryResult r = runDelivery(straightOne(18.0), cfg, 31);
  ASSERT_TRUE(r.valid) << r.reject_reason;
  EXPECT_FALSE(r.lbw_applicable);
  EXPECT_NE(r.impact, ImpactKind::Pad);
  EXPECT_NE(r.impact, ImpactKind::Lost);
}
