#include <gtest/gtest.h>

#include <random>

#include "pocketeye/calibration.hpp"
#include "sim.hpp"

using namespace pe;

namespace {

CalibrationInput makeTaps(const Camera& cam, const Pitch& pitch, double noise, std::mt19937& rng,
                          bool corners = false) {
  std::normal_distribution<double> nd(0, noise);
  CalibrationInput in;
  in.width = cam.width;
  in.height = cam.height;
  in.pitch = pitch;
  std::vector<std::string> names = {"batter_left_base", "batter_left_top", "batter_right_base",
                                    "batter_right_top", "bowler_left_base", "bowler_left_top",
                                    "bowler_right_base", "bowler_right_top"};
  if (corners) {
    for (const char* c : {"corner_bowler_left", "corner_bowler_right", "corner_batter_left", "corner_batter_right"}) {
      names.push_back(c);
    }
  }
  for (const auto& n : names) {
    const Vec3 w = *tapWorldPoint(n, pitch);
    if (cam.depth(w) < 0.2) continue;
    const Vec2 uv = cam.project(w);
    if (uv.x() < 0 || uv.y() < 0 || uv.x() >= cam.width || uv.y() >= cam.height) continue;  // out of frame
    in.taps.push_back({n, uv + Vec2(nd(rng), nd(rng))});
  }
  return in;
}

}  // namespace

TEST(Calibration, StumpTapsRecoverCameraWithKnownLength) {
  std::mt19937 rng(3);
  Pitch pitch;
  pitch.length = 17.0;
  pitch.stump_height = 0.66;
  pitch.stump_width = 0.21;
  const Camera truth = test::standardCamera(pitch.length);
  CalibrationInput in = makeTaps(truth, pitch, 0.7, rng);
  ASSERT_GE(in.taps.size(), 6u);
  const CalibrationResult r = calibrateFromTaps(in);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_LT((r.camera.center() - truth.center()).norm(), 0.15);
  EXPECT_NEAR(r.camera.f / truth.f, 1.0, 0.06);
  EXPECT_LT(r.rms_px, 1.5);
  // The stumps themselves must reproject where they were tapped.
  const Vec2 e = r.camera.project(Vec3(0, pitch.length, 0.3)) - truth.project(Vec3(0, pitch.length, 0.3));
  EXPECT_LT(e.norm(), 1.5);
}

TEST(Calibration, EstimatesPitchLengthFromStumps) {
  std::mt19937 rng(5);
  Pitch pitch;
  pitch.length = 15.5;
  const Camera truth = test::standardCamera(pitch.length);
  CalibrationInput in = makeTaps(truth, pitch, 0.5, rng);
  in.length_known = false;
  in.pitch.length = 18.0;  // only a guess
  const CalibrationResult r = calibrateFromTaps(in);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_NEAR(r.pitch_length, 15.5, std::max(0.6, 2.5 * r.pitch_length_sigma));
  EXPECT_GT(r.pitch_length_sigma, 0.0);
}

TEST(Calibration, RequiresBatterEndStumps) {
  CalibrationInput in;
  in.width = 1280;
  in.height = 720;
  in.taps = {{"bowler_left_top", Vec2(500, 600)}, {"bowler_right_top", Vec2(700, 600)}};
  const CalibrationResult r = calibrateFromTaps(in);
  EXPECT_FALSE(r.ok);
  EXPECT_NE(r.error.find("batter_left_base"), std::string::npos);
}

TEST(Calibration, CornersAndStumpsTogether) {
  std::mt19937 rng(9);
  Pitch pitch;
  pitch.length = 20.12;
  pitch.width = 3.05;
  const Camera truth = test::standardCamera(pitch.length);
  CalibrationInput in = makeTaps(truth, pitch, 0.7, rng, true);
  const CalibrationResult r = calibrateFromTaps(in);
  ASSERT_TRUE(r.ok) << r.error;
  EXPECT_LT((r.camera.center() - truth.center()).norm(), 0.15);
  EXPECT_NEAR(r.camera.f / truth.f, 1.0, 0.05);
}
