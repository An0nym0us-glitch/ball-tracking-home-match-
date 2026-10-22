// pocketeye-cli: video + session config in, delivery JSON (trajectory, metrics, LBW) out.
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>

#include "pocketeye/autocal.hpp"
#include "pocketeye/config.hpp"
#include "pocketeye/engine.hpp"
#include "pocketeye/result_json.hpp"
#include "pocketeye/video.hpp"

namespace {

using nlohmann::json;

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
      --gray                   detect on brightness only (like a luma-only stream)
      --jpeg-stream            frames were each compressed as a JPEG (the old-phone
                               camera app's stream to the laptop)
      --tracks-only            debugging: list raw tracks without analysing them
      --dump-candidates f.json debugging: per-frame ball candidates
  pocketeye-cli autocal --image frame.png --config session.json [--out found.json]
      finds the stumps in a picture (taps in the config are ignored); measures
      the pitch length if the config has none and both stump sets are visible
  pocketeye-cli benchmark [--width 1280 --height 720 --frames 240]
  pocketeye-cli version
)";
}

std::map<std::string, std::string> parseArgs(int argc, char** argv, int start) {
  std::map<std::string, std::string> a;
  for (int i = start; i < argc; ++i) {
    std::string k = argv[i];
    if (k.rfind("--", 0) != 0) throw std::runtime_error("unexpected argument: " + k);
    k = k.substr(2);
    if (k == "no-shot" || k == "keep-rejected" || k == "tracks-only" || k == "gray" || k == "jpeg-stream") {
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

int cmdAutocal(const std::map<std::string, std::string>& args) {
  json cj = readJson(args.at("config"));
  const cv::Mat img = cv::imread(args.at("image"), cv::IMREAD_COLOR);
  if (img.empty()) throw std::runtime_error("cannot read " + args.at("image"));
  cj["image"] = {{"width", img.cols}, {"height", img.rows}};
  cj.erase("taps");
  const pe::AutoCalibration a = pe::autoCalibrate(img, pe::calibrationInputFromJson(cj));
  writeOut(pe::autoCalibrationToJson(a), args);
  return a.ok ? 0 : 3;
}

int cmdAnalyze(const std::map<std::string, std::string>& args) {
  json cj = readJson(args.at("config"));
  if (args.count("ball")) cj["ball"] = args.at("ball");
  if (args.count("batter")) cj["batter"] = args.at("batter");
  if (args.count("no-shot")) cj["no_shot_offered"] = true;
  if (args.count("jpeg-stream")) cj["source"] = "jpeg_stream";
  pe::LoadedConfig cfg = pe::loadConfig(cj);
  cfg.engine.keep_rejected = args.count("keep-rejected") > 0 || args.count("tracks-only") > 0;
  cfg.engine.skip_analysis = args.count("tracks-only") > 0;
  for (const auto& w : cfg.calibration.warnings) std::cerr << "calibration warning: " << w << "\n";

  pe::VideoAnalysisOptions opt;
  opt.video_path = args.at("video");
  if (args.count("timestamps")) {
    std::ifstream f(args.at("timestamps"));
    if (!f) throw std::runtime_error("cannot open " + args.at("timestamps"));
    for (double t; f >> t;) opt.timestamps.push_back(t);
  }
  if (args.count("fps")) opt.fps_override = std::stod(args.at("fps"));
  if (args.count("overlay")) opt.overlay_path = args.at("overlay");
  opt.gray = args.count("gray") > 0;
  json cand_dump = json::array();
  if (args.count("dump-candidates")) {
    opt.on_frame = [&cand_dump](int i, double, const pe::Engine& engine) {
      if (i < 2) return;
      json c = json::array();
      for (const auto& cd : engine.lastCandidates()) {
        c.push_back({cd.uv.x(), cd.uv.y(), cd.radius_px, cd.area, cd.elongation, cd.strength});
      }
      cand_dump.push_back({{"frame", i - 1}, {"c", c}});
    };
  }
  json out = pe::analyzeVideoFile(cfg, opt);
  if (out.contains("overlay_error")) std::cerr << "overlay: " << out["overlay_error"] << "\n";
  if (args.count("dump-candidates")) {
    // Candidates belong to the frame before the one just pushed: add its time.
    std::vector<double> ts = opt.timestamps;
    for (auto& c : cand_dump) {
      const int fr = c["frame"].get<int>();
      c["t"] = fr < static_cast<int>(ts.size()) ? ts[fr] : 0.0;
    }
    std::ofstream f(args.at("dump-candidates"));
    f << cand_dump.dump() << "\n";
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
    if (cmd == "autocal") return cmdAutocal(args);
    if (cmd == "benchmark") {
      const int w = args.count("width") ? std::stoi(args.at("width")) : 1280;
      const int h = args.count("height") ? std::stoi(args.at("height")) : 720;
      const int n = args.count("frames") ? std::stoi(args.at("frames")) : 240;
      std::cout << pe::runBenchmark(w, h, n).dump(2) << "\n";
      return 0;
    }
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
