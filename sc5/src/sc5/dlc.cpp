// The player's own downloadable content (docs\PROTOCOL.md 3.7, docs\GAME.md "Unlocks and downloadable content"):
// Build game (or tools\py\x360.py dlc-install) lays each of the player's own Xbox 360 DLC packages out as
// <dlc>\<package>\ (its files) and <dlc>\<package>.header (the package's header). The SDK's content manager finds
// marketplace content under the saves, Xenia's way: <saves>\0000000000000000\4E4D083D\00000002\<package>\, with a
// record of its own beside it in ...\4E4D083D\Headers\00000002\<package>.header (the content record, 0x148 bytes, then
// the license mask). Install puts the player's packages there and writes the records (the package's display name,
// every license bit: the player owns it), and takes out what is no longer in <dlc>: only packages the player has.
// A package's files go there as hard links to the ones in <dlc> (a costume pack's data is a hundred megabytes, and every
// online game starts from fresh saves of its own), copies only where the two folders are on different drives.
// Nothing here unlocks paid content any other way.

#include "sc5/dlc.h"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <cstring>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>

#include "sc5/game.h"

namespace sc5::dlc {
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kTitleId = 0x4E4D083D;
constexpr uint32_t kMarketplaceContent = 0x00000002;

// the game's DLC manager (docs\GAME.md, "Unlocks and downloadable content"): its state at +8 (7 to 11 once the
// packages are read), at +0x28 its 30 package objects of 344 bytes (+2 the entries' count, +0x1C the entries, 48
// bytes each, +0xD their flags: 0x80 the entry's file is there), their count at +0x2C
constexpr uint32_t kDlcManager = 0x8276F430;
constexpr uint32_t kPackageSize = 344, kEntrySize = 48;
constexpr uint8_t kEntryHasFile = 0x80;
// a created character's record: 4 break levels from +1128, 288 bytes each, 34 parts of 8 bytes (ModelData first)
constexpr uint32_t kBreakLevels = 1128, kBreakLevelSize = 288, kBreakLevelCount = 4, kPartsPerLevel = 34;

uint8_t ReadU8(uint32_t a) { return uint8_t(game::ReadU32(a & ~3u) >> (24 - 8 * (a & 3))); }
uint16_t ReadU16(uint32_t a) { return uint16_t(ReadU8(a) << 8 | ReadU8(a + 1)); }

void Be32(std::vector<uint8_t>& out, size_t at, uint32_t v) {
  out[at] = uint8_t(v >> 24);
  out[at + 1] = uint8_t(v >> 16);
  out[at + 2] = uint8_t(v >> 8);
  out[at + 3] = uint8_t(v);
}

// the SDK's record for a package (XCONTENT_AGGREGATE_DATA, then the license mask)
std::vector<uint8_t> Record(const std::string& name, const std::vector<uint8_t>& stfs_header) {
  std::vector<uint8_t> r(0x148 + 4, 0);
  Be32(r, 0x000, 1);  // the hard drive (the SDK's DummyDeviceId::HDD)
  Be32(r, 0x004, kMarketplaceContent);
  // the display name: the package's own (UTF-16BE at 0x411 of an STFS header), kept as it is
  if (stfs_header.size() >= 0x411 + 0x100)
    for (size_t i = 0; i + 1 < 0x100; i += 2) {
      r[0x008 + i] = stfs_header[0x411 + i];
      r[0x008 + i + 1] = stfs_header[0x411 + i + 1];
    }
  std::memcpy(r.data() + 0x108, name.data(), std::min<size_t>(name.size(), 42));
  // xuid 0 (marketplace content is everybody's on the console it is on), the title
  Be32(r, 0x140, kTitleId);
  // the license mask, as the SDK reads it (host order): every bit
  const uint32_t license = 0xFFFFFFFFu;
  std::memcpy(r.data() + 0x148, &license, 4);
  return r;
}

// a package's file in the saves: a hard link to the one in <dlc>, or a copy (another drive); false: neither could be
// made. One already there stays when it is that file (a link) or a copy of it as it is now (size and time).
bool Place(const fs::path& from, const fs::path& to, bool* copied) {
  std::error_code ec;
  if (fs::equivalent(from, to, ec)) return true;
  ec.clear();
  if (fs::exists(to, ec) && fs::file_size(from, ec) == fs::file_size(to, ec) && !ec &&
      fs::last_write_time(from, ec) == fs::last_write_time(to, ec) && !ec)
    return true;
  ec.clear();
  fs::remove(to, ec);
  ec.clear();
  fs::create_hard_link(from, to, ec);
  if (!ec) return true;
  ec.clear();
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  if (ec) return false;
  fs::last_write_time(to, fs::last_write_time(from, ec), ec);  // so the next start knows this copy
  *copied = true;
  return true;
}

}  // namespace

