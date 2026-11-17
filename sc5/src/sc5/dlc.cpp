// The player's own downloadable content (docs\PROTOCOL.md 3.7, docs\GAME.md "Unlocks and downloadable content"):
// Build game (or tools\py\x360.py dlc-install) lays each of the player's own Xbox 360 DLC packages out as
// <dlc>\<package>\ (its files) and <dlc>\<package>.header (the package's header). The SDK's content manager finds
// marketplace content under the saves, Xenia's way: <saves>\0000000000000000\4E4D083D\00000002\<package>\, with a
// record of its own beside it in ...\4E4D083D\Headers\00000002\<package>.header (the content record, 0x148 bytes, then
// the license mask). Install copies the player's packages there and writes the records (the package's display name,
// every license bit: the player owns it), and takes out what is no longer in <dlc>: only packages the player has.
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

namespace sc5::dlc {
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kTitleId = 0x4E4D083D;
constexpr uint32_t kMarketplaceContent = 0x00000002;

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

bool SameFile(const fs::path& a, const fs::path& b) {
  std::error_code ec;
  return fs::exists(b, ec) && fs::file_size(a, ec) == fs::file_size(b, ec);
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
      // the package's files
      size_t files = 0;
      for (const auto& f : fs::recursive_directory_iterator(entry.path(), ec)) {
        if (!f.is_regular_file()) continue;
        const fs::path to = content / name / fs::relative(f.path(), entry.path(), ec);
        fs::create_directories(to.parent_path(), ec);
        if (!SameFile(f.path(), to)) fs::copy_file(f.path(), to, fs::copy_options::overwrite_existing, ec);
        files++;
      }
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

}  // namespace sc5::dlc
