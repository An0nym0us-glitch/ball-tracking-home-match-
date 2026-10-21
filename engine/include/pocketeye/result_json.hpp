#pragma once

#include <nlohmann/json.hpp>

#include "pocketeye/analysis.hpp"

namespace pe {

nlohmann::json deliveryToJson(const DeliveryResult& r, const AnalysisConfig& cfg,
                              bool include_observations = true);

}  // namespace pe
