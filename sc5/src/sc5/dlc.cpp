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
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "sc5/frame.h"
#include "sc5/game.h"

REX_EXTERN(sub_82348660);  // registers a text table (r3 its number, r4 its data, unscrambled in place)
REX_EXTERN(sub_82348780);  // a text table's data (r3 its number): 0 when the game does not have it

namespace sc5::dlc {
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kTitleId = 0x4E4D083D;
constexpr uint32_t kMarketplaceContent = 0x00000002;

// the game's DLC manager (docs\GAME.md, "Unlocks and downloadable content"): its state at +8 (7 to 11 once the
// packages are read), at +0x28 its 30 package objects of 344 bytes (+2 the entries' count, +0x1C the entries, 48
// bytes each, +0xD their flags: 0x80 the entry's file is there), their count at +0x2C
constexpr uint32_t kDlcManager = 0x82782F30;
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

// --- the names of the parts the DLC adds (text table 55) ---
// The game's texts are tables of strings by number (sub_82348660 registers one: r3 its number, r4 its data, which it
// unscrambles in place; sub_82347670 gives string r4 of table r3, an empty one for a table it does not have). Title
// update 1 registers 0 to 53 (the sections of text 424) and 54 (patch 0 to 7 by language, sub_8215CCC8); the parts the
// costume packs add are named in table 55, which only a later title update registers: patch 8 to 15 by language,
// which the packages replace (the highest costume group's carries them: docs\GAME.md). So, the first time the game
// asks for table 55 without having it, the packages' copy of patch 8 + the language is registered as 55.
constexpr int kArchivePatch = 11, kArchiveDlc = 14;
constexpr uint32_t kDlcNamesTable = 55;
constexpr int kNamesFirstFile = 8;
std::mutex g_names_lock;
std::map<int, fs::path> g_names_files;  // patch file (8..15) -> the installed package's file that replaces it
int g_language = 0;                     // the patch file the game took for table 54 (0 to 7)
std::vector<uint8_t> g_names_data;      // the file as it is (scrambled), read once
uint32_t g_names_guest = 0;             // its copy in guest memory (the game unscrambles it in place)
bool g_names_failed = false;

uint16_t Be16(const std::vector<uint8_t>& d, size_t at) { return uint16_t(d[at] << 8 | d[at + 1]); }
uint32_t Be32At(const std::vector<uint8_t>& d, size_t at) {
  return uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3];
}

// a package's group, as the game's DLC manager takes it: the first "DLCnn" in its display name; -1 none
int GroupOf(const std::string& display) {
  for (size_t i = 0; i + 5 <= display.size(); i++)
    if (display.compare(i, 3, "DLC") == 0 && std::isdigit(uint8_t(display[i + 3])) &&
        std::isdigit(uint8_t(display[i + 4])))
      return (display[i + 3] - '0') * 10 + (display[i + 4] - '0');
  return -1;
}

// the files of the patch archive (8 to 15) that an installed package's index replaces: file -> the package's file
std::map<int, std::string> PatchFilesOf(const fs::path& package, int group) {
  std::map<int, std::string> out;
  char index_name[32];
  std::snprintf(index_name, sizeof index_name, "index_%02d_X360.dat", group);
  std::ifstream in(package / index_name, std::ios::binary);
  const std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  // u32 version, u32 ids, u32 strings size, the strings, u16 slots, u16 entries; each entry (sub_82397F28): u16 group,
  // u16 slot, u8, u16 n1 + n1 u16, u16 archive, u16 file, u16 n2 + n2 u16, u32 its file's name, u8, u64 offer, u32
  // license bits
  if (d.size() < 12) return out;
  const uint32_t strings = Be32At(d, 8);
  size_t p = 12 + size_t(strings);
  if (p + 4 > d.size()) return out;
  const uint16_t entries = Be16(d, p + 2);
  p += 4;
  for (uint16_t e = 0; e < entries; e++) {
    if (p + 7 > d.size()) return out;
    p += 5;
    p += 2 + 2 * size_t(Be16(d, p));
    if (p + 6 > d.size()) return out;
    const int archive = Be16(d, p), file = Be16(d, p + 2);
    p += 4;
    p += 2 + 2 * size_t(Be16(d, p));
    if (p + 17 > d.size()) return out;
    const uint32_t name = Be32At(d, p);
    p += 17;
    if (archive != kArchivePatch || file < kNamesFirstFile || file >= kNamesFirstFile + 8 || !name || name >= strings)
      continue;
    std::string s;
    for (size_t i = 12 + name; i < 12 + size_t(strings) && d[i]; i++) s += char(d[i]);
    if (!s.empty()) out.emplace(file, s);
  }
  return out;
}

}  // namespace

