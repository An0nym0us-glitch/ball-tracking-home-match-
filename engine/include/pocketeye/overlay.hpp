#pragma once

#include <string>
#include <vector>

#include "pocketeye/analysis.hpp"

namespace pe {

// Re-reads the video and writes a replay with the tracked ball, the fitted path,
// bounce/impact markers, the projection to the stumps and the result card.
bool writeOverlayVideo(const std::string& video_in, const std::vector<double>& timestamps,
                       const std::vector<DeliveryResult>& deliveries, const AnalysisConfig& cfg,
                       const std::string& video_out, std::string* error);

}  // namespace pe
