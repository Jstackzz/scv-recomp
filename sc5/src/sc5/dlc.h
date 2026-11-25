#pragma once
// The player's own downloadable content, see dlc.cpp.
#include <cstdint>
#include <filesystem>

namespace sc5::dlc {

// copies the packages in `dlc_dir` (SC5_DLC_DIR, game\dlc) where the SDK's content manager finds them under `saves`
// (SC5_USER_DIR), with their records, and takes out what is not among them; at startup
void Install(const std::filesystem::path& dlc_dir, const std::filesystem::path& saves);

// whether this PC has the file of a model (ModelData) a created character wears: any model of the disc; a downloadable
// one (0x7Fggss..: package group gg, entry ss) when the game's DLC manager has read that package and the entry's file
// is there. False while the manager has not read the packages yet.
bool ModelFileInstalled(uint32_t model);

// a byte of guest memory
uint8_t ReadByte(uint32_t guest);

}  // namespace sc5::dlc
