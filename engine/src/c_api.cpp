#include "pocketeye/pocketeye_c.h"

#include <cstring>
#include <deque>
#include <memory>
#include <string>

#include "pocketeye/config.hpp"
#include "pocketeye/engine.hpp"
#include "pocketeye/result_json.hpp"

struct pe_engine {
  pe::LoadedConfig config;
  std::unique_ptr<pe::Engine> engine;
  std::deque<std::string> pending;
};

namespace {

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

}  // namespace

extern "C" {

const char* pe_version(void) { return "pocketeye 0.1.0"; }

pe_engine* pe_create(const char* config_json, char* err, int32_t err_len) {
  try {
    auto e = std::make_unique<pe_engine>();
    e->config = pe::loadConfig(nlohmann::json::parse(config_json));
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

int32_t pe_push_luma(pe_engine* e, const uint8_t* data, int32_t width, int32_t height,
                     int32_t stride, double t_seconds) {
  if (!e || !data) return -1;
  const pe::Camera& cam = e->config.engine.analysis.camera;
  if (width != cam.width || height != cam.height) return -2;
  try {
    const cv::Mat gray(height, width, CV_8UC1, const_cast<uint8_t*>(data), static_cast<size_t>(stride));
    e->engine->pushFrame(gray, t_seconds);
    drain(e);
    return 0;
  } catch (...) {
    return -3;
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

}  // extern "C"
