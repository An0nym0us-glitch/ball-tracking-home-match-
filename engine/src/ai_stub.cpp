// Builds without the AI assist (PE_WITH_AI off, e.g. Android): AiAssist exists
// so that the engine links, but never runs (the engine does not create it).
#include "pocketeye/ai.hpp"

namespace pe {

std::vector<AiBox> decodeYolox(const cv::Mat&, int, int, float) { return {}; }

struct AiAssist::Net {};

AiAssist::AiAssist(AiConfig cfg) : cfg_(std::move(cfg)) { status_.error = "this build has no AI assist"; }
AiAssist::~AiAssist() = default;
bool AiAssist::wantsPeople(double) const { return false; }
void AiAssist::submitPeople(double, cv::Mat, double) {}
void AiAssist::submitCheck(const AiCheck&, cv::Mat) {}
bool AiAssist::people(std::vector<AiBox>&, double&) const { return false; }
std::vector<AiCheckResult> AiAssist::takeChecks() { return {}; }
AiStatus AiAssist::status() const { return status_; }
std::vector<AiBox> AiAssist::detectNow(const cv::Mat&, int, float) { return {}; }

}  // namespace pe
