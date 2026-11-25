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
//
// The names of the parts the costume packs add (text table 55) need nothing from here under title update 3: Creation's
// loader (sub_8224A5F8) registers the table itself from patch 8 + the language, a file the installed packages replace
// (the lowest group's copy, which names every part; the update's own copy still has 145 placeholders such as arm_201).
// Title update 1 never registered it, and this file used to.

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

REX_EXTERN(sub_82348780);  // a text table's data (r3 its number): 0 when the game does not have it

namespace sc5::dlc {
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kTitleId = 0x4E4D083D;
constexpr uint32_t kMarketplaceContent = 0x00000002;

// the game's DLC manager's table (docs\GAME.md, "Unlocks and downloadable content"; title update 3's, seen with
// SC5_DBG_DLC=1: the manager 0x82782F94 keeps it at +8): its state at +8 (7 to 11 once the packages are read), at +0x28
// its 30 package objects of 344 bytes (+2 the entries' count, +0x1C the entries, 48 bytes each, +0xD their flags: 0x80
// the entry's file is there), their count at +0x2C
constexpr uint32_t kDlcManager = 0x82782F30;
constexpr uint32_t kPackageSize = 344, kEntrySize = 48;
constexpr uint8_t kEntryHasFile = 0x80;

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

uint8_t ReadByte(uint32_t a) { return ReadU8(a); }

bool ModelFileInstalled(uint32_t model) {
  if (model >> 24 != 0x7F) return true;
  const uint32_t state = game::ReadU32(kDlcManager + 8);
  const uint32_t packages = game::ReadU32(kDlcManager + 0x28), count = game::ReadU32(kDlcManager + 0x2C);
  if (state < 7 || state > 11 || !packages || count > 30) return false;
  const uint32_t group = (model >> 16) & 0xFF, slot = (model >> 8) & 0xFF;
  if (group >= count) return false;
  const uint32_t package = packages + group * kPackageSize;
  const uint32_t entries = game::ReadU32(package + 0x1C);
  return entries && slot < ReadU16(package + 2) && (ReadU8(entries + slot * kEntrySize + 0xD) & kEntryHasFile);
}

}  // namespace sc5::dlc

// Title update 3's fallback for a created character whose models cannot be loaded (docs\GAME.md, "Downloadable parts
// in a fight"). The character model loader's state machine (sub_82360E18; its object: +0x80 the state, +0x84 the
// fighter's part after the creation manager's entry, +0x88 the entry's holder, +0x120 which break levels) loads break
// levels 0 and 1 (sub_823616A8 with r4 = 0); when that load fails (sub_82361750) it starts again with r4 = 1, the
// record's levels 2 and 3 in their place (sub_82221C00 sets the entry's +0x34D8 and lists that level's models),
// which Creation fills with the plain body, and the fight shows them (the fighter's byte +0x2246A, which the fight's
// setup sub_82148400 passes on). Logged: online both games must show the same levels, which the check makes sure of
// (creation.cpp: a downloadable model whose file this PC lacks is refused), so this line in a match's log is a fault.
REX_EXTERN(__imp__sub_823616A8);
REX_HOOK_RAW(sub_823616A8) {
  const uint32_t flag = ctx.r4.u32, holder = ctx.r3.u32 + 0x88;
  __imp__sub_823616A8(ctx, base);
  if (flag) {
    const uint32_t entry = sc5::game::ReadU32(holder);
    std::string name;
    for (uint32_t i = 0; entry && i < 44; i++) {
      const uint8_t c = sc5::dlc::ReadByte(entry + 13245 + i);
      if (!c) break;
      name += char(c);
    }
    REXLOG_WARN("sc5 dlc: the models of the created character {} could not be loaded: the game shows its plain body "
                "(break levels 2 and 3)", name);
  }
}

// The game's file requests (sub_82363FC8 the size of a file, asked before it is read; sub_82364500 the read: r3 the
// archive, r4 the file): SC5_DBG_FILES=1 logs each with its callers.
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
  if (FileDebug()) LogFile("size", ctx);
  __imp__sub_82363FC8(ctx, base);
}
REX_EXTERN(__imp__sub_82364500);
REX_HOOK_RAW(sub_82364500) {
  if (FileDebug()) LogFile("read", ctx);
  __imp__sub_82364500(ctx, base);
}

// A string of a text table (sub_82347670: r3 the table, r4 the string, 1 on): SC5_DBG_TEXT=1 logs each string the game
// asks for and does not have (no such table, or not that many strings), once each, with the caller.
REX_EXTERN(__imp__sub_82347670);
REX_HOOK_RAW(sub_82347670) {
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