void Install(const fs::path& dlc_dir, const fs::path& saves) {
  std::error_code ec;
  const fs::path content = saves / "0000000000000000" / "4E4D083D" / "00000002";
  const fs::path headers = saves / "0000000000000000" / "4E4D083D" / "Headers" / "00000002";
  std::vector<std::string> mine;
  if (!dlc_dir.empty() && fs::is_directory(dlc_dir, ec)) {
    for (const auto& entry : fs::directory_iterator(dlc_dir, ec)) {
      if (!entry.is_directory()) continue;
      const std::string name = entry.path().filename().string();
      const fs::path header_file = dlc_dir / (name + ".header");
      if (!fs::exists(header_file, ec)) {
        REXLOG_WARN("sc5 dlc: {} has no header beside it, left out", name);
        continue;
      }
      std::vector<uint8_t> stfs;
      {
        std::ifstream in(header_file, std::ios::binary);
        stfs.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      }
      // the package's files, and none that it no longer has
      size_t files = 0, copies = 0, failed = 0;
      std::vector<fs::path> placed;
      for (const auto& f : fs::recursive_directory_iterator(entry.path(), ec)) {
        if (!f.is_regular_file()) continue;
        const fs::path to = content / name / fs::relative(f.path(), entry.path(), ec);
        fs::create_directories(to.parent_path(), ec);
        bool copied = false;
        if (!Place(f.path(), to, &copied)) failed++;
        copies += copied;
        placed.push_back(to.lexically_normal());
        files++;
      }
      std::vector<fs::path> stale;
      for (const auto& f : fs::recursive_directory_iterator(content / name, ec))
        if (f.is_regular_file() && std::find(placed.begin(), placed.end(), f.path().lexically_normal()) == placed.end())
          stale.push_back(f.path());
      for (const fs::path& f : stale) fs::remove(f, ec);
      if (copies) REXLOG_INFO("sc5 dlc: {}: {} files copied (the saves are on another drive than the DLC)", name, copies);
      if (failed) REXLOG_WARN("sc5 dlc: {}: {} files could not be put in the saves (is the disk full?)", name, failed);
      fs::create_directories(headers, ec);
      const std::vector<uint8_t> record = Record(name, stfs);
      if (FILE* out = std::fopen((headers / (name + ".header")).string().c_str(), "wb")) {
        std::fwrite(record.data(), 1, record.size(), out);
        std::fclose(out);
      }
      std::string display;
      for (size_t i = 0x411; i + 1 < stfs.size() && i < 0x411 + 0x80; i += 2) {
        const char16_t c = char16_t(stfs[i] << 8 | stfs[i + 1]);
        if (!c) break;
        display += c < 0x80 ? char(c) : '?';
      }
      REXLOG_INFO("sc5 dlc: {} ({}), {} files", display, name, files);
      mine.push_back(name);
    }
  }
  // what is not among the player's packages (any more) goes
  if (fs::is_directory(content, ec))
    for (const auto& entry : fs::directory_iterator(content, ec)) {
      const std::string name = entry.path().filename().string();
      if (std::find(mine.begin(), mine.end(), name) != mine.end()) continue;
      fs::remove_all(entry.path(), ec);
      fs::remove(headers / (name + ".header"), ec);
      REXLOG_INFO("sc5 dlc: {} is not among the player's packages: taken out", name);
    }
}

bool ModelsInstalled(uint32_t record) {
  const uint32_t state = game::ReadU32(kDlcManager + 8);
  const uint32_t packages = game::ReadU32(kDlcManager + 0x28), count = game::ReadU32(kDlcManager + 0x2C);
  if (state < 7 || state > 11 || !packages || count > 30) return false;
  for (uint32_t level = 0; level < kBreakLevelCount; level++)
    for (uint32_t p = 0; p < kPartsPerLevel; p++) {
      const uint32_t model = game::ReadU32(record + kBreakLevels + level * kBreakLevelSize + p * 8);
      if (model >> 24 != 0x7F) continue;
      const uint32_t group = (model >> 16) & 0xFF, slot = (model >> 8) & 0xFF;
      if (group >= count) return false;
      const uint32_t package = packages + group * kPackageSize;
      const uint32_t entries = game::ReadU32(package + 0x1C);
      if (!entries || slot >= ReadU16(package + 2) || !(ReadU8(entries + slot * kEntrySize + 0xD) & kEntryHasFile))
        return false;
    }
  return true;
}

}  // namespace sc5::dlc

// [[midasm_hook]] Sc5DlcFighterModels (config\sc5_codegen.toml), in sub_82147C70, which sets up a fighter for a fight
// from a creation manager entry (r4; the record first): r11 is the entry's "wears downloadable models" flag (+0x34D8,
// the record's byte 15, which Creation sets for any 0x7F model), and with it set the game swaps the character for a
// template of its style in plain clothes (sub_822958C8 mode 1), its own way out when such a model cannot be loaded. It
// did so for every such character, the costume packs' data installed or not; so when every downloadable model the
// character wears has its data on this PC the flag is taken as clear and the character fights as created. Both games
// of a match decide alike: a created character with downloadable parts is picked only when the two games' downloadable
// parts are the same (creation.cpp's fingerprint).
void Sc5DlcFighterModels(PPCRegister& r4, PPCRegister& r11) {
  if (r11.u32 && r4.u32 && sc5::dlc::ModelsInstalled(r4.u32)) r11.u64 = 0;
}
