#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <opencv2/imgproc.hpp>

#include "pocketeye/autocal.hpp"
#include "sim.hpp"

using namespace pe;

namespace {

// Draws a rectangle in the plane y = const (facing the camera), anti-aliased.
void drawBox(cv::Mat& img, const Camera& cam, double x0, double x1, double y, double z0, double z1,
             const cv::Scalar& color) {
  std::vector<cv::Point> pts;
  for (const auto& [x, z] : {std::pair{x0, z0}, {x1, z0}, {x1, z1}, {x0, z1}}) {
    const Vec3 w(x, y, z);
    if (cam.depth(w) < 0.2) return;
    const Vec2 uv = cam.project(w);
    pts.emplace_back(static_cast<int>(std::lround(uv.x() * 16)), static_cast<int>(std::lround(uv.y() * 16)));
  }
  cv::fillConvexPoly(img, pts, color, cv::LINE_AA, 4);
}

// A pitch as the phone sees it before play: grass, a lighter strip, a crease
// line under each set of stumps, and the stumps with bails.
cv::Mat renderPitch(const Camera& cam, const Pitch& p, bool with_stumps) {
  cv::Mat img(cam.height, cam.width, CV_8UC3);
  cv::RNG rng(11);
  rng.fill(img, cv::RNG::UNIFORM, cv::Scalar(50, 105, 45), cv::Scalar(75, 135, 70));
  cv::GaussianBlur(img, img, cv::Size(3, 3), 0);
  // Sky above the horizon.
  const Vec2 far = cam.project(Vec3(0, 200.0, 0));
  img.rowRange(0, std::clamp(static_cast<int>(far.y()), 0, cam.height)).setTo(cv::Scalar(215, 200, 180));
  std::vector<cv::Point> strip;
  for (const Vec3& w : {Vec3(-1.5, 0.5, 0), Vec3(1.5, 0.5, 0), Vec3(1.5, p.length + 2, 0), Vec3(-1.5, p.length + 2, 0)}) {
    const Vec2 uv = cam.project(w);
    strip.emplace_back(static_cast<int>(uv.x()), static_cast<int>(uv.y()));
  }
  cv::fillConvexPoly(img, strip, cv::Scalar(120, 160, 175), cv::LINE_AA);
  const double xs = p.outerStumpX();
  for (double y : {0.0, p.length}) {
    if (with_stumps) {
      for (double x : {-xs, 0.0, xs}) {
        drawBox(img, cam, x - p.stump_diameter / 2, x + p.stump_diameter / 2, y, 0.0, p.stump_height,
                cv::Scalar(205, 230, 240));
      }
      drawBox(img, cam, -xs - p.stump_diameter / 2, xs + p.stump_diameter / 2, y, p.stump_height - 0.012,
              p.stump_height + 0.008, cv::Scalar(175, 205, 220));
    }
  }
  cv::Mat noise(img.size(), CV_8UC3);
  rng.fill(noise, cv::RNG::UNIFORM, cv::Scalar::all(0), cv::Scalar::all(6));
  img += noise;
  return img;
}

CalibrationInput inputFor(const Camera& cam, const Pitch& p, bool length_known) {
  CalibrationInput in;
  in.width = cam.width;
  in.height = cam.height;
  in.pitch = p;
  in.length_known = length_known;
  in.hfov_deg = 2.0 * std::atan(0.5 * cam.width / cam.f) * 180.0 / kPi;
  in.hfov_sigma_deg = 1.0;
  return in;
}

}  // namespace

TEST(AutoCalibration, FindsBothStumpSetsAndTheirPositions) {
  Pitch p;
  p.length = 17.0;
  const Camera cam = test::standardCamera(p.length);
  const cv::Mat img = renderPitch(cam, p, true);
  const AutoCalibration a = autoCalibrate(img, inputFor(cam, p, true));
  ASSERT_TRUE(a.ok) << a.message;
  EXPECT_TRUE(a.batter_found);
  EXPECT_TRUE(a.bowler_found);
  for (const TapPoint& t : a.taps) {
    const Vec3 w = *tapWorldPoint(t.name, p);
    const Vec2 truth = cam.project(w);
    // Far stumps are ~40 px tall: within 1.5 px. Near stumps are hundreds of px: within 1 %.
    const double tol = t.name.rfind("batter", 0) == 0 ? 1.5 : 0.01 * cam.height;
    EXPECT_NEAR(t.uv.x(), truth.x(), tol) << t.name;
    EXPECT_NEAR(t.uv.y(), truth.y(), tol) << t.name;
  }
}

TEST(AutoCalibration, MeasuresThePitchLength) {
  Pitch p;
  p.length = 16.0;
  const Camera cam = test::standardCamera(p.length);
  const cv::Mat img = renderPitch(cam, p, true);
  const AutoCalibration a = autoCalibrate(img, inputFor(cam, p, false));
  ASSERT_TRUE(a.ok) << a.message;
  ASSERT_TRUE(a.length_estimated);
  EXPECT_NEAR(a.calibration.pitch_length, p.length, 0.8);
}

TEST(AutoCalibration, NoStumpsNoResult) {
  Pitch p;
  p.length = 17.0;
  const Camera cam = test::standardCamera(p.length);
  const cv::Mat img = renderPitch(cam, p, false);
  const AutoCalibration a = autoCalibrate(img, inputFor(cam, p, true));
  EXPECT_FALSE(a.ok);
  EXPECT_FALSE(a.message.empty());
}

TEST(AutoCalibration, OnlyBatterStumpsUsesTheMeasurements) {
  // A pitch with stumps only at the batter's end (here the near set is drawn
  // too: it must be ignored when the user says there are none).
  Pitch p;
  p.length = 17.0;
  const Camera cam = test::standardCamera(p.length);
  const cv::Mat img = renderPitch(cam, p, true);
  CalibrationInput in = inputFor(cam, p, true);
  in.bowler_stumps = false;
  const AutoCalibration none = autoCalibrate(img, in);
  EXPECT_TRUE(none.batter_found);
  EXPECT_FALSE(none.bowler_found);
  EXPECT_FALSE(none.ok) << "measurements missing";
  in.camera_height_m = cam.center().z();
  in.camera_distance_m = -cam.center().y();
  const AutoCalibration a = autoCalibrate(img, in);
  ASSERT_TRUE(a.ok) << a.message;
  EXPECT_FALSE(a.bowler_found);
  for (const TapPoint& t : a.taps) EXPECT_EQ(t.name.rfind("batter", 0), 0u) << t.name;
}
