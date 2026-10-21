#include "pocketeye/video.hpp"

#include <chrono>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <stdexcept>

#include "pocketeye/overlay.hpp"
#include "pocketeye/result_json.hpp"

namespace pe {

using nlohmann::json;

namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
}  // namespace

json analyzeVideoFile(const LoadedConfig& cfg, const VideoAnalysisOptions& opt,
                      std::vector<DeliveryResult>* results_out) {
  cv::VideoCapture cap;
  cap.open(opt.video_path, cv::CAP_ANY, {cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY});
  if (!cap.isOpened()) cap.open(opt.video_path);
  if (!cap.isOpened()) throw std::runtime_error("cannot open video " + opt.video_path);
  const double file_fps = cap.get(cv::CAP_PROP_FPS);
  const double frame_count = cap.get(cv::CAP_PROP_FRAME_COUNT);

  Engine engine(cfg.engine);
  std::vector<DeliveryResult> results;
  std::vector<double> timestamps;
  cv::Mat frame, image;
  double decode_ms = 0, convert_ms = 0, engine_ms = 0;
  const auto t_all = Clock::now();
  const Camera& cam = cfg.engine.analysis.camera;
  for (int i = 0;; ++i) {
    auto t0 = Clock::now();
    if (!cap.read(frame)) break;
    decode_ms += msSince(t0);
    double t;
    if (i < static_cast<int>(opt.timestamps.size())) {
      t = opt.timestamps[i];
    } else if (opt.fps_override > 0) {
      t = i / opt.fps_override;
    } else {
      t = cap.get(cv::CAP_PROP_POS_MSEC) / 1000.0;
      if (i > 0 && t <= timestamps.back()) t = i / (file_fps > 0 ? file_fps : 30.0);
    }
    timestamps.push_back(t);
    if (frame.cols != cam.width || frame.rows != cam.height) {
      throw std::runtime_error("video is " + std::to_string(frame.cols) + "x" + std::to_string(frame.rows) +
                               " but the calibration is for " + std::to_string(cam.width) + "x" +
                               std::to_string(cam.height));
    }
    t0 = Clock::now();
    if (opt.gray && frame.channels() != 1) {
      cv::cvtColor(frame, image, frame.channels() == 4 ? cv::COLOR_BGRA2GRAY : cv::COLOR_BGR2GRAY);
    } else if (frame.channels() == 4) {
      cv::cvtColor(frame, image, cv::COLOR_BGRA2BGR);
    } else {
      image = frame;
    }
    convert_ms += msSince(t0);
    t0 = Clock::now();
    engine.pushFrame(image, t);
    engine_ms += msSince(t0);
    if (opt.on_frame) opt.on_frame(i, t, engine);
    for (DeliveryResult r; engine.popResult(r);) results.push_back(std::move(r));
    if (opt.progress_permille && frame_count > 0) {
      opt.progress_permille->store(static_cast<int>(std::min(999.0, 1000.0 * (i + 1) / frame_count)));
    }
  }
  auto t0 = Clock::now();
  engine.finish();
  engine_ms += msSince(t0);
  for (DeliveryResult r; engine.popResult(r);) results.push_back(std::move(r));
  const double total_ms = msSince(t_all);

  json out;
  out["version"] = "pocketeye 0.1.0";
  out["video"] = {{"path", opt.video_path}, {"frames", timestamps.size()}, {"fps_reported", file_fps}};
  out["calibration"] = calibrationToJson(cfg.calibration);
  out["ball"] = cfg.engine.analysis.ball.name;
  out["batter"] = cfg.engine.analysis.batter == Hand::Right ? "right" : "left";
  json dl = json::array();
  for (const auto& r : results) dl.push_back(deliveryToJson(r, cfg.engine.analysis));
  out["deliveries"] = dl;
  const auto& st = engine.stats();
  const double n = std::max<size_t>(1, timestamps.size());
  out["timings_ms"] = {{"decode_total", decode_ms},       {"decode_per_frame", decode_ms / n},
                       {"convert_total", convert_ms},     {"detect_total", st.detect_ms},
                       {"detect_per_frame", st.detect_ms / n}, {"track_total", st.track_ms},
                       {"analyze_total", st.analyze_ms},  {"engine_total", engine_ms},
                       {"wall_total", total_ms}};
  out["tracks_analyzed"] = st.tracks_analyzed;

  if (!opt.overlay_path.empty()) {
    std::string err;
    if (!writeOverlayVideo(opt.video_path, timestamps, results, cfg.engine.analysis, opt.overlay_path, &err)) {
      out["overlay_error"] = err;
    }
  }
  if (opt.progress_permille) opt.progress_permille->store(1000);
  if (results_out) *results_out = std::move(results);
  return out;
}

}  // namespace pe
