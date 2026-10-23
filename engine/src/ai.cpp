#include "pocketeye/ai.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <opencv2/core/ocl.hpp>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

namespace pe {

namespace {
using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// Letterbox into a square of `input` px (grey 114 padding, as YOLOX was trained).
cv::Mat letterbox(const cv::Mat& bgr, int input, double& scale) {
  scale = std::min(static_cast<double>(input) / bgr.cols, static_cast<double>(input) / bgr.rows);
  cv::Mat out(input, input, CV_8UC3, cv::Scalar::all(114));
  cv::Mat resized;
  const int w = std::max(1, static_cast<int>(std::lround(bgr.cols * scale)));
  const int h = std::max(1, static_cast<int>(std::lround(bgr.rows * scale)));
  cv::resize(bgr, resized, cv::Size(w, h), 0, 0, scale > 1.0 ? cv::INTER_CUBIC : cv::INTER_AREA);
  resized.copyTo(out(cv::Rect(0, 0, w, h)));
  return out;
}
}  // namespace

std::vector<AiBox> decodeYolox(const cv::Mat& out_in, int input, int cls, float min_score) {
  // out: rows of [cx, cy, log w, log h, objectness, 80 class scores] on the
  // stride 8, 16 and 32 grids, in that order.
  const cv::Mat out = out_in.reshape(1, static_cast<int>(out_in.total() / 85));
  std::vector<cv::Rect2d> boxes;
  std::vector<float> scores;
  int row = 0;
  for (int stride : {8, 16, 32}) {
    const int n = input / stride;
    for (int gy = 0; gy < n; ++gy) {
      for (int gx = 0; gx < n; ++gx, ++row) {
        if (row >= out.rows) break;
        const float* p = out.ptr<float>(row);
        const float score = p[4] * p[5 + cls];
        if (score < min_score) continue;
        const double cx = (p[0] + gx) * stride, cy = (p[1] + gy) * stride;
        const double w = std::exp(p[2]) * stride, h = std::exp(p[3]) * stride;
        boxes.emplace_back(cx - w / 2, cy - h / 2, w, h);
        scores.push_back(score);
      }
    }
  }
  std::vector<int> keep;
  cv::dnn::NMSBoxes(boxes, scores, min_score, 0.45f, keep);
  std::vector<AiBox> result;
  for (int i : keep) {
    result.push_back({cv::Rect2f(static_cast<float>(boxes[i].x), static_cast<float>(boxes[i].y),
                                 static_cast<float>(boxes[i].width), static_cast<float>(boxes[i].height)),
                      scores[i]});
  }
  std::sort(result.begin(), result.end(), [](const AiBox& a, const AiBox& b) { return a.score > b.score; });
  return result;
}

struct AiAssist::Net {
  cv::dnn::Net net;
};

AiAssist::AiAssist(AiConfig cfg) : cfg_(std::move(cfg)) { thread_ = std::thread(&AiAssist::worker, this); }

AiAssist::~AiAssist() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    stop_ = true;
  }
  cv_.notify_all();
  thread_.join();
}

bool AiAssist::wantsPeople(double t) const {
  std::lock_guard<std::mutex> lock(mu_);
  return status_.ready && !people_pending_ && t - people_sent_t_ >= 1.0 / std::max(cfg_.person_hz, 0.1);
}

void AiAssist::submitPeople(double t, cv::Mat bgr, double to_picture) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    people_in_ = std::move(bgr);
    people_t_ = t;
    people_scale_ = to_picture;
    people_pending_ = true;
    people_sent_t_ = t;
  }
  cv_.notify_all();
}

void AiAssist::submitCheck(const AiCheck& check, cv::Mat bgr) {
  {
    std::lock_guard<std::mutex> lock(mu_);
    checks_in_.emplace_back(check, std::move(bgr));
    while (checks_in_.size() > 4) checks_in_.pop_front();  // never fall behind the camera
  }
  cv_.notify_all();
}

bool AiAssist::people(std::vector<AiBox>& out, double& t) const {
  std::lock_guard<std::mutex> lock(mu_);
  if (people_out_t_ < 0) return false;
  out = people_out_;
  t = people_out_t_;
  return true;
}

std::vector<AiCheckResult> AiAssist::takeChecks() {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<AiCheckResult> out;
  out.swap(checks_out_);
  return out;
}

AiStatus AiAssist::status() const {
  std::lock_guard<std::mutex> lock(mu_);
  return status_;
}

std::vector<AiBox> AiAssist::detectNow(const cv::Mat& bgr, int cls, float min_score) {
  auto done = std::make_shared<std::promise<std::vector<AiBox>>>();
  auto result = done->get_future();
  {
    std::lock_guard<std::mutex> lock(mu_);
    calls_.push_back([this, &bgr, cls, min_score, done] { done->set_value(run(bgr, cls, min_score)); });
  }
  cv_.notify_all();
  return result.get();
}

