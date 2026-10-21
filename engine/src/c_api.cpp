#include "pocketeye/pocketeye_c.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <thread>

#include "pocketeye/config.hpp"
#include "pocketeye/engine.hpp"
#include "pocketeye/result_json.hpp"
#include "pocketeye/video.hpp"

#ifdef PE_WITH_VIDEOIO
#include <opencv2/videoio.hpp>
#endif

struct pe_engine {
  pe::LoadedConfig config;
  std::unique_ptr<pe::Engine> engine;
  std::deque<std::string> pending;
  cv::Mat scratch;
  cv::Mat last;  // last pushed frame (header only; data owned by the caller or scratch)
  std::vector<uchar> jpeg;
};

struct pe_video {
#ifdef PE_WITH_VIDEOIO
  cv::VideoCapture cap;
#endif
  std::vector<uchar> jpeg;  // last encoded frame, kept if the caller's buffer was too small
  double t = 0.0;
  bool pending = false;
  double fps = 30.0;
  int index = 0;
};

namespace {

using nlohmann::json;

int32_t copyOut(const std::string& s, char* buf, int32_t len) {
  if (buf && len > 0) {
    const size_t n = std::min<size_t>(s.size(), static_cast<size_t>(len - 1));
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
  }
  return static_cast<int32_t>(s.size());
}

void drain(pe_engine* e) {
  pe::DeliveryResult r;
  while (e->engine->popResult(r)) {
    e->pending.push_back(pe::deliveryToJson(r, e->config.engine.analysis).dump());
  }
}

int32_t push(pe_engine* e, const cv::Mat& image, double t) {
  const pe::Camera& cam = e->config.engine.analysis.camera;
  if (image.cols != cam.width || image.rows != cam.height) return -2;
  e->engine->pushFrame(image, t);
  image.copyTo(e->last);
  drain(e);
  return 0;
}

// Android YUV_420_888 planes -> BGR.
void yuv420ToBgr(const uint8_t* y, const uint8_t* u, const uint8_t* v, int width, int height, int y_stride,
                 int uv_stride, int uv_pixel_stride, cv::Mat& out) {
  cv::Mat yuv(height * 3 / 2, width, CV_8UC1);
  for (int r = 0; r < height; ++r) std::memcpy(yuv.ptr(r), y + static_cast<size_t>(r) * y_stride, width);
  const int cw = width / 2, ch = height / 2;
  uint8_t* du = yuv.ptr(height);
  uint8_t* dv = du + cw * ch;
  for (int r = 0; r < ch; ++r) {
    const uint8_t* ur = u + static_cast<size_t>(r) * uv_stride;
    const uint8_t* vr = v + static_cast<size_t>(r) * uv_stride;
    for (int c = 0; c < cw; ++c) {
      du[r * cw + c] = ur[static_cast<size_t>(c) * uv_pixel_stride];
      dv[r * cw + c] = vr[static_cast<size_t>(c) * uv_pixel_stride];
    }
  }
  cv::cvtColor(yuv, out, cv::COLOR_YUV2BGR_I420);
}

int32_t encodeJpeg(const cv::Mat& img, int max_width, int quality, std::vector<uchar>& tmp, uint8_t* buf,
                   int32_t buf_len) {
  cv::Mat m = img;
  if (max_width > 0 && img.cols > max_width) {
    cv::resize(img, m, cv::Size(max_width, img.rows * max_width / img.cols), 0, 0, cv::INTER_AREA);
  }
  cv::imencode(".jpg", m, tmp, {cv::IMWRITE_JPEG_QUALITY, std::clamp(quality, 30, 100)});
  const int32_t n = static_cast<int32_t>(tmp.size());
  if (buf_len < n) return -n;
  std::memcpy(buf, tmp.data(), tmp.size());
  return n;
}

// Points to draw on the calibration screen so the user can check the fit.
json calibrationOverlay(const pe::Camera& cam, const pe::Pitch& p) {
  json lines = json::array();
  auto add = [&](const pe::Vec3& a, const pe::Vec3& b) {
    if (cam.depth(a) < 0.2 || cam.depth(b) < 0.2) return;
    const pe::Vec2 ua = cam.project(a), ub = cam.project(b);
    lines.push_back({ua.x(), ua.y(), ub.x(), ub.y()});
  };
  const double xs = p.outerStumpX();
  for (double y : {0.0, p.length}) {
    for (double x : {-xs, 0.0, xs}) add({x, y, 0.0}, {x, y, p.stump_height});
    add({-xs, y, p.stump_height}, {xs, y, p.stump_height});
  }
  // Stump-width corridor along the pitch (the LBW "in line" zone).
  const double w = 0.5 * p.stump_width;
  for (double x : {-w, w}) add({x, 0.0, 0.0}, {x, p.length, 0.0});
  return lines;
}

}  // namespace

