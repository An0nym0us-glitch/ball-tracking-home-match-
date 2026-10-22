#include "pocketeye/autocal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <map>
#include <tuple>
#include <sstream>
#include <opencv2/imgproc.hpp>

#include "pocketeye/config.hpp"

namespace pe {

namespace {

// Vertical-bar response at one bar width: how much a k-pixel box differs from
// the boxes on BOTH sides (the smaller of the two steps). A thin stump stands
// out on both sides; the edge of something wide (pads, a person, the
// background next to the outer stump) only on one, and gives nothing.
struct RidgeMap {
  int k = 1;
  cv::Mat signed_ridge;  // CV_32F: light bars positive, dark bars negative
  cv::Mat ii[2];         // integral images (CV_32S) of the light (0) and dark (1) response, dilated sideways
  cv::Mat ii_raw[2];     // the same without dilation (for the gaps between bars)
};

// k: bar width; d: distance to the background samples (the middle of the
// gap between stumps); kn: width of the background samples (the gap width).
RidgeMap makeRidge(const cv::Mat& g, int k, int d, int kn) {
  RidgeMap r;
  r.k = k;
  cv::Mat box, nbox;
  cv::blur(g, box, cv::Size(k, 1), cv::Point(-1, -1), cv::BORDER_REPLICATE);
  cv::blur(g, nbox, cv::Size(kn, 1), cv::Point(-1, -1), cv::BORDER_REPLICATE);
  cv::Mat kl = cv::Mat::zeros(1, 2 * d + 1, CV_32F), kr = cv::Mat::zeros(1, 2 * d + 1, CV_32F);
  kl.at<float>(0, 0) = 1.0f;      // sample at x - d
  kr.at<float>(0, 2 * d) = 1.0f;  // sample at x + d
  cv::Mat nl, nr;
  cv::filter2D(nbox, nl, CV_32F, kl, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
  cv::filter2D(nbox, nr, CV_32F, kr, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
  const cv::Mat left = box - nl, right = box - nr;
  cv::Mat lo, hi;
  cv::min(left, right, lo);
  cv::max(left, right, hi);
  const cv::Mat light = cv::max(lo, 0.0);
  const cv::Mat dark = cv::max(-hi, 0.0);
  r.signed_ridge = light - dark;
  // Bars are not exactly vertical (camera roll, perspective): allow some sideways slack.
  const int tol = std::max(1, k / 2);
  const cv::Mat dil = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2 * tol + 1, 1));
  for (int s = 0; s < 2; ++s) {
    // Grey levels fit in 8 bits: exact 32-bit sums, a quarter of the memory of doubles.
    cv::Mat part;
    (s == 0 ? light : dark).convertTo(part, CV_8U);
    cv::integral(part, r.ii_raw[s], CV_32S);
    cv::Mat wide;
    cv::dilate(part, wide, dil);
    cv::integral(wide, r.ii[s], CV_32S);
  }
  return r;
}

using RidgeCache = std::map<std::tuple<int, int, int>, RidgeMap>;

// Ridge map for stumps with bar spacing s (px).
const RidgeMap& ridgeFor(RidgeCache& cache, const cv::Mat& g, double s, double bar_frac, int k_max) {
  const int k = std::clamp(static_cast<int>(std::lround(bar_frac * s)), 1, k_max);
  const int d = std::max(1, static_cast<int>(std::lround(0.5 * s)));
  const int kn = std::clamp(static_cast<int>(std::lround(s * (1.0 - bar_frac))), 1, k);
  const auto key = std::make_tuple(k, d, kn);
  auto it = cache.find(key);
  if (it == cache.end()) it = cache.emplace(key, makeRidge(g, k, d, kn)).first;
  return it->second;
}

// Sum of column x over rows [y0, y1) (clamped).
double colSum(const cv::Mat& ii, int x, int y0, int y1) {
  const int w = ii.cols - 1, h = ii.rows - 1;
  if (x < 0 || x >= w) return 0.0;
  y0 = std::clamp(y0, 0, h);
  y1 = std::clamp(y1, 0, h);
  if (y1 <= y0) return 0.0;
  return static_cast<double>(ii.at<int>(y1, x + 1) - ii.at<int>(y0, x + 1) - ii.at<int>(y1, x) + ii.at<int>(y0, x));
}

struct Raw {
  double score;
  int pol;   // 0 light, 1 dark
  double s;  // bar spacing (px)
  int x0;    // first bar column
  int y0;    // batter: base row (exclusive); bowler: top row
  int H;     // full bar height (px)
};

// Best per coarse cell, then best first with non-maximum suppression.
std::vector<Raw> pickBest(std::map<std::pair<int, int>, Raw>& cells, int max_n) {
  std::vector<Raw> all;
  for (auto& [key, r] : cells) all.push_back(r);
  std::sort(all.begin(), all.end(), [](const Raw& a, const Raw& b) { return a.score > b.score; });
  std::vector<Raw> out;
  for (const Raw& r : all) {
    bool near = false;
    for (const Raw& o : out) {
      near |= std::abs(r.x0 + r.s - (o.x0 + o.s)) < 2.0 * std::max(r.s, o.s) && std::abs(r.y0 - o.y0) < std::max(r.H, o.H);
    }
    if (!near) out.push_back(r);
    if (static_cast<int>(out.size()) >= max_n) break;
  }
  return out;
}

struct Geometry {
  double ratio_h;   // stump height / bar spacing
  double bar_frac;  // stump diameter / bar spacing
};

Geometry stumpGeometry(const Pitch& p) {
  const double spacing = std::max(0.02, p.outerStumpX());
  return {p.stump_height / spacing, std::clamp(p.stump_diameter / spacing, 0.1, 0.8)};
}

cv::Mat prepareGray(const cv::Mat& image) {
  cv::Mat gray, g;
  if (image.channels() == 3) {
    cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
  } else {
    gray = image;
  }
  gray.convertTo(g, CV_32F);
  // Light vertical smoothing: bars are long, noise is not.
  const cv::Mat kv = (cv::Mat_<float>(3, 1) << 0.25f, 0.5f, 0.25f);
  cv::filter2D(g, g, CV_32F, kv, cv::Point(-1, -1), 0, cv::BORDER_REPLICATE);
  return g;
}

// Sub-pixel column of a bar near x over rows [y0, y1): parabola through the peak.
double refineColumn(const cv::Mat& ridge, int x, int y0, int y1, int radius) {
  y0 = std::clamp(y0, 0, ridge.rows);
  y1 = std::clamp(y1, 0, ridge.rows);
  if (y1 <= y0) return x;
  auto col = [&](int c) {
    if (c < 0 || c >= ridge.cols) return -1e30;
    double s = 0;
    for (int y = y0; y < y1; ++y) s += ridge.at<float>(y, c);
    return s;
  };
  int best = x;
  double bv = -1e30;
  for (int c = x - radius; c <= x + radius; ++c) {
    const double v = col(c);
    if (v > bv) {
      bv = v;
      best = c;
    }
  }
  const double l = col(best - 1), r = col(best + 1);
  const double den = l - 2 * bv + r;
  double off = 0.0;
  if (l > -1e29 && r > -1e29 && den < 0) off = std::clamp(0.5 * (l - r) / den, -0.5, 0.5);
  return best + off;
}

// Mean signed ridge of the bars (at columns xs) on row y.
double rowResponse(const cv::Mat& ridge, const std::vector<int>& xs, int y) {
  if (y < 0 || y >= ridge.rows) return 0.0;
  double s = 0;
  for (int x : xs) {
    if (x < 0 || x >= ridge.cols) continue;
    float best = ridge.at<float>(y, x);
    if (x > 0) best = std::max(best, ridge.at<float>(y, x - 1));
    if (x + 1 < ridge.cols) best = std::max(best, ridge.at<float>(y, x + 1));
    s += best;
  }
  return s / xs.size();
}


// Where the bars end, traced from their middle. The trace follows the bars
// down to a low threshold (bridging short breaks such as a crease line behind
// them) but not beyond `limit`; the end is then placed at half the contrast
// found just inside it, so a change of background along the stumps does not
// move it. Returns false if the bars run out of the picture.
bool traceEnd(const cv::Mat& ridge, const std::vector<int>& xs, int y_mid, double thr, int dir, int max_gap, int limit,
              double& end) {
  auto resp = [&](int y) { return y >= 0 && y < ridge.rows ? rowResponse(ridge, xs, y) : 0.0; };
  auto good = [&](int y) { return y >= 0 && y < ridge.rows && resp(y) >= thr; };
  auto beyond = [&](int y) { return dir < 0 ? y < limit : y > limit; };
  int y = y_mid;
  while (true) {
    int next = -1;
    for (int j = 1; j <= max_gap + 1; ++j) {
      const int c = y + j * dir;
      if (c < 0 || c >= ridge.rows) {
        if (j == 1) return false;
        break;
      }
      if (beyond(c)) break;
      // Continue only onto a stretch of bar, not a single noisy row.
      if (good(c) && (j == 1 || (good(c + dir) && good(c + 2 * dir)))) {
        next = c;
        break;
      }
    }
    if (next < 0) break;
    y = next;
  }
  // Local contrast just inside the end.
  std::vector<double> inside;
  for (int i = 0; i < 6; ++i) {
    const double v = resp(y - i * dir);
    if (v >= thr) inside.push_back(v);
  }
  std::sort(inside.begin(), inside.end());
  const double half = 0.5 * (inside.empty() ? resp(y) : inside[inside.size() / 2]);
  int in = y;
  while (resp(in) < half && in != y_mid) in -= dir;
  while (resp(in + dir) >= half && !beyond(in + dir) && in + dir >= 0 && in + dir < ridge.rows) in += dir;
  const double a = resp(in), b = resp(in + dir);
  const double frac = a > b ? std::clamp((a - half) / (a - b), 0.0, 1.0) : 0.5;
  end = in + dir * frac;
  return true;
}

struct RefinedSet {
  bool ok = false;
  bool base_visible = false;
  double y_top = 0, y_base = 0;
  double xl_top = 0, xl_base = 0, xr_top = 0, xr_base = 0;
};

// Exact ends and outer bar columns of a stump set found near (xs, y_top..y_base).
// gray: raw brightness (for the bails), ridge: signed so that the bars are positive.
RefinedSet refineSet(const cv::Mat& gray, const cv::Mat& ridge, const std::vector<int>& xs, int y_top, int y_base,
                     double spacing, int k, double sign, double ratio_h) {
  RefinedSet r;
  const int lo = std::max(0, y_top), hi = std::min(ridge.rows, y_base);
  if (hi - lo < 4) return r;
  std::vector<double> resp;
  for (int y = lo + (hi - lo) / 4; y < hi - (hi - lo) / 4; ++y) resp.push_back(rowResponse(ridge, xs, y));
  if (resp.empty()) return r;
  std::nth_element(resp.begin(), resp.begin() + resp.size() / 2, resp.end());
  const double level = resp[resp.size() / 2];
  if (level <= 0) return r;
  const int y_mid = (lo + hi) / 2;
  // Expected height from the spacing of the outer bars (measured on the middle rows).
  const int rad0 = std::max(2, k);
  const double sp = 0.5 * (refineColumn(ridge, xs.back(), lo + (hi - lo) / 4, hi - (hi - lo) / 4, rad0) -
                           refineColumn(ridge, xs.front(), lo + (hi - lo) / 4, hi - (hi - lo) / 4, rad0));
  const double h_exp = (sp > 0.5 * spacing && sp < 1.5 * spacing ? sp : spacing) * ratio_h;
  const int max_gap = 1 + static_cast<int>(0.06 * h_exp);
  const double thr = 0.2 * level;
  double top = 0, base = 0;
  // The top must be in the picture.
  if (!traceEnd(ridge, xs, y_mid, thr, -1, max_gap, static_cast<int>(std::floor(y_mid - 1.1 * h_exp)), top)) return r;
  r.base_visible = traceEnd(ridge, xs, y_mid, thr, +1, max_gap, static_cast<int>(std::ceil(top + 1.1 * h_exp)), base);
  // Bars that stop right at the bottom of the picture are cut off, not standing there.
  if (r.base_visible && base > ridge.rows - 2 - max_gap) r.base_visible = false;
  // Bails: a band above the bars where the whole span (gaps too) is bar-coloured.
  // The stump top is about the middle of the bail.
  if (spacing >= 5.0) {
    const int xl = xs.front(), xr = xs.back();
    const int out = std::max(2, static_cast<int>(0.6 * spacing));
    auto span = [&](int y) {
      if (y < 0 || y >= gray.rows) return 0.0;
      double in = 0, bg = 0;
      int n_in = 0, n_bg = 0;
      for (int x = std::max(0, xl); x <= std::min(gray.cols - 1, xr); ++x, ++n_in) in += gray.at<float>(y, x);
      for (int x = xl - out - k; x < xl - k; ++x) {
        if (x >= 0) bg += gray.at<float>(y, x), ++n_bg;
      }
      for (int x = xr + k + 1; x <= xr + k + out; ++x) {
        if (x < gray.cols) bg += gray.at<float>(y, x), ++n_bg;
      }
      return n_in && n_bg ? sign * (in / n_in - bg / n_bg) : 0.0;
    };
    const int y0 = static_cast<int>(std::floor(top));
    int b = y0;
    const int max_rows = std::max(2, static_cast<int>(0.06 * (r.base_visible ? base - top : y_base - top)));
    while (y0 - b < max_rows && span(b - 1) > 0.6 * level) --b;
    if (b < y0) top = 0.5 * (b - 0.5 + top);
  }
  // Crease: the stumps stand in the middle of a painted line. The bars seem
  // to end where it starts; the base is the middle of the line.
  if (r.base_visible) {
    const int xl = xs.front(), xr = xs.back();
    const int pad = std::max(2, static_cast<int>(spacing));
    auto wide = [&](int y) {
      if (y < 0 || y >= gray.rows) return 0.0;
      double sum = 0;
      int n = 0;
      for (int x = std::max(0, xl - pad); x <= std::min(gray.cols - 1, xr + pad); ++x, ++n) sum += gray.at<float>(y, x);
      return n ? sum / n : 0.0;
    };
    const double h = base - top;
    const int y0 = static_cast<int>(std::floor(base)) + 1;
    const int g0 = y0 + std::max(3, static_cast<int>(0.15 * h)), g1 = g0 + std::max(2, static_cast<int>(0.05 * h));
    if (g1 < gray.rows) {
      double ground = 0;
      for (int y = g0; y < g1; ++y) ground += wide(y);
      ground /= g1 - g0;
      int b = y0;
      const int max_rows = std::max(2, static_cast<int>(0.12 * h));
      while (b - y0 < max_rows && sign * (wide(b) - ground) > 0.5 * level) ++b;
      if (b > y0) base = 0.5 * (base + b - 0.5);
    }
  }
  if (std::getenv("PE_AUTOCAL_DEBUG")) {
    std::fprintf(stderr, "refine xs %d..%d rows %d..%d level %.1f h_exp %.1f -> top %.2f base %.2f (visible %d)\n",
                 xs.front(), xs.back(), y_top, y_base, level, h_exp, top, base, r.base_visible);
  }
  r.y_top = top;
  r.y_base = r.base_visible ? base : y_base;
  const int it = static_cast<int>(std::ceil(top)), ib = static_cast<int>(std::floor(r.y_base));
  const int half = std::max(2, (std::min(ib, ridge.rows) - it) / 2);
  const int rad = std::max(2, k);
  r.xl_top = refineColumn(ridge, xs.front(), it, it + half, rad);
  r.xr_top = refineColumn(ridge, xs.back(), it, it + half, rad);
  r.xl_base = refineColumn(ridge, xs.front(), it + half, std::min(ib, ridge.rows), rad);
  r.xr_base = refineColumn(ridge, xs.back(), it + half, std::min(ib, ridge.rows), rad);
  r.ok = true;
  return r;
}

constexpr double kMinContrast = 6.0;  // grey levels per bar row

}  // namespace

std::vector<StumpSetCandidate> findBatterStumps(const cv::Mat& image, const Pitch& pitch, int max_candidates) {
  // The phone is behind the bowler's stumps, looking down the pitch: the
  // batter's stumps are in the middle part of the picture. Search only there.
  const int W = image.cols, Hh = image.rows;
  const cv::Rect roi(static_cast<int>(0.2 * W), static_cast<int>(0.05 * Hh), static_cast<int>(0.6 * W),
                     static_cast<int>(0.9 * Hh));
  const cv::Mat g = prepareGray(image(roi));
  const int w = W;  // scales follow the full picture
  const Geometry geo = stumpGeometry(pitch);
  RidgeCache maps;
  std::map<std::pair<int, int>, Raw> cells;
  // Bar spacing from ~4 px (long pitch, wide lens) to ~30 px (short pitch) at 720p.
  const double s_min = std::max(2.5, 3.0 * w / 1280.0), s_max = 30.0 * w / 1280.0;
  for (double s = s_min; s <= s_max; s += (s < 8 ? 0.5 : 1.0)) {
    const RidgeMap& rm = ridgeFor(maps, g, s, geo.bar_frac, 8);
    const int H = static_cast<int>(std::lround(s * geo.ratio_h));
    const int Q = std::max(2, H / 4);
    const int ylo = H + Q, yhi = g.rows - Q - 1;
    const int xlo = 2, xhi = static_cast<int>(g.cols - 2 - 2 * s);
    const bool use_gap = s >= 3.0;
    const double valley_min = s >= 4.0 ? 0.3 : 0.15;  // gaps of 2 px or less are blurred
    for (int pol = 0; pol < 2; ++pol) {
      const cv::Mat& ii = rm.ii[pol];
      for (int yb = ylo; yb <= yhi; ++yb) {
        const int yt = yb - H;
        for (int x0 = xlo; x0 <= xhi; ++x0) {
          const double b0 = colSum(ii, x0, yt, yb);
          if (b0 < kMinContrast * H) continue;
          const int x1 = static_cast<int>(std::lround(x0 + s)), x2 = static_cast<int>(std::lround(x0 + 2 * s));
          const double b1 = colSum(ii, x1, yt, yb);
          if (b1 < kMinContrast * H) continue;
          const double b2 = colSum(ii, x2, yt, yb);
          if (b2 < kMinContrast * H) continue;
          // Weakest bar (all three must be there) and the average (bars vs gaps of the same pattern).
          const double m = 0.5 * (std::min({b0, b1, b2}) + (b0 + b1 + b2) / 3.0) / H;
          double gap = 0;
          if (use_gap) {
            const int g0 = static_cast<int>(std::lround(x0 + 0.5 * s)), g1 = static_cast<int>(std::lround(x0 + 1.5 * s));
            gap = (colSum(rm.ii_raw[pol], g0, yt, yb) + colSum(rm.ii_raw[pol], g1, yt, yb)) / (2.0 * H);
            // Between real stumps the background shows: a valley. Inside a pad
            // or a trouser leg there is none.
            const double valley = std::min(colSum(rm.ii[1 - pol], g0, yt, yb), colSum(rm.ii[1 - pol], g1, yt, yb)) / H;
            if (valley < valley_min * m) continue;
          }
          double above = 0, below = 0;
          for (int x : {x0, x1, x2}) {
            above = std::max(above, colSum(ii, x, yt - Q, yt) / Q);
            below = std::max(below, colSum(ii, x, yb, yb + Q) / Q);
          }
          const double score = m - 0.7 * gap - 0.5 * above - 0.5 * below;
          if (score < kMinContrast) continue;
          const std::pair<int, int> key{(x0 / 16) * 2 + pol, yb / 16};
          auto it = cells.find(key);
          if (it == cells.end() || it->second.score < score) cells[key] = {score, pol, s, x0, yb, H};
        }
      }
    }
  }

  std::vector<StumpSetCandidate> out;
  for (const Raw& r : pickBest(cells, max_candidates)) {
    if (std::getenv("PE_AUTOCAL_DEBUG")) {
      std::fprintf(stderr, "batter cand score %.1f pol %d s %.1f x0 %d yb %d H %d\n", r.score, r.pol, r.s, r.x0, r.y0, r.H);
    }
    const RidgeMap& rm = ridgeFor(maps, g, r.s, geo.bar_frac, 8);
    const int k = rm.k;
    cv::Mat ridge = rm.signed_ridge;
    if (r.pol == 1) ridge = -ridge;
    std::vector<int> xs = {r.x0, static_cast<int>(std::lround(r.x0 + r.s)),
                           static_cast<int>(std::lround(r.x0 + 2 * r.s))};
    const RefinedSet rs = refineSet(g, ridge, xs, r.y0 - r.H, r.y0, r.s, k, r.pol == 0 ? 1.0 : -1.0, geo.ratio_h);
    if (!rs.ok || !rs.base_visible) continue;
    const double s_meas = 0.25 * (rs.xr_top + rs.xr_base - rs.xl_top - rs.xl_base);
    if (std::abs(s_meas - r.s) > 0.25 * r.s + 0.5) continue;
    // As tall as the stump size says (not a short fence or a pattern).
    const double len = rs.y_base - rs.y_top, len_exp = s_meas * geo.ratio_h;
    if (len < 0.7 * len_exp || len > 1.3 * len_exp) continue;
    StumpSetCandidate c;
    c.score = r.score;
    c.light = r.pol == 0;
    // Upper and lower halves are measured separately (camera roll): extend to the ends.
    const double h = rs.y_base - rs.y_top;
    auto at = [&](double x_top, double x_base, double y) {
      return x_top + (x_base - x_top) * ((y - rs.y_top) / std::max(1.0, h) - 0.25) / 0.5;
    };
    const Vec2 off(roi.x, roi.y);
    c.taps.push_back({"batter_left_base", off + Vec2(at(rs.xl_top, rs.xl_base, rs.y_base), rs.y_base)});
    c.taps.push_back({"batter_left_top", off + Vec2(at(rs.xl_top, rs.xl_base, rs.y_top), rs.y_top)});
    c.taps.push_back({"batter_right_base", off + Vec2(at(rs.xr_top, rs.xr_base, rs.y_base), rs.y_base)});
    c.taps.push_back({"batter_right_top", off + Vec2(at(rs.xr_top, rs.xr_base, rs.y_top), rs.y_top)});
    out.push_back(std::move(c));
  }
  return out;
}

std::vector<StumpSetCandidate> findBowlerStumps(const cv::Mat& image, const Pitch& pitch, int max_candidates) {
  const cv::Mat g_full = prepareGray(image);
  constexpr int kScale = 4;
  cv::Mat g;
  cv::resize(g_full, g, cv::Size(g_full.cols / kScale, g_full.rows / kScale), 0, 0, cv::INTER_AREA);
  const int w = g.cols, h = g.rows;
  const Geometry geo = stumpGeometry(pitch);
  RidgeCache maps;
  std::map<std::pair<int, int>, Raw> cells;
  // Bar spacing (downscaled px): the camera 0.5-5 m behind the stumps.
  const double s_min = 4.0 * w / 320.0, s_max = 50.0 * w / 320.0;
  for (double s = s_min; s <= s_max; s += 1.0) {
    const RidgeMap& rm = ridgeFor(maps, g, s, geo.bar_frac, 20);
    const int H = static_cast<int>(std::lround(s * geo.ratio_h));
    const int xlo = static_cast<int>(0.03 * w), xhi = static_cast<int>(0.97 * w - 2 * s);
    for (int pol = 0; pol < 2; ++pol) {
      const cv::Mat& ii = rm.ii[pol];
      for (int yt = static_cast<int>(0.25 * h); yt <= h - 6; ++yt) {
        const int V = std::min(H, h - yt);  // visible length: usually cut by the picture
        if (V < std::max(6, H / 6)) continue;
        const int Q = std::max(2, std::min(H / 6, yt));
        const bool base_visible = yt + H + 2 < h;
        for (int x0 = xlo; x0 <= xhi; ++x0) {
          const double b0 = colSum(ii, x0, yt, yt + V);
          if (b0 < kMinContrast * V) continue;
          const int x1 = static_cast<int>(std::lround(x0 + s)), x2 = static_cast<int>(std::lround(x0 + 2 * s));
          const double b1 = colSum(ii, x1, yt, yt + V);
          if (b1 < kMinContrast * V) continue;
          const double b2 = colSum(ii, x2, yt, yt + V);
          if (b2 < kMinContrast * V) continue;
          const double m = 0.5 * (std::min({b0, b1, b2}) + (b0 + b1 + b2) / 3.0) / V;
          const int g0 = static_cast<int>(std::lround(x0 + 0.5 * s)), g1 = static_cast<int>(std::lround(x0 + 1.5 * s));
          const double gap = (colSum(rm.ii_raw[pol], g0, yt, yt + V) + colSum(rm.ii_raw[pol], g1, yt, yt + V)) / (2.0 * V);
          const double valley =
              std::min(colSum(rm.ii[1 - pol], g0, yt, yt + V), colSum(rm.ii[1 - pol], g1, yt, yt + V)) / V;
          if (valley < 0.3 * m) continue;
          double above = 0, below = 0;
          for (int x : {x0, x1, x2}) {
            above = std::max(above, colSum(ii, x, yt - Q, yt) / Q);
            if (base_visible) below = std::max(below, colSum(ii, x, yt + H, yt + H + Q) / Q);
          }
          const double score = m - 0.7 * gap - 0.5 * above - 0.5 * below;
          if (score < kMinContrast) continue;
          const std::pair<int, int> key{(x0 / 8) * 2 + pol, yt / 8};
          auto it = cells.find(key);
          if (it == cells.end() || it->second.score < score) cells[key] = {score, pol, s, x0, yt, H};
        }
      }
    }
  }

  std::vector<StumpSetCandidate> out;
  for (const Raw& r : pickBest(cells, max_candidates)) {
    // Refine at full resolution, on a crop around the downscaled find.
    const double s_full = r.s * kScale;
    const int H = r.H * kScale;
    const int yt0 = r.y0 * kScale + kScale / 2;
    auto full_x = [&](double xs) { return static_cast<int>(std::lround(xs * kScale + 0.5 * (kScale - 1))); };
    const int margin = static_cast<int>(1.5 * s_full) + 8;
    const int cx0 = std::max(0, full_x(r.x0) - margin), cx1 = std::min(g_full.cols, full_x(r.x0 + 2 * r.s) + margin);
    const int cy0 = std::max(0, yt0 - H / 3 - 8);
    const cv::Rect crop(cx0, cy0, cx1 - cx0, g_full.rows - cy0);
    const cv::Mat gc = g_full(crop);
    RidgeCache cache;
    const RidgeMap& rm = ridgeFor(cache, gc, s_full, geo.bar_frac, 80);
    const int k = rm.k;
    cv::Mat ridge = rm.signed_ridge;
    if (r.pol == 1) ridge = -ridge;
    std::vector<int> xs = {full_x(r.x0) - cx0, full_x(r.x0 + r.s) - cx0, full_x(r.x0 + 2 * r.s) - cx0};
    RefinedSet rs = refineSet(gc, ridge, xs, yt0 - cy0, yt0 - cy0 + H, s_full, k, r.pol == 0 ? 1.0 : -1.0, geo.ratio_h);
    rs.xl_top += cx0;
    rs.xr_top += cx0;
    rs.xl_base += cx0;
    rs.xr_base += cx0;
    rs.y_top += cy0;
    rs.y_base += cy0;
    if (!rs.ok) continue;
    StumpSetCandidate c;
    c.score = r.score;
    c.light = r.pol == 0;
    c.base_visible = rs.base_visible;
    // Near stumps lean in the picture (perspective): use the columns next to each end.
    c.taps.push_back({"bowler_left_top", Vec2(rs.xl_top, rs.y_top)});
    c.taps.push_back({"bowler_right_top", Vec2(rs.xr_top, rs.y_top)});
    if (rs.base_visible) {
      c.taps.push_back({"bowler_left_base", Vec2(rs.xl_base, rs.y_base)});
      c.taps.push_back({"bowler_right_base", Vec2(rs.xr_base, rs.y_base)});
    }
    out.push_back(std::move(c));
  }
  return out;
}

namespace {

// Pitch length from the pictures is used without asking only if it is this good.
constexpr double kLengthSigmaOk = 0.4;

bool plausible(const CalibrationResult& r, bool length_estimated) {
  if (!r.ok || r.rms_px > 2.5) return false;
  const Vec3 c = r.camera.center();
  if (c.z() < 0.2 || c.z() > 4.0 || c.y() > 0.5 || c.y() < -10.0 || std::abs(c.x()) > 2.0) return false;
  if (length_estimated && (r.pitch_length < 5.0 || r.pitch_length > 30.0)) return false;
  return true;
}

}  // namespace

AutoCalibration autoCalibrate(const cv::Mat& image, const CalibrationInput& in) {
  AutoCalibration a;
  const auto batter = findBatterStumps(image, in.pitch, 4);
  if (batter.empty()) {
    a.message = "Could not find the batter's stumps. Tap them yourself.";
    return a;
  }
  // No stumps at the bowler's end: do not look for them (anything found would be wrong).
  const auto bowler = in.bowler_stumps ? findBowlerStumps(image, in.pitch, 4) : std::vector<StumpSetCandidate>{};
  const double hfov = in.hfov_deg.value_or(65.0);
  const double f_prior = 0.5 * in.width / std::tan(0.5 * hfov * kPi / 180.0);

  // Try the batter candidates best first; for each, the bowler candidates that
  // give a consistent camera. Wrong finds rarely fit together.
  for (const StumpSetCandidate& bc : batter) {
    // The stump spacing in pixels says how far away they are: must look like a pitch.
    const double spacing_px = 0.5 * std::abs(bc.taps[2].uv.x() + bc.taps[3].uv.x() - bc.taps[0].uv.x() - bc.taps[1].uv.x()) / 2.0;
    const double dist = f_prior * in.pitch.outerStumpX() / std::max(1.0, spacing_px);
    if (dist < 4.0 || dist > 40.0) continue;
    for (const StumpSetCandidate& wc : bowler) {
      CalibrationInput ci = in;
      ci.taps = bc.taps;
      ci.taps.insert(ci.taps.end(), wc.taps.begin(), wc.taps.end());
      const CalibrationResult r = calibrateFromTaps(ci);
      if (!plausible(r, !in.length_known)) continue;
      a.ok = true;
      a.batter_found = a.bowler_found = true;
      a.taps = ci.taps;
      a.calibration = r;
      a.length_estimated = !in.length_known;
      break;
    }
    if (a.ok) break;
    if (!a.batter_found) {
      // Remember the best batter-only find in case no bowler set fits.
      a.batter_found = true;
      a.taps = bc.taps;
    }
  }
  if (!a.batter_found) {
    a.message = "Could not find the batter's stumps. Tap them yourself.";
    return a;
  }
  if (a.ok) {
    std::ostringstream m;
    m.setf(std::ios::fixed);
    m.precision(1);
    m << "Found both sets of stumps.";
    if (a.length_estimated && a.calibration.pitch_length_sigma <= kLengthSigmaOk) {
      m << " Pitch length measured: " << a.calibration.pitch_length << " m (+/- "
        << std::max(0.1, a.calibration.pitch_length_sigma) << " m).";
    } else if (a.length_estimated) {
      // Not precise enough for LBW: ask for a tape measurement.
      a.length_uncertain = true;
      m << " The pitch looks about " << a.calibration.pitch_length << " m long, but that is not exact (+/- "
        << a.calibration.pitch_length_sigma << " m). Measure it and enter it.";
    }
    a.message = m.str();
    return a;
  }
  // Batter's stumps only: the calibration needs the measurements instead.
  CalibrationInput ci = in;
  ci.taps = a.taps;
  if (in.length_known && in.camera_height_m && in.camera_distance_m) {
    const CalibrationResult r = calibrateFromTaps(ci);
    if (plausible(r, false)) {
      a.ok = true;
      a.calibration = r;
      a.message = in.bowler_stumps
                      ? "Found the batter's stumps. The bowler's stumps are not in the picture, so your measurements "
                        "are used."
                      : "Found the batter's stumps. Your pitch length and camera measurements are used.";
      return a;
    }
  }
  if (!in.bowler_stumps) {
    a.message = "Found the batter's stumps. Enter the pitch length (bowling mark to the batter's stumps), the camera "
                "height and its distance behind the bowling mark.";
    return a;
  }
  a.message = in.length_known
                  ? "Found the batter's stumps, but not the bowler's. Enter the camera height and its distance "
                    "behind the bowler's stumps, or tap the bowler's stump tops."
                  : "Found the batter's stumps, but not the bowler's, so the pitch length can't be measured. "
                    "Enter the pitch length, the camera height and its distance behind the bowler's stumps.";
  return a;
}

nlohmann::json autoCalibrationToJson(const AutoCalibration& a) {
  nlohmann::json taps = nlohmann::json::object();
  for (const TapPoint& t : a.taps) taps[t.name] = {std::round(t.uv.x() * 100) / 100, std::round(t.uv.y() * 100) / 100};
  nlohmann::json j = {{"ok", a.ok},
                      {"message", a.message},
                      {"taps", taps},
                      {"batter_found", a.batter_found},
                      {"bowler_found", a.bowler_found},
                      {"length_estimated", a.length_estimated},
                      {"length_uncertain", a.length_uncertain}};
  if (a.ok && a.calibration.ok) {
    j["calibration"] = calibrationToJson(a.calibration);
    j["pitch_length_m"] = a.calibration.pitch_length;
    j["pitch_length_sigma_m"] = a.calibration.pitch_length_sigma;
  }
  return j;
}

}  // namespace pe