bool AiAssist::load() {
  AiStatus st;
  try {
    // The GPU: OpenCL devices of all platforms. A dedicated GPU has its own
    // memory (not shared with the processor); pick the one with the most.
    cv::ocl::Device chosen;
    std::string note;
    const bool want_gpu = cfg_.device == "dgpu" || cfg_.device == "igpu";
    if (want_gpu && cv::ocl::haveOpenCL()) {
      std::vector<cv::ocl::PlatformInfo> platforms;
      cv::ocl::getPlatfomsInfo(platforms);
      cv::ocl::Device other;
      for (const auto& pl : platforms) {
        for (int d = 0; d < pl.deviceNumber(); ++d) {
          cv::ocl::Device dev;
          pl.getDevice(dev, d);
          if (!(dev.type() & cv::ocl::Device::TYPE_GPU) || !dev.available()) continue;
          const bool dedicated = !dev.hostUnifiedMemory();
          const bool wanted = dedicated == (cfg_.device == "dgpu");
          cv::ocl::Device& slot = wanted ? chosen : other;
          if (slot.ptr() == nullptr || dev.globalMemSize() > slot.globalMemSize()) slot = dev;
        }
      }
      if (chosen.ptr() == nullptr && other.ptr() != nullptr) {
        chosen = other;
        note = cfg_.device == "dgpu" ? "no dedicated GPU found: using the integrated one"
                                     : "no integrated GPU found: using the dedicated one";
      }
      if (chosen.ptr() == nullptr) note = "no GPU with OpenCL found: using the CPU";
    } else if (want_gpu) {
      note = "OpenCL not available: using the CPU";
    }
    net_ = new Net;
    net_->net = cv::dnn::readNetFromONNX(cfg_.model);
    net_->net.setPreferableBackend(cv::dnn::DNN_BACKEND_OPENCV);
    if (chosen.ptr() != nullptr) {
      const cv::ocl::Context ctx = cv::ocl::Context::fromDevice(chosen);
      cv::ocl::OpenCLExecutionContext::create(ctx, chosen).bind();  // this thread only
      cv::ocl::setUseOpenCL(true);
      net_->net.setPreferableTarget(cv::dnn::DNN_TARGET_OPENCL);
      st.device = chosen.name() + (chosen.hostUnifiedMemory() ? " (integrated GPU)" : " (dedicated GPU)");
    } else {
      net_->net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
      st.device = "CPU";
    }
    // First run: compiles the GPU kernels (seconds); a failure here falls back to the CPU.
    try {
      run(cv::Mat(cfg_.input, cfg_.input, CV_8UC3, cv::Scalar::all(114)), kPerson, 0.5f);
    } catch (const cv::Exception& e) {
      if (chosen.ptr() == nullptr) throw;
      net_->net.setPreferableTarget(cv::dnn::DNN_TARGET_CPU);
      st.device = "CPU";
      note = std::string("the GPU failed (") + e.what() + "): using the CPU";
      run(cv::Mat(cfg_.input, cfg_.input, CV_8UC3, cv::Scalar::all(114)), kPerson, 0.5f);
    }
    st.ready = true;
    st.note = note;
  } catch (const std::exception& e) {
    st.ready = false;
    st.error = std::string("AI model could not be loaded: ") + e.what();
    delete net_;
    net_ = nullptr;
  }
  std::lock_guard<std::mutex> lock(mu_);
  status_ = st;
  return st.ready;
}

std::vector<AiBox> AiAssist::run(const cv::Mat& bgr, int cls, float min_score) {
  if (net_ == nullptr) return {};
  double scale = 1.0;
  const cv::Mat in = letterbox(bgr, cfg_.input, scale);
  net_->net.setInput(cv::dnn::blobFromImage(in, 1.0, cv::Size(cfg_.input, cfg_.input), cv::Scalar(), false, false));
  const cv::Mat out = net_->net.forward();
  std::vector<AiBox> boxes = decodeYolox(out, cfg_.input, cls, min_score);
  for (AiBox& b : boxes) {
    b.box = cv::Rect2f(static_cast<float>(b.box.x / scale), static_cast<float>(b.box.y / scale),
                       static_cast<float>(b.box.width / scale), static_cast<float>(b.box.height / scale));
  }
  return boxes;
}

void AiAssist::worker() {
  load();
  std::unique_lock<std::mutex> lock(mu_);
  while (true) {
    cv_.wait(lock, [this] { return stop_ || people_pending_ || !checks_in_.empty() || !calls_.empty(); });
    if (stop_) break;
    if (!calls_.empty()) {
      auto call = std::move(calls_.front());
      calls_.pop_front();
      lock.unlock();
      call();
      lock.lock();
      continue;
    }
    if (people_pending_) {
      cv::Mat img = std::move(people_in_);
      const double t = people_t_, scale = people_scale_;
      people_pending_ = false;
      lock.unlock();
      const auto t0 = Clock::now();
      std::vector<AiBox> boxes;
      try {
        boxes = run(img, kPerson, 0.4f);
      } catch (const std::exception&) {
      }
      for (AiBox& b : boxes) {
        b.box = cv::Rect2f(static_cast<float>(b.box.x * scale), static_cast<float>(b.box.y * scale),
                           static_cast<float>(b.box.width * scale), static_cast<float>(b.box.height * scale));
      }
      const double ms = msSince(t0);
      lock.lock();
      people_out_ = std::move(boxes);
      people_out_t_ = t;
      status_.person_ms = ms;
      continue;
    }
    auto [check, crop] = std::move(checks_in_.front());
    checks_in_.pop_front();
    lock.unlock();
    const auto t0 = Clock::now();
    AiCheckResult r;
    r.id = check.id;
    r.t = check.t;
    r.uv = check.uv;
    try {
      const auto balls = run(crop, kSportsBall, 0.05f);
      if (!balls.empty()) {
        const cv::Rect2f& b = balls.front().box;
        r.score = balls.front().score;
        r.uv = check.uv + Vec2(b.x + b.width / 2 - crop.cols / 2.0, b.y + b.height / 2 - crop.rows / 2.0);
      }
    } catch (const std::exception&) {
    }
    const double ms = msSince(t0);
    lock.lock();
    checks_out_.push_back(r);
    status_.check_ms = ms;
    ++status_.checks;
  }
  lock.unlock();
  delete net_;
  net_ = nullptr;
}

}  // namespace pe
