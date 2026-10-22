#include <algorithm>
#include <chrono>
#include <opencv2/imgproc.hpp>
#include <random>

#include "pocketeye/config.hpp"
#include "pocketeye/video.hpp"

namespace pe {

using nlohmann::json;

// Renders a simple delivery (textured ground, a ball flying away from the
// camera) and times the per-frame engine work. The numbers only rank devices;
// they are not an accuracy test.
json runBenchmark(int width, int height, int frames) {
  const double L = 18.0;
  const double f = 0.5 * width / std::tan(0.5 * 65.0 * kPi / 180.0);
  EngineConfig cfg;
  cfg.analysis.camera = Camera::lookAt(width, height, f, Vec3(0.0, -1.5, 1.25), Vec3(0.0, 0.62 * L, 0.25));
  cfg.analysis.pitch.length = L;
  cfg.analysis.ball = ballSpec(BallKind::Tennis);
  cfg.analysis.mc_samples = 100;
  Engine engine(cfg);

  std::mt19937 rng(1);
  cv::Mat background(height, width, CV_8UC3);
  cv::randu(background, cv::Scalar(90, 100, 90), cv::Scalar(140, 150, 140));
  cv::GaussianBlur(background, background, cv::Size(5, 5), 0);
  std::normal_distribution<float> noise(0.f, 2.f);
  cv::Mat frame, n16(height, width, CV_16SC3);

  const double fps = 30.0;
  const double t_release = 0.3;
  const Camera& cam = cfg.analysis.camera;
  std::vector<double> per_frame_ms;
  YuvFrame yuv;
  const auto t_start = std::chrono::steady_clock::now();
  for (int i = 0; i < frames; ++i) {
    const double t = i / fps;
    background.copyTo(frame);
    const double tau = std::fmod(t, 1.6) - t_release;
    if (tau > 0.0 && tau < 0.9) {
      const Vec3 p(0.05, 0.8 + 22.0 * tau, std::max(0.04, 2.0 - 2.0 * tau - 0.5 * kGravity * tau * tau * 0.3));
      if (cam.depth(p) > 0.5) {
        const Vec2 uv = cam.project(p);
        cv::circle(frame, cv::Point(static_cast<int>(uv.x()), static_cast<int>(uv.y())),
                   std::max(1, static_cast<int>(cam.apparentRadius(p, 0.0335))), cv::Scalar(60, 225, 215), cv::FILLED,
                   cv::LINE_AA);
      }
    }
    cv::randn(n16, 0, 2);
    frame.convertTo(frame, CV_16SC3);
    frame += n16;
    frame.convertTo(frame, CV_8UC3);
    // Phones deliver YUV: time the engine on that, as in phone-only mode.
    toYuvFrame(frame, yuv);
    const auto t0 = std::chrono::steady_clock::now();
    engine.pushFrame(yuv, t);
    per_frame_ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    for (DeliveryResult r; engine.popResult(r);) {
    }
  }
  const auto t0 = std::chrono::steady_clock::now();
  engine.finish();
  const double finish_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_start).count();
  // Ball analysis runs on a background thread, so what limits the frame rate is
  // the steady per-frame work. Use a high percentile: a frame that takes too
  // long is a dropped frame.
  std::vector<double> sorted = per_frame_ms;
  std::sort(sorted.begin(), sorted.end());
  const double median = sorted.empty() ? 0.0 : sorted[sorted.size() / 2];
  const double p90 = sorted.empty() ? 0.0 : sorted[sorted.size() * 9 / 10];
  double mean = 0;
  for (double v : per_frame_ms) mean += v;
  mean /= std::max<size_t>(1, per_frame_ms.size());
  // 60 fps leaves 16.7 ms per frame; keep room for the camera, copying and the UI.
  std::string suggestion = p90 < 12.0 ? "phone_only_60fps" : p90 < 25.0 ? "phone_only_30fps" : "laptop";
  const double per_frame = mean;
  return {{"width", width},          {"height", height},
          {"frames", frames},        {"engine_ms_per_frame", per_frame},
          {"median_ms", median},     {"p90_ms", p90},
          {"finish_ms", finish_ms},  {"wall_ms", wall},
          {"suggestion", suggestion},
          {"detect_ms_per_frame", engine.stats().detect_ms / std::max(1, frames)},
          {"track_ms_per_frame", engine.stats().track_ms / std::max(1, frames)},
          {"analyze_ms_total", engine.stats().analyze_ms},
          {"tracks_analyzed", engine.stats().tracks_analyzed}};
}

}  // namespace pe
