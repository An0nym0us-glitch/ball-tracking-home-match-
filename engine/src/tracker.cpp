#include "pocketeye/tracker.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace pe {

namespace {

struct Node {
  int f;  // index into the frame window
  int c;  // candidate index
};

struct Edge {
  int from;  // node id
  int to;    // node id
  double score;
  int prev;  // edge id, -1 = path starts here
  int length;
  bool turned = false;  // the path turned sharply at `to` (velocity estimate unreliable next)
};

}  // namespace

std::vector<std::vector<std::pair<int, int>>> findBallPaths(const std::deque<FrameCandidates>& frames,
                                                            const PathConfig& cfg, int image_width) {
  std::vector<std::vector<std::pair<int, int>>> paths;
  std::vector<std::vector<bool>> used;
  for (const auto& fr : frames) used.push_back(fr.used);
  const double max_step = cfg.max_step_frac * image_width;

  for (int round = 0; round < cfg.max_paths; ++round) {
    std::vector<Node> nodes;
    std::vector<std::vector<int>> node_id(frames.size());
    for (size_t f = 0; f < frames.size(); ++f) {
      node_id[f].assign(frames[f].cands.size(), -1);
      for (size_t c = 0; c < frames[f].cands.size(); ++c) {
        if (used[f][c]) continue;
        node_id[f][c] = static_cast<int>(nodes.size());
        nodes.push_back({static_cast<int>(f), static_cast<int>(c)});
      }
    }
    std::vector<Edge> edges;
    std::vector<std::vector<int>> into(nodes.size());  // edge ids ending at node
    auto cand = [&](int n) -> const Candidate& { return frames[nodes[n].f].cands[nodes[n].c]; };
    auto time = [&](int n) { return frames[nodes[n].f].t; };
    auto reward = [&](const Candidate& c) {
      return cfg.node_reward * (0.5 + 0.5 * std::min(1.0, c.strength / cfg.strength_ref));
    };

    for (size_t b = 0; b < nodes.size(); ++b) {
      const int fb = nodes[b].f;
      const Candidate& cb = cand(static_cast<int>(b));
      for (int fa = std::max(0, fb - cfg.max_gap); fa < fb; ++fa) {
        for (size_t ca = 0; ca < frames[fa].cands.size(); ++ca) {
          const int a = node_id[fa][ca];
          if (a < 0) continue;
          const Candidate& cA = cand(a);
          const int gap = fb - fa;
          const double dist = (cb.uv - cA.uv).norm();
          if (dist > max_step * gap || dist < cfg.min_step_px * gap) continue;
          const double ratio = (cb.radius_px + 0.5) / (cA.radius_px + 0.5);
          if (ratio < 0.25 || ratio > 4.0) continue;  // blur makes radii unreliable
          const double gap_pen = cfg.gap_cost * (gap - 1);
          const double reward_b = reward(cb);
          Edge e{a, static_cast<int>(b), reward(cA) + reward_b - cfg.start_cost - gap_pen, -1, 2, false};
          const double ta = time(a), tb = time(static_cast<int>(b));
          for (int pe_id : into[a]) {
            const Edge& p = edges[pe_id];
            const Candidate& cz = cand(p.from);
            const double tz = time(p.from);
            const Vec2 v1 = (cA.uv - cz.uv) / (ta - tz);
            const Vec2 pred = cA.uv + v1 * (tb - ta);
            const double step = v1.norm() * (tb - ta);
            double tol = cfg.tol_px + cfg.tol_rel * step + 0.25 * (cA.radius_px + cb.radius_px);
            // A bounce spreads over two frames: right after a sharp turn the
            // velocity from the last two points is unreliable.
            if (p.turned) tol *= 3.0;
            const double r = (cb.uv - pred).norm() / tol;
            double pen = std::min(r * r, cfg.kink_cost);
            if (ratio < 0.4 || ratio > 2.5) pen += 1.5;
            const double s = p.score + reward_b - pen - gap_pen;
            if (s > e.score) {
              e.score = s;
              e.prev = pe_id;
              e.length = p.length + 1;
              e.turned = r > 2.0;
            }
          }
          into[b].push_back(static_cast<int>(edges.size()));
          edges.push_back(e);
        }
      }
    }
    int best = -1;
    for (size_t i = 0; i < edges.size(); ++i) {
      if (edges[i].length < cfg.min_points) continue;
      if (best < 0 || edges[i].score > edges[best].score) best = static_cast<int>(i);
    }
    if (best < 0 || edges[best].score <= 0) break;
    std::vector<std::pair<int, int>> path;
    int e = best;
    path.push_back({nodes[edges[e].to].f, nodes[edges[e].to].c});
    while (e >= 0) {
      path.push_back({nodes[edges[e].from].f, nodes[edges[e].from].c});
      e = edges[e].prev;
    }
    std::reverse(path.begin(), path.end());
    for (const auto& [f, c] : path) used[f][c] = true;
    paths.push_back(std::move(path));
  }
  return paths;
}

Tracker::Tracker(int image_width, PathConfig cfg, double window_s, double settle_s)
    : width_(image_width), cfg_(cfg), window_s_(window_s), settle_s_(settle_s) {}

void Tracker::update(int frame, double t, const std::vector<Candidate>& cands) {
  t_now_ = t;
  FrameCandidates fc;
  fc.frame = frame;
  fc.t = t;
  // Candidates come strongest first.
  fc.cands.assign(cands.begin(), cands.begin() + std::min<size_t>(cands.size(), cfg_.max_frame_candidates));
  fc.used.assign(fc.cands.size(), false);
  frames_.push_back(std::move(fc));
  if (++since_search_ >= 4) {
    since_search_ = 0;
    search(false);
  }
  while (!frames_.empty() && frames_.front().t < t - window_s_) frames_.pop_front();
}

void Tracker::flush() {
  search(true);
  frames_.clear();
}

void Tracker::search(bool all) {
  const auto paths = findBallPaths(frames_, cfg_, width_);
  for (const auto& path : paths) {
    const FrameCandidates& last = frames_[path.back().first];
    // Still being followed: wait until the ball has been gone for a while,
    // unless the window is about to drop its first frames.
    const bool settled = all || last.t < t_now_ - settle_s_;
    const bool expiring = frames_[path.front().first].t < t_now_ - window_s_ + 0.2;
    if (!settled && !expiring) continue;
    std::vector<TrackPoint> pts;
    for (const auto& [f, c] : path) {
      const Candidate& cd = frames_[f].cands[c];
      pts.push_back({frames_[f].frame, frames_[f].t, cd.uv, cd.radius_px, cd.strength});
      frames_[f].used[c] = true;
    }
    finished_.push_back(std::move(pts));
  }
}

std::vector<std::vector<TrackPoint>> Tracker::takeFinished(size_t min_points) {
  std::vector<std::vector<TrackPoint>> out;
  for (auto& f : finished_) {
    if (f.size() >= min_points) out.push_back(std::move(f));
  }
  finished_.clear();
  return out;
}

}  // namespace pe
