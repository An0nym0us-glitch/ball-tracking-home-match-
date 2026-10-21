#include "pocketeye/result_json.hpp"

#include <cmath>

namespace pe {

using nlohmann::json;

namespace {

json est(const Estimate& e, int digits = 1) {
  if (!e.valid) return nullptr;
  const double s = std::pow(10.0, digits);
  auto r = [s](double v) { return std::round(v * s) / s; };
  return {{"value", r(e.value)}, {"lo", r(e.lo)}, {"hi", r(e.hi)}};
}

json vec(const Vec3& v) {
  if (!v.allFinite()) return nullptr;
  return {std::round(v.x() * 1000) / 1000, std::round(v.y() * 1000) / 1000,
          std::round(v.z() * 1000) / 1000};
}

json pathJson(const std::vector<std::pair<double, Vec3>>& path, const Camera& cam) {
  json a = json::array();
  for (const auto& [t, p] : path) {
    const Vec2 uv = cam.project(p);
    a.push_back({{"t", t}, {"p", vec(p)}, {"uv", {std::round(uv.x() * 10) / 10, std::round(uv.y() * 10) / 10}}});
  }
  return a;
}

json component(const LbwComponent& c) { return {{"status", c.status}, {"verdict", verdictName(c.verdict)}}; }

}  // namespace

json deliveryToJson(const DeliveryResult& r, const AnalysisConfig& cfg, bool include_observations) {
  json j;
  j["valid"] = r.valid;
  if (!r.valid) j["reject_reason"] = r.reject_reason;
  j["frames"] = {r.frame_first, r.frame_last};
  j["time_s"] = {r.t_first, r.t_last};

  json lbw;
  lbw["applicable"] = r.lbw_applicable;
  lbw["impact_kind"] = impactKindName(r.impact);
  if (r.valid) {
    lbw["decision"] = r.lbw_applicable ? json(r.lbw.decision) : json(nullptr);
    lbw["pitching"] = component(r.lbw.pitching);
    lbw["impact"] = r.lbw_applicable ? component(r.lbw.impact) : json(nullptr);
    lbw["wickets"] = component(r.lbw.wickets);
    lbw["p_hitting"] = std::round(r.lbw_input.p_hit * 1000) / 1000;
    lbw["p_impact_in_line"] =
        std::round((1.0 - r.lbw_input.p_impact_outside_leg - r.lbw_input.p_impact_outside_off) * 1000) / 1000;
    lbw["p_pitched_outside_leg"] = std::round(r.lbw_input.p_pitch_outside_leg * 1000) / 1000;
    lbw["no_shot_offered"] = r.lbw_input.no_shot_offered;
    lbw["projection_m"] = std::isfinite(r.projection_m) ? json(std::round(r.projection_m * 100) / 100) : json(nullptr);
  }
  j["lbw"] = lbw;

  if (r.valid) {
    j["has_bounce"] = r.has_bounce;
    j["length_zone"] = r.length_zone;
    j["line_zone"] = r.line_zone;
    j["metrics"] = {
        {"release_speed_kmh", est(r.release_speed_kmh)},
        {"average_speed_kmh", est(r.average_speed_kmh)},
        {"swing_cm", est(r.swing_cm)},
        {"swing_type", r.swing_type},
        {"turn_deg", est(r.turn_deg, 2)},
        {"turn_type", r.turn_type},
        {"bounce_line_cm", est(r.bounce_x_off_cm)},
        {"bounce_length_m", est(r.length_m, 2)},
        {"height_at_stumps_cm", est(r.stumps_height_cm)},
        {"line_at_stumps_cm", est(r.stumps_x_off_cm)},
        {"impact_line_cm", est(r.impact_x_off_cm)},
        {"impact_height_cm", est(r.impact_height_cm)},
    };
    j["sign_convention"] = "lateral values: + = batter's off side, - = leg side (" +
                           std::string(cfg.batter == Hand::Right ? "right" : "left") + "-handed batter)";
    j["impact_point_m"] = vec(r.impact_point);
    j["t_bounce"] = std::isfinite(r.t_bounce) ? json(r.t_bounce) : json(nullptr);
    j["t_impact"] = std::isfinite(r.t_impact) ? json(r.t_impact) : json(nullptr);
    j["path"] = pathJson(r.path, cfg.camera);
    j["projection"] = pathJson(r.projection, cfg.camera);
  }
  j["quality"] = {{"rms_px", std::round(r.rms_px * 100) / 100},
                  {"n_pre_bounce", r.n_pre},
                  {"n_post_bounce", r.n_post},
                  {"n_after_impact", r.n_after_impact},
                  {"n_outliers", r.n_outliers},
                  {"warnings", r.warnings}};
  if (include_observations) {
    json o = json::array();
    for (size_t i = 0; i < r.observations.size(); ++i) {
      const Observation& ob = r.observations[i];
      o.push_back({{"t", ob.t},
                   {"uv", {std::round(ob.uv.x() * 100) / 100, std::round(ob.uv.y() * 100) / 100}},
                   {"r", std::round(ob.radius_px * 100) / 100},
                   {"label", r.labels.empty() ? 0 : r.labels[i]}});
    }
    j["observations"] = o;
  }
  json t = json::object();
  for (const auto& [k, v] : r.timings_ms) t[k] = std::round(v * 100) / 100;
  j["timings_ms"] = t;
  return j;
}

}  // namespace pe
