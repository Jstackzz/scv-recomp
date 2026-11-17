#pragma once
// The player's own downloadable content, see dlc.cpp.
#include <filesystem>

namespace sc5::dlc {

// copies the packages in `dlc_dir` (SC5_DLC_DIR, game\dlc) where the SDK's content manager finds them under `saves`
// (SC5_USER_DIR), with their records, and takes out what is not among them; at startup
void Install(const std::filesystem::path& dlc_dir, const std::filesystem::path& saves);

}  // namespace sc5::dlc
