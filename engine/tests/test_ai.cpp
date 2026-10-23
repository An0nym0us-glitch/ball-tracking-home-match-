// YOLO assist with the real model. Needs PE_AI_MODEL (yolox_nano.onnx) and
// PE_AI_IMAGE (a photo with a person and a football, e.g. OpenCV's
// samples/data/messi5.jpg); skipped otherwise. CI downloads both.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <thread>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "pocketeye/ai.hpp"

using namespace pe;

namespace {
const char* env(const char* k) { return std::getenv(k); }
}  // namespace

TEST(AiAssist, FindsPeopleAndTheBall) {
  if (!env("PE_AI_MODEL") || !env("PE_AI_IMAGE")) GTEST_SKIP() << "set PE_AI_MODEL and PE_AI_IMAGE";
  const cv::Mat img = cv::imread(env("PE_AI_IMAGE"));
  ASSERT_FALSE(img.empty());
  AiConfig c;
  c.enabled = true;
  c.model = env("PE_AI_MODEL");
  c.device = "dgpu";  // falls back to the CPU on machines without one
  AiAssist ai(c);
  const auto people = ai.detectNow(img, AiAssist::kPerson, 0.5f);
  const auto balls = ai.detectNow(img, AiAssist::kSportsBall, 0.5f);
  const AiStatus st = ai.status();
  ASSERT_TRUE(st.ready) << st.error;
  EXPECT_FALSE(st.device.empty());
  EXPECT_GE(people.size(), 1u);
  ASSERT_GE(balls.size(), 1u);
  // The football in messi5.jpg: centre about (362, 313), about 49 px across.
  const cv::Rect2f& b = balls.front().box;
  EXPECT_NEAR(b.x + b.width / 2, 362, 8);
  EXPECT_NEAR(b.y + b.height / 2, 313, 8);
}

TEST(AiAssist, BallCheckOnASmallBall) {
  if (!env("PE_AI_MODEL") || !env("PE_AI_IMAGE")) GTEST_SKIP() << "set PE_AI_MODEL and PE_AI_IMAGE";
  const cv::Mat img = cv::imread(env("PE_AI_IMAGE"));
  AiConfig c;
  c.enabled = true;
  c.model = env("PE_AI_MODEL");
  c.device = "cpu";
  AiAssist ai(c);
  // The picture shrunk so that the ball is about 10 px across (as a cricket
  // ball 15 m away at 720p), then the 104 px crop around it, as the engine sends.
  const double s = 10.0 / 49.0;
  cv::Mat small;
  cv::resize(img, small, cv::Size(), s, s, cv::INTER_AREA);
  const Vec2 ball(362 * s, 313 * s);
  cv::Mat canvas(400, 400, CV_8UC3, cv::Scalar(60, 120, 60));
  small.copyTo(canvas(cv::Rect(100, 100, small.cols, small.rows)));
  const Vec2 at = ball + Vec2(100, 100);
  const cv::Rect roi(static_cast<int>(at.x()) - 52, static_cast<int>(at.y()) - 52, 104, 104);
  for (int i = 0; i < 100 && !ai.status().ready && ai.status().error.empty(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  ASSERT_TRUE(ai.status().ready) << ai.status().error;
  ai.submitCheck({7, 1.0, Vec2(roi.x + 52, roi.y + 52)}, canvas(roi).clone());
  std::vector<AiCheckResult> r;
  for (int i = 0; i < 100 && r.empty(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    r = ai.takeChecks();
  }
  ASSERT_EQ(r.size(), 1u);
  EXPECT_EQ(r[0].id, 7);
  EXPECT_GE(r[0].score, c.ball_score);
  EXPECT_LT((r[0].uv - at).norm(), 4.0) << "ball centre in picture pixels";
}