extern "C" {

const char* pe_version(void) { return "pocketeye 0.1.0"; }

int32_t pe_calibrate(const char* config_json, char* buf, int32_t buf_len) {
  json out;
  try {
    const pe::LoadedConfig cfg = pe::loadConfig(json::parse(config_json));
    out = pe::calibrationToJson(cfg.calibration);
    pe::Pitch pitch = cfg.engine.analysis.pitch;
    out["overlay"] = calibrationOverlay(cfg.calibration.camera, pitch);
  } catch (const std::exception& ex) {
    out = {{"ok", false}, {"error", ex.what()}};
  }
  return copyOut(out.dump(), buf, buf_len);
}

pe_engine* pe_create(const char* config_json, char* err, int32_t err_len) {
  try {
    auto e = std::make_unique<pe_engine>();
    e->config = pe::loadConfig(json::parse(config_json));
    e->engine = std::make_unique<pe::Engine>(e->config.engine);
    return e.release();
  } catch (const std::exception& ex) {
    copyOut(ex.what(), err, err_len);
    return nullptr;
  }
}

void pe_destroy(pe_engine* e) { delete e; }

int32_t pe_calibration_json(pe_engine* e, char* buf, int32_t buf_len) {
  if (!e) return 0;
  return copyOut(pe::calibrationToJson(e->config.calibration).dump(), buf, buf_len);
}

int32_t pe_push_luma(pe_engine* e, const uint8_t* data, int32_t width, int32_t height, int32_t stride,
                     double t_seconds) {
  if (!e || !data) return -1;
  try {
    const cv::Mat m(height, width, CV_8UC1, const_cast<uint8_t*>(data), static_cast<size_t>(stride));
    return push(e, m, t_seconds);
  } catch (...) {
    return -3;
  }
}

int32_t pe_push_bgr(pe_engine* e, const uint8_t* data, int32_t width, int32_t height, int32_t stride,
                    double t_seconds) {
  if (!e || !data) return -1;
  try {
    const cv::Mat m(height, width, CV_8UC3, const_cast<uint8_t*>(data), static_cast<size_t>(stride));
    return push(e, m, t_seconds);
  } catch (...) {
    return -3;
  }
}

int32_t pe_push_jpeg(pe_engine* e, const uint8_t* data, int32_t len, double t_seconds) {
  if (!e || !data || len <= 0) return -1;
  try {
    const cv::Mat raw(1, len, CV_8UC1, const_cast<uint8_t*>(data));
    cv::imdecode(raw, cv::IMREAD_COLOR, &e->scratch);
    if (e->scratch.empty()) return -4;
    return push(e, e->scratch, t_seconds);
  } catch (...) {
    return -3;
  }
}

int32_t pe_push_yuv420(pe_engine* e, const uint8_t* y, const uint8_t* u, const uint8_t* v, int32_t width,
                       int32_t height, int32_t y_stride, int32_t uv_stride, int32_t uv_pixel_stride,
                       double t_seconds) {
  if (!e || !y || !u || !v) return -1;
  try {
    yuv420ToBgr(y, u, v, width, height, y_stride, uv_stride, uv_pixel_stride, e->scratch);
    return push(e, e->scratch, t_seconds);
  } catch (...) {
    return -3;
  }
}

int32_t pe_last_frame_jpeg(pe_engine* e, int32_t max_width, int32_t quality, uint8_t* buf, int32_t buf_len) {
  if (!e || e->last.empty()) return 0;
  try {
    return encodeJpeg(e->last, max_width, quality, e->jpeg, buf, buf_len);
  } catch (...) {
    return 0;
  }
}

int32_t pe_yuv420_to_jpeg(const uint8_t* y, const uint8_t* u, const uint8_t* v, int32_t width, int32_t height,
                          int32_t y_stride, int32_t uv_stride, int32_t uv_pixel_stride, int32_t quality,
                          uint8_t* buf, int32_t buf_len) {
  if (!y || !u || !v) return 0;
  try {
    cv::Mat bgr;
    yuv420ToBgr(y, u, v, width, height, y_stride, uv_stride, uv_pixel_stride, bgr);
    std::vector<uchar> tmp;
    return encodeJpeg(bgr, 0, quality, tmp, buf, buf_len);
  } catch (...) {
    return 0;
  }
}

void pe_finish(pe_engine* e) {
  if (!e) return;
  e->engine->finish();
  drain(e);
}

int32_t pe_poll_result(pe_engine* e, char* buf, int32_t buf_len) {
  if (!e || e->pending.empty()) return 0;
  const std::string& s = e->pending.front();
  const int32_t n = static_cast<int32_t>(s.size());
  if (buf_len <= n) return n;
  copyOut(s, buf, buf_len);
  e->pending.pop_front();
  return n;
}

int32_t pe_stats_json(pe_engine* e, char* buf, int32_t buf_len) {
  if (!e) return 0;
  const auto& st = e->engine->stats();
  const double n = std::max(1, st.frames);
  const json j = {{"frames", st.frames},
                  {"detect_ms_per_frame", st.detect_ms / n},
                  {"track_ms_per_frame", st.track_ms / n},
                  {"analyze_ms_total", st.analyze_ms},
                  {"tracks_analyzed", st.tracks_analyzed},
                  {"deliveries", st.deliveries}};
  return copyOut(j.dump(), buf, buf_len);
}

int32_t pe_benchmark(int32_t width, int32_t height, int32_t frames, char* buf, int32_t buf_len) {
  try {
    return copyOut(pe::runBenchmark(width, height, frames).dump(), buf, buf_len);
  } catch (const std::exception& ex) {
    return copyOut(json{{"error", ex.what()}}.dump(), buf, buf_len);
  }
}

int32_t pe_analyze_video(const char* config_json, const char* video_path, const char* out_json_path,
                         const char* overlay_path, int32_t* progress, char* err, int32_t err_len) {
#ifdef PE_WITH_VIDEOIO
  try {
    const pe::LoadedConfig cfg = pe::loadConfig(json::parse(config_json));
    pe::VideoAnalysisOptions opt;
    opt.video_path = video_path;
    if (overlay_path) opt.overlay_path = overlay_path;
    std::atomic<int> prog{0};
    opt.progress_permille = &prog;
    std::atomic<bool> done{false};
    // Mirror the progress into the caller's plain int (read by another isolate).
    std::thread mirror([&] {
      while (!done.load()) {
        if (progress) *reinterpret_cast<volatile int32_t*>(progress) = prog.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    });
    json out;
    try {
      out = pe::analyzeVideoFile(cfg, opt);
    } catch (...) {
      done = true;
      mirror.join();
      throw;
    }
    done = true;
    mirror.join();
    if (progress) *reinterpret_cast<volatile int32_t*>(progress) = 1000;
    std::ofstream f(out_json_path, std::ios::binary);
    if (!f) throw std::runtime_error(std::string("cannot write ") + out_json_path);
    f << out.dump(1);
    return 0;
  } catch (const std::exception& ex) {
    copyOut(ex.what(), err, err_len);
    return -1;
  }
#else
  copyOut("video files are not supported in this build", err, err_len);
  return -1;
#endif
}

pe_video* pe_video_open(const char* path) {
#ifdef PE_WITH_VIDEOIO
  auto v = std::make_unique<pe_video>();
  v->cap.open(path);
  if (!v->cap.isOpened()) return nullptr;
  const double fps = v->cap.get(cv::CAP_PROP_FPS);
  if (fps > 1 && fps < 1000) v->fps = fps;
  return v.release();
#else
  return nullptr;
#endif
}

int32_t pe_video_info(pe_video* v, char* buf, int32_t buf_len) {
#ifdef PE_WITH_VIDEOIO
  if (!v) return 0;
  const double frames = v->cap.get(cv::CAP_PROP_FRAME_COUNT);
  const json j = {{"width", static_cast<int>(v->cap.get(cv::CAP_PROP_FRAME_WIDTH))},
                  {"height", static_cast<int>(v->cap.get(cv::CAP_PROP_FRAME_HEIGHT))},
                  {"fps", v->fps},
                  {"frames", frames},
                  {"duration_s", frames / v->fps}};
  return copyOut(j.dump(), buf, buf_len);
#else
  return 0;
#endif
}

int32_t pe_video_seek(pe_video* v, double t_seconds) {
#ifdef PE_WITH_VIDEOIO
  if (!v) return -1;
  v->pending = false;
  const int frame = std::max(0, static_cast<int>(std::lround(t_seconds * v->fps)));
  v->index = frame;
  return v->cap.set(cv::CAP_PROP_POS_FRAMES, frame) ? 0 : -1;
#else
  return -1;
#endif
}

int32_t pe_video_read_jpeg(pe_video* v, int32_t max_width, int32_t quality, uint8_t* buf, int32_t buf_len,
                           double* t_out) {
#ifdef PE_WITH_VIDEOIO
  if (!v) return -1;
  if (!v->pending) {
    cv::Mat frame;
    if (!v->cap.read(frame)) return 0;
    double t = v->cap.get(cv::CAP_PROP_POS_MSEC) / 1000.0;
    if (!(t > 0)) t = v->index / v->fps;
    ++v->index;
    if (max_width > 0 && frame.cols > max_width) {
      cv::resize(frame, frame, cv::Size(max_width, frame.rows * max_width / frame.cols), 0, 0, cv::INTER_AREA);
    }
    cv::imencode(".jpg", frame, v->jpeg, {cv::IMWRITE_JPEG_QUALITY, std::clamp(quality, 30, 100)});
    v->t = t;
    v->pending = true;
  }
  const int32_t n = static_cast<int32_t>(v->jpeg.size());
  if (t_out) *t_out = v->t;
  if (buf_len < n) return -n;
  std::memcpy(buf, v->jpeg.data(), v->jpeg.size());
  v->pending = false;
  return n;
#else
  return -1;
#endif
}

void pe_video_close(pe_video* v) { delete v; }

}  // extern "C"