void Install(const fs::path& dlc_dir, const fs::path& saves) {
  std::error_code ec;
  const fs::path content = saves / "0000000000000000" / "4E4D083D" / "00000002";
  const fs::path headers = saves / "0000000000000000" / "4E4D083D" / "Headers" / "00000002";
  std::vector<std::string> mine;
  std::map<std::pair<int, int>, fs::path> names_by_group;  // (group, patch file) -> the file in the saves
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
      // the parts' names it carries (the game keeps the lowest group's copy of a file the packages replace)
      if (const int group = GroupOf(display); group >= 0) {
        for (const auto& [file, at] : PatchFilesOf(entry.path(), group))
          names_by_group[{group, file}] = content / name / at;
      }
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
  std::lock_guard<std::mutex> lock(g_names_lock);
  g_names_files.clear();
  for (const auto& [key, path] : names_by_group) g_names_files.emplace(key.second, path);  // the lowest group first
  if (!g_names_files.empty())
    REXLOG_INFO("sc5 dlc: the names of the downloadable parts (patch {} to {}) from {}", g_names_files.begin()->first,
                g_names_files.rbegin()->first, g_names_files.begin()->second.parent_path().filename().string());
}

void OnFileAsked(int archive, int file) {
  // the game's patch text for table 54 (patch 0 to 7, one per language) tells the language
  if (archive == kArchivePatch && file >= 0 && file < kNamesFirstFile) g_language = file;
}

