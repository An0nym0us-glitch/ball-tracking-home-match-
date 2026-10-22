#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <opencv2/imgproc.hpp>
#include <random>

#include "pocketeye/engine.hpp"
#include "sim.hpp"

using namespace pe;

namespace {

// Renders a simulated delivery (yellow ball on a textured green background)
// and feeds every frame to an engine, as BGR or as camera YUV planes.
enum class Input { Bgr, YuvI420 };

std::vector<DeliveryResult> runEngine(bool async, Input input, test::SimTruth* truth = nullptr, int balls = 1) {
  const double L = 18.0, fps = 30.0;
  EngineConfig cfg;
  cfg.analysis.pitch.length = L;
  cfg.analysis.camera = test::standardCamera(L);
  cfg.analysis.ball = ballSpec(BallKind::Tennis);
  cfg.analysis.mc_samples = 100;
  cfg.async_analysis = async;
  const Camera& cam = cfg.analysis.camera;

  test::SimDelivery d;
  d.speed = 24.0;
  d.release = Vec3(0.1, 0.8, 1.9);
  d.bounce_y = 0.75 * L;
  d.pad_y = L - 1.0;
  d.noise_px = 0.0;
  d.fps = fps;
  std::mt19937 rng(3);
  const test::SimOutput sim = test::simulate(d, cam, cfg.analysis.pitch, cfg.analysis.ball, rng);
  if (truth) *truth = sim.truth;
  // The same delivery bowled `balls` times, 6 s apart, in one stream.
  const double gap_s = 6.0;
  std::map<long, Observation> at;
  for (int b = 0; b < balls; ++b) {
    for (const Observation& o : sim.obs) at[std::lround((o.t + b * gap_s) * fps)] = o;
  }

  cv::Mat background(cam.height, cam.width, CV_8UC3);
  cv::RNG cv_rng(7);
  cv_rng.fill(background, cv::RNG::UNIFORM, cv::Scalar(60, 110, 50), cv::Scalar(100, 160, 90));
  cv::GaussianBlur(background, background, cv::Size(5, 5), 0);

  Engine engine(cfg);
  std::vector<DeliveryResult> out;
  cv::Mat frame, noise(cam.height, cam.width, CV_8UC3), i420;
  YuvFrame yuv;
  for (long i = 0; i < static_cast<long>((3.0 + (balls - 1) * gap_s) * fps); ++i) {
    background.copyTo(frame);
    cv_rng.fill(noise, cv::RNG::UNIFORM, cv::Scalar::all(0), cv::Scalar::all(4));
    frame += noise;
    if (const auto it = at.find(i); it != at.end()) {
      const Observation& o = it->second;
      cv::circle(frame, cv::Point2d(o.uv.x(), o.uv.y()), std::max(1.0, o.radius_px), cv::Scalar(60, 225, 215),
                 cv::FILLED, cv::LINE_AA);
    }
    const double t = i / fps;
    if (input == Input::Bgr) {
      engine.pushFrame(frame, t);
    } else {
      // What Android cameras deliver: Y plane + quarter-size U and V planes.
      cv::cvtColor(frame, i420, cv::COLOR_BGR2YUV_I420);
      const int w = cam.width, h = cam.height;
      yuv.y = i420.rowRange(0, h);
      const cv::Mat u(h / 2, w / 2, CV_8UC1, i420.ptr(h));
      const cv::Mat v(h / 2, w / 2, CV_8UC1, i420.ptr(h) + (w / 2) * (h / 2));
      cv::merge(std::vector<cv::Mat>{u, v}, yuv.uv);
      engine.pushFrame(yuv, t);
    }
    for (DeliveryResult r; engine.popResult(r);) out.push_back(std::move(r));
  }
  engine.finish();
  for (DeliveryResult r; engine.popResult(r);) out.push_back(std::move(r));
  return out;
}

}  // namespace

TEST(Engine, BackgroundAnalysisGivesTheSameResult) {
  test::SimTruth truth;
  const auto sync = runEngine(false, Input::Bgr, &truth);
  const auto async = runEngine(true, Input::Bgr);
  ASSERT_EQ(sync.size(), 1u);
  ASSERT_EQ(async.size(), 1u);
  EXPECT_DOUBLE_EQ(sync[0].release_speed_kmh.value, async[0].release_speed_kmh.value);
  EXPECT_EQ(sync[0].lbw.decision, async[0].lbw.decision);
  EXPECT_NEAR(sync[0].release_speed_kmh.value, truth.release_speed * 3.6, 0.1 * truth.release_speed * 3.6);
}

TEST(Engine, CameraYuvInputMatchesBgr) {
  const auto bgr = runEngine(true, Input::Bgr);
  const auto yuv = runEngine(true, Input::YuvI420);
  ASSERT_EQ(bgr.size(), 1u);
  ASSERT_EQ(yuv.size(), 1u);
  EXPECT_NEAR(yuv[0].release_speed_kmh.value, bgr[0].release_speed_kmh.value, 0.05 * bgr[0].release_speed_kmh.value);
  // Colour arrives at half resolution, so a borderline ball may move between a
  // definite call and umpire's call, but the two must never contradict.
  const auto definite = [](const std::string& d) { return d == "OUT" || d == "NOT OUT"; };
  if (definite(yuv[0].lbw.decision) && definite(bgr[0].lbw.decision)) {
    EXPECT_EQ(yuv[0].lbw.decision, bgr[0].lbw.decision);
  }
}

TEST(Engine, FindsEveryBallInALongSession) {
  const auto out = runEngine(true, Input::YuvI420, nullptr, 6);
  ASSERT_EQ(out.size(), 6u);
  for (const auto& r : out) EXPECT_NEAR(r.release_speed_kmh.value, out[0].release_speed_kmh.value, 1.0);
}
