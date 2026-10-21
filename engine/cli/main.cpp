// pocketeye-cli: video + session config in, delivery JSON (trajectory, metrics, LBW) out.
#include <chrono>
#include <fstream>
#include <iostream>
#include <map>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <string>
#include <vector>

#include "overlay.hpp"
#include "pocketeye/config.hpp"
#include "pocketeye/engine.hpp"
#include "pocketeye/result_json.hpp"

namespace {

using nlohmann::json;
using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

void usage() {
  std::cerr <<
      R"(usage:
  pocketeye-cli calibrate --config session.json [--out calibration.json]
  pocketeye-cli analyze --video clip.mp4 --config session.json [options]
      --out result.json        write JSON here (default: stdout)
      --overlay replay.mp4     write a replay with the trajectory drawn
      --timestamps ts.txt      per-frame capture times in seconds (one per line)
      --fps N                  use frame_index / N as the capture time
      --ball tennis|plastic|rubber|leather
      --batter right|left
      --no-shot                batter offered no shot (allows impact outside off)
      --keep-rejected          also list tracks that were not deliveries
  pocketeye-cli version
)";
}

std::map<std::string, std::string> parseArgs(int argc, char** argv, int start) {
  std::map<std::string, std::string> a;
  for (int i = start; i < argc; ++i) {
    std::string k = argv[i];
    if (k.rfind("--", 0) != 0) throw std::runtime_error("unexpected argument: " + k);
    k = k.substr(2);
    if (k == "no-shot" || k == "keep-rejected") {
      a[k] = "1";
    } else {
      if (i + 1 >= argc) throw std::runtime_error("missing value for --" + k);
      a[k] = argv[++i];
    }
  }
  return a;
}

json readJson(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot open " + path);
  return json::parse(f);
}

void writeOut(const json& j, const std::map<std::string, std::string>& args) {
  const auto it = args.find("out");
  if (it == args.end()) {
    std::cout << j.dump(2) << "\n";
    return;
  }
  std::ofstream f(it->second);
  if (!f) throw std::runtime_error("cannot write " + it->second);
  f << j.dump(2) << "\n";
}

int cmdCalibrate(const std::map<std::string, std::string>& args) {
  const pe::LoadedConfig cfg = pe::loadConfig(readJson(args.at("config")));
  writeOut(pe::calibrationToJson(cfg.calibration), args);
  for (const auto& w : cfg.calibration.warnings) std::cerr << "warning: " << w << "\n";
  return 0;
}

int cmdAnalyze(const std::map<std::string, std::string>& args) {
  json cj = readJson(args.at("config"));
  if (args.count("ball")) cj["ball"] = args.at("ball");
  if (args.count("batter")) cj["batter"] = args.at("batter");
  if (args.count("no-shot")) cj["no_shot_offered"] = true;
  pe::LoadedConfig cfg = pe::loadConfig(cj);
  cfg.engine.keep_rejected = args.count("keep-rejected") > 0;
  for (const auto& w : cfg.calibration.warnings) std::cerr << "calibration warning: " << w << "\n";

  const std::string video = args.at("video");
  cv::VideoCapture cap;
  cap.open(video, cv::CAP_FFMPEG, {cv::CAP_PROP_HW_ACCELERATION, cv::VIDEO_ACCELERATION_ANY});
  if (!cap.isOpened()) cap.open(video);
  if (!cap.isOpened()) throw std::runtime_error("cannot open video " + video);
  const double file_fps = cap.get(cv::CAP_PROP_FPS);

  std::vector<double> given_ts;
  if (args.count("timestamps")) {
    std::ifstream f(args.at("timestamps"));
    if (!f) throw std::runtime_error("cannot open " + args.at("timestamps"));
    for (double t; f >> t;) given_ts.push_back(t);
  }
  const double fps_override = args.count("fps") ? std::stod(args.at("fps")) : 0.0;

  pe::Engine engine(cfg.engine);
  std::vector<pe::DeliveryResult> results;
  std::vector<double> timestamps;
  cv::Mat frame, gray;
  double decode_ms = 0, convert_ms = 0, engine_ms = 0;
  const auto t_all = Clock::now();
  for (int i = 0;; ++i) {
    auto t0 = Clock::now();
    if (!cap.read(frame)) break;
    decode_ms += msSince(t0);
    double t;
    if (i < static_cast<int>(given_ts.size())) {
      t = given_ts[i];
    } else if (fps_override > 0) {
      t = i / fps_override;
    } else {
      t = cap.get(cv::CAP_PROP_POS_MSEC) / 1000.0;
      if (i > 0 && t <= timestamps.back()) t = i / (file_fps > 0 ? file_fps : 30.0);
    }
    timestamps.push_back(t);
    if (frame.cols != cfg.engine.analysis.camera.width || frame.rows != cfg.engine.analysis.camera.height) {
      throw std::runtime_error("video is " + std::to_string(frame.cols) + "x" + std::to_string(frame.rows) +
                               " but the calibration is for " +
                               std::to_string(cfg.engine.analysis.camera.width) + "x" +
                               std::to_string(cfg.engine.analysis.camera.height));
    }
    t0 = Clock::now();
    if (frame.channels() == 3) {
      cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    } else {
      gray = frame;
    }
    convert_ms += msSince(t0);
    t0 = Clock::now();
    engine.pushFrame(gray, t);
    engine_ms += msSince(t0);
    for (pe::DeliveryResult r; engine.popResult(r);) results.push_back(std::move(r));
  }
  auto t0 = Clock::now();
  engine.finish();
  engine_ms += msSince(t0);
  for (pe::DeliveryResult r; engine.popResult(r);) results.push_back(std::move(r));
  const double total_ms = msSince(t_all);

  json out;
  out["version"] = "pocketeye 0.1.0";
  out["video"] = {{"path", video}, {"frames", timestamps.size()}, {"fps_reported", file_fps}};
  out["calibration"] = pe::calibrationToJson(cfg.calibration);
  out["ball"] = cfg.engine.analysis.ball.name;
  out["batter"] = cfg.engine.analysis.batter == pe::Hand::Right ? "right" : "left";
  json dl = json::array();
  for (const auto& r : results) dl.push_back(pe::deliveryToJson(r, cfg.engine.analysis));
  out["deliveries"] = dl;
  const auto& st = engine.stats();
  const double n = std::max<size_t>(1, timestamps.size());
  out["timings_ms"] = {{"decode_total", decode_ms},
                       {"decode_per_frame", decode_ms / n},
                       {"convert_total", convert_ms},
                       {"detect_total", st.detect_ms},
                       {"detect_per_frame", st.detect_ms / n},
                       {"track_total", st.track_ms},
                       {"analyze_total", st.analyze_ms},
                       {"engine_total", engine_ms},
                       {"wall_total", total_ms}};
  out["tracks_analyzed"] = st.tracks_analyzed;

  if (args.count("overlay")) {
    std::string err;
    if (!pe::writeOverlayVideo(video, timestamps, results, cfg.engine.analysis, args.at("overlay"), &err)) {
      std::cerr << "overlay: " << err << "\n";
    }
  }
  writeOut(out, args);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string cmd = argv[1];
  try {
    if (cmd == "version") {
      std::cout << "pocketeye 0.1.0\n";
      return 0;
    }
    const auto args = parseArgs(argc, argv, 2);
    if (cmd == "calibrate") return cmdCalibrate(args);
    if (cmd == "analyze") return cmdAnalyze(args);
  } catch (const std::out_of_range&) {
    usage();
    return 2;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  usage();
  return 2;
}
