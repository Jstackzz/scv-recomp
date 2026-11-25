#pragma once
// The player's own downloadable content, see dlc.cpp.
#include <cstdint>
#include <filesystem>

#include <rex/ppc/context.h>

namespace sc5::dlc {

// copies the packages in `dlc_dir` (SC5_DLC_DIR, game\dlc) where the SDK's content manager finds them under `saves`
// (SC5_USER_DIR), with their records, and takes out what is not among them; at startup
void Install(const std::filesystem::path& dlc_dir, const std::filesystem::path& saves);

// whether every downloadable model (ModelData 0x7Fggss..: package group gg, entry ss) that the created character's
// record at guest address `record` wears, at any break level, has its data installed (the game's DLC manager: the
// package's entry has a file); false while the manager has not read the packages yet
bool ModelsInstalled(uint32_t record);

// the game asks for a file of an archive (its size, before it reads it): the patch text tells the game's language
void OnFileAsked(int archive, int file);

// before the game gives a string of text table `table`: the downloadable parts' names (table 55, which title update 1
// never registers) from the installed packages' patch text, the first time it is asked for
void BeforeText(PPCContext& ctx, uint8_t* base, uint32_t table);

}  // namespace sc5::dlc
