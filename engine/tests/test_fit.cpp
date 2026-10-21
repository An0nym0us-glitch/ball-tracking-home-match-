#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>

#include "pocketeye/fit.hpp"
#include "sim.hpp"

using namespace pe;

namespace {

struct Split {
  std::vector<Observation> pre, post;
};

Split splitAtBounce(const test::SimOutput& s, double t_end) {
  Split out;
  for (const Observation& o : s.obs) {
    if (o.t > t_end) break;
    (o.t < s.truth.t_bounce ? out.pre : out.post).push_back(o);
  }
  return out;
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

}  // namespace

// Noise-free observations: the fit must recover the simulated delivery almost exactly.
TEST(Fit, RecoversNoiseFreeBounce) {
  std::mt19937 rng(11);
  Pitch pitch;
  pitch.length = 18.0;
  const BallSpec ball = ballSpec(BallKind::Tennis);
  const Camera cam = test::standardCamera(pitch.length);
  test::SimDelivery d;
  d.speed = 24.0;
  d.bounce_x = 0.05;
  d.bounce_y = 12.0;
  d.lateral_accel = 1.5;
  d.turn_deg = 3.0;
  d.noise_px = 0.0;
  const test::SimOutput s = test::simulate(d, cam, pitch, ball, rng);
  ASSERT_TRUE(s.truth.has_bounce);
  const Split sp = splitAtBounce(s, 1e9);
  ASSERT_GE(sp.post.size(), 3u);

  FitOptions opt;
  opt.ball = ball;
  opt.sigma_px = 0.3;
  opt.pitch_length = pitch.length;
  const FitResult f = fitTrajectory(cam, sp.pre, sp.post, opt);
  ASSERT_TRUE(f.ok) << f.error;
  EXPECT_LT(f.rms_px, 0.3);
  EXPECT_NEAR(f.theta[theta::YB], s.truth.bounce.y(), 0.05);
  EXPECT_NEAR(f.theta[theta::XB], s.truth.bounce.x(), 0.01);
  const double turn = (std::atan2(f.theta[theta::VOX], f.theta[theta::VOY]) -
                       std::atan2(f.theta[theta::VIX], f.theta[theta::VIY])) * 180.0 / kPi;
  EXPECT_NEAR(turn, s.truth.turn_deg, 0.3);
}

// Realistic noise at 30 fps over many deliveries: check the median errors of
// the quantities LBW depends on.
TEST(Fit, AccuracyOverRandomDeliveries30fps) {
  std::mt19937 rng(21);
  std::uniform_real_distribution<double> U(0, 1);
  const BallSpec ball = ballSpec(BallKind::Tennis);
  std::vector<double> e_by, e_bx, e_sx, e_sz, e_turn;
  for (int i = 0; i < 25; ++i) {
    Pitch pitch;
    pitch.length = 14.0 + 6.0 * U(rng);
    const Camera cam = test::standardCamera(pitch.length);
    test::SimDelivery d;
    d.speed = 17.0 + 13.0 * U(rng);
    d.release = Vec3(-0.3 + 0.6 * U(rng), 0.6 + 0.4 * U(rng), 1.7 + 0.4 * U(rng));
    d.bounce_y = pitch.length * (0.5 + 0.35 * U(rng));
    d.bounce_x = -0.25 + 0.5 * U(rng);
    d.lateral_accel = -2.0 + 4.0 * U(rng);
    d.turn_deg = -5.0 + 10.0 * U(rng);
    d.noise_px = 0.7;
    const test::SimOutput s = test::simulate(d, cam, pitch, ball, rng);
    if (!s.truth.has_bounce || !s.truth.at_stumps.allFinite()) continue;
    const Split sp = splitAtBounce(s, 1e9);
    if (sp.post.size() < 3 || sp.pre.size() < 5) continue;
    FitOptions opt;
    opt.ball = ball;
    opt.sigma_px = 0.7;
    opt.pitch_length = pitch.length;
    const FitResult f = fitTrajectory(cam, sp.pre, sp.post, opt);
    ASSERT_TRUE(f.ok) << f.error;
    ModelSpec spec = f.spec;
    spec.t_end += 1.0;
    const Trajectory tr = buildTrajectory(spec, f.theta);
    const auto ts = tr.timeAtY(pitch.length, f.spec.t_begin);
    ASSERT_TRUE(ts.has_value());
    const Vec3 st = tr.position(*ts);
    e_by.push_back(std::abs(f.theta[theta::YB] - s.truth.bounce.y()));
    e_bx.push_back(std::abs(f.theta[theta::XB] - s.truth.bounce.x()));
    e_sx.push_back(std::abs(st.x() - s.truth.at_stumps.x()));
    e_sz.push_back(std::abs(st.z() - s.truth.at_stumps.z()));
    const double turn = (std::atan2(f.theta[theta::VOX], f.theta[theta::VOY]) -
                         std::atan2(f.theta[theta::VIX], f.theta[theta::VIY])) * 180.0 / kPi;
    e_turn.push_back(std::abs(turn - s.truth.turn_deg));
  }
  ASSERT_GE(e_by.size(), 15u);
  EXPECT_LT(median(e_by), 0.25);   // length (m)
  EXPECT_LT(median(e_bx), 0.03);   // line at bounce (m)
  EXPECT_LT(median(e_sx), 0.03);   // line at stumps (m)
  EXPECT_LT(median(e_sz), 0.06);   // height at stumps (m)
  EXPECT_LT(median(e_turn), 1.5);  // degrees
  RecordProperty("median_bounce_y_err_m", std::to_string(median(e_by)));
  RecordProperty("median_stumps_x_err_m", std::to_string(median(e_sx)));
}

TEST(Fit, FullToss) {
  std::mt19937 rng(4);
  Pitch pitch;
  pitch.length = 16.0;
  const BallSpec ball = ballSpec(BallKind::Rubber);
  const Camera cam = test::standardCamera(pitch.length);
  test::SimDelivery d;
  d.speed = 22.0;
  d.bounce_y = test::kNaN;
  d.full_toss_z_at_stumps = 0.35;
  d.bounce_x = 0.02;
  d.noise_px = 0.5;
  const test::SimOutput s = test::simulate(d, cam, pitch, ball, rng);
  ASSERT_FALSE(s.truth.has_bounce);
  FitOptions opt;
  opt.ball = ball;
  opt.sigma_px = 0.5;
  opt.pitch_length = pitch.length;
  const FitResult f = fitTrajectory(cam, s.obs, {}, opt);
  ASSERT_TRUE(f.ok) << f.error;
  ModelSpec spec = f.spec;
  spec.t_end += 0.5;
  const Trajectory tr = buildTrajectory(spec, f.theta);
  const auto ts = tr.timeAtY(pitch.length, f.spec.t_begin);
  ASSERT_TRUE(ts.has_value());
  EXPECT_NEAR(tr.position(*ts).z(), s.truth.at_stumps.z(), 0.05);
  EXPECT_NEAR(tr.position(*ts).x(), s.truth.at_stumps.x(), 0.03);
}