void BeforeText(PPCContext& ctx, uint8_t* base, uint32_t table) {
  if (table != kDlcNamesTable) return;
  std::lock_guard<std::mutex> lock(g_names_lock);
  if (g_names_failed || g_names_files.empty()) return;
  {
    PPCContext c = ctx;
    c.r3.u64 = table;
    sub_82348780(c, base);
    if (c.r3.u32) return;  // the game has it
  }
  if (g_names_data.empty()) {
    const auto it = g_names_files.find(kNamesFirstFile + g_language);
    if (it == g_names_files.end()) {
      REXLOG_WARN("sc5 dlc: the packages carry no names of their parts for language {}", g_language);
      g_names_failed = true;
      return;
    }
    std::ifstream in(it->second, std::ios::binary);
    g_names_data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    g_names_guest = g_names_data.size() >= 16 && g_names_data.size() < (4u << 20)
                        ? REX_KERNEL_STATE()->memory()->SystemHeapAlloc(uint32_t(g_names_data.size()) + 16, 0x20)
                        : 0;
    if (!g_names_guest) {
      REXLOG_WARN("sc5 dlc: the names of the downloadable parts ({}) could not be read", it->second.string());
      g_names_failed = true;
      return;
    }
  }
  // the file as it is (the game unscrambles it in place), registered as the table
  uint8_t* at = REX_KERNEL_STATE()->memory()->TranslateVirtual<uint8_t*>(g_names_guest);
  std::memcpy(at, g_names_data.data(), g_names_data.size());
  std::memset(at + g_names_data.size(), 0, 16);
  PPCContext c = ctx;
  c.r3.u64 = table;
  c.r4.u64 = g_names_guest;
  c.lr = 0x82189A40;  // as from main's loop
  sub_82348660(c, base);
  const uint32_t strings = game::ReadU32(g_names_guest + 4);
  REXLOG_INFO("sc5 dlc: the names of the downloadable parts: table {}, {} strings (patch {})", table, strings,
              kNamesFirstFile + g_language);
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

// [[midasm_hook]] Sc5DlcFighterModels (config\sc5_codegen.toml), in sub_82148400, which sets up a fighter for a fight
// from a creation manager entry (r4; the record first): r11 is the entry's "wears downloadable models" flag (+0x34D8,
// the record's byte 15, which Creation sets for any 0x7F model), and with it set the game swaps the character for a
// template of its style in plain clothes (sub_8229EF18 mode 1), its own way out when such a model cannot be loaded. It
// did so for every such character, the costume packs' data installed or not; so when every downloadable model the
// character wears has its data on this PC the flag is taken as clear and the character fights as created. Both games
// of a match decide alike: a created character with downloadable parts is picked only when the two games' downloadable
// parts are the same (creation.cpp's fingerprint).
void Sc5DlcFighterModels(PPCRegister& r4, PPCRegister& r11) {
  if (r11.u32 && r4.u32 && sc5::dlc::ModelsInstalled(r4.u32)) r11.u64 = 0;
}

// The game's file requests (sub_82363FC8 the size of a file, asked before it is read; sub_82355DF8 the read: r3 the
// archive, r4 the file): the patch text's language for the parts' names. SC5_DBG_FILES=1 logs each with its callers.
namespace {
uint32_t Rd32(uint32_t a) { return sc5::game::ReadU32(a); }
bool FileDebug() {
  static const bool on = [] { const char* v = std::getenv("SC5_DBG_FILES"); return v && v[0] == '1'; }();
  return on;
}
void LogFile(const char* what, PPCContext& ctx) {
  const uint32_t back = Rd32(ctx.r1.u32), lr2 = back ? Rd32(back - 8) : 0, back2 = back ? Rd32(back) : 0,
                 lr3 = back2 ? Rd32(back2 - 8) : 0;
  REXLOG_INFO("sc5 dbg file: {} archive {} file {} from {:08X} <- {:08X} <- {:08X} (frame {})", what, ctx.r3.s32,
              ctx.r4.s32, uint32_t(ctx.lr), lr2, lr3, sc5::frame::Count());
}
}  // namespace

REX_EXTERN(__imp__sub_82363FC8);
REX_HOOK_RAW(sub_82363FC8) {
  sc5::dlc::OnFileAsked(ctx.r3.s32, ctx.r4.s32);
  if (FileDebug()) LogFile("size", ctx);
  __imp__sub_82363FC8(ctx, base);
}
REX_EXTERN(__imp__sub_82355DF8);
REX_HOOK_RAW(sub_82355DF8) {
  if (FileDebug()) LogFile("read", ctx);
  __imp__sub_82355DF8(ctx, base);
}

// A string of a text table (sub_82347670: r3 the table, r4 the string, 1 on): the downloadable parts' names registered
// the first time the game asks for their table (above). SC5_DBG_TEXT=1 logs each string the game asks for and does not
// have (no such table, or not that many strings), once each, with the caller.
REX_EXTERN(__imp__sub_82347670);
REX_HOOK_RAW(sub_82347670) {
  sc5::dlc::BeforeText(ctx, base, ctx.r3.u32);
  static const bool debug = [] { const char* v = std::getenv("SC5_DBG_TEXT"); return v && v[0] == '1'; }();
  if (!debug) {
    __imp__sub_82347670(ctx, base);
    return;
  }
  const uint32_t table = ctx.r3.u32, index = ctx.r4.u32 & 0xFFFF, lr = uint32_t(ctx.lr);
  PPCContext c = ctx;
  sub_82348780(c, base);
  const uint32_t data = c.r3.u32;
  const bool missing = int32_t(ctx.r4.u32) != -1 && (!data || index == 0 || index > Rd32(data + 4));
  __imp__sub_82347670(ctx, base);
  static std::mutex lock;
  std::lock_guard<std::mutex> hold(lock);
  static std::map<uint64_t, int> seen;
  if (missing && seen[uint64_t(table) << 32 | index]++ == 0)
    REXLOG_INFO("sc5 dbg text: table {} string {} not there ({}), asked from {:08X}", table, index,
                data ? fmt::format("the table has {}", Rd32(data + 4)) : std::string("no such table"), lr);
}
