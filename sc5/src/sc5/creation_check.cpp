// The check of a created character from another PC, see creation_check.h.
#include "sc5/creation_check.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <string_view>
#include <tuple>
#include <vector>

#include <format>

namespace sc5::creation {
namespace {

using namespace dna;

uint32_t U32(const uint8_t* r, uint32_t at) {
  return uint32_t(r[at]) << 24 | uint32_t(r[at + 1]) << 16 | uint32_t(r[at + 2]) << 8 | r[at + 3];
}
uint32_t U16(const uint8_t* r, uint32_t at) { return uint32_t(r[at]) << 8 | r[at + 1]; }
float F32(const uint8_t* r, uint32_t at) { return std::bit_cast<float>(U32(r, at)); }

// a float field within [lo, hi] (NaN fails)
bool Within(const uint8_t* r, uint32_t at, float lo, float hi) {
  const float v = F32(r, at);
  return v >= lo && v <= hi;
}

// A name field: NUL-terminated inside its bytes, UTF-8 without control characters, and without the three characters
// the game itself turns into their full-width forms before it keeps a name ('%', '#', '$': sub_82222B28 and
// sub_82349AE0), since its text code takes a name for a format.
std::string Name(const uint8_t* r, uint32_t at, uint32_t size, const char* what) {
  uint32_t i = 0;
  while (i < size && r[at + i]) {
    const uint8_t c = r[at + i];
    if (c < 0x20 || c == 0x7F) return std::format("its {} has a control character ({:02X})", what, c);
    if (c == '%' || c == '#' || c == '$') return std::format("its {} has a '{}'", what, char(c));
    if (c < 0x80) {
      i++;
      continue;
    }
    // a UTF-8 sequence: its length from the first byte, then continuation bytes, the shortest form, no surrogates
    const uint32_t n = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (!n || i + n > size) return std::format("its {} is not UTF-8", what);
    uint32_t cp = c & (0x7F >> n);
    for (uint32_t k = 1; k < n; k++) {
      const uint8_t d = r[at + i + k];
      if ((d & 0xC0) != 0x80) return std::format("its {} is not UTF-8", what);
      cp = cp << 6 | (d & 0x3F);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
      return std::format("its {} is not UTF-8", what);
    i += n;
  }
  if (i >= size) return std::format("its {} has no end", what);
  return {};
}

// A colour (TK_CreationColorData_st: palette, a flag, row, column, saturation): none (palette 0xFF) or a place on one
// of the game's palettes (0-3; sub_8224B8E8) within its rows and columns (sub_8222D210), saturation 0-1
std::string Colour(const uint8_t* r, uint32_t at, const Known& k, const std::string& what) {
  const uint8_t palette = r[at];
  if (palette == 0xFF) return {};
  if (palette > 3) return std::format("{} has palette {}", what, palette);
  if (r[at + 2] > k.palette_rows || r[at + 3] > k.palette_cols)
    return std::format("{} at {},{} is off the palette", what, r[at + 2], r[at + 3]);
  if (!Within(r, at + 4, 0.0f, 1.0f)) return std::format("{} has saturation {}", what, F32(r, at + 4));
  return {};
}

// A voice the game knows: sub_8235EE20 keeps these and turns any other into 257, with which a customized fighter's
// fight never loads (both games give up on it after 40 s; an original character takes it)
bool KnownVoice(int v) {
  return (v >= 1 && v <= 8) || (v >= 11 && v <= 23) || v == 26 || (v >= 34 && v <= 36) || v == 40 || v == 41 ||
         (v >= 47 && v <= 66) || (v >= 96 && v <= 111) || v == 255;
}

}  // namespace

bool KnownModel(const std::set<uint32_t>& models, uint32_t model) {
  if (models.count(model)) return true;
  if ((model & 0xFF) != kNoSegment) return false;
  // a segment the game never shows: any of this file's models here (they sort together, the file first)
  const auto it = models.lower_bound(model & kModelFile);
  return it != models.end() && (*it & kModelFile) == (model & kModelFile);
}

std::string PlainWhy(const std::string& why) {
  const auto has = [&](const char* s) { return why.find(s) != std::string::npos; };
  if (why.empty()) return {};
  if (has("slots 49 and 50")) return "slots 49 and 50 are kept for the other player's character";
  if (has("tables yet")) return "this PC's game has not listed its parts yet";
  // Decide's: what the other PC lacks
  if (has("on the other PC") && has("character type"))
    return "the other player's game does not have this fighter (it comes with a downloadable package)";
  if (has("the other PC's select screen"))
    return "the other player's game does not have its fighting style (it comes with a downloadable package)";
  if (has("this PC's select screen"))
    return "this PC's game does not have its fighting style (it comes with a downloadable package)";
  if (has("no file on the other PC"))
    return "it wears downloadable parts the other player's PC does not have, even without its equipment";
  if (has("the other PC lacks"))
    return "the other player's PC does not have some of its downloadable parts: both games show it without its "
           "equipment";
  if (has("this PC lacks"))
    return "this PC does not have some of its downloadable parts: both games show it without its equipment";
  if (has("no file on this PC"))
    return "it wears downloadable parts this PC does not have, even without its equipment";
  if (has("bytes, not") || has("version") || has("checksum") || has("chunk"))
    return "its saved data is damaged";
  if (has("name")) return "its name has a character the game cannot keep";
  if (has("colour") || has("trail") || has("effect") || has("value") || has("rate") || has("scale") ||
      has("breadth") || has("pitch") || has("formant") || has("flanger") || has("validity"))
    return "a setting in it is outside what Creation allows";
  if (has("more than")) return "it wears more pieces than the game can load";
  if (has("sticker")) return "it has a sticker this PC's game does not have";
  if (has("voice")) return "its voice cannot be used for a fighter";
  if (has("with base type")) return "it is not made the way Creation makes it";
  if (has("weapon's style")) return "its weapon does not match its fighting style";
  if (has("weapon")) return "its weapon is not in this PC's game";
  if (has("style")) return "its fighting style is not in this PC's game";
  if (has("character type") || has("own costumes")) return "its fighter is not in this PC's game";
  if (has("model") || has("base type") || has("costume")) return "it wears a part the game does not have";
  return "it does not pass the online check";
}

std::vector<uint32_t> MissingFiles(const uint8_t* r, Form form, const std::set<uint32_t>& files) {
  std::vector<uint32_t> missing;
  for (uint32_t level = FirstLevel(form); level < FirstLevel(form) + 2; level++)
    for (uint32_t k = 0; k < kPartsPerLevel; k++) {
      const uint32_t at = kBreakLevels + level * kBreakLevelSize + k * 8;
      if (r[at] & 0x80) continue;  // none
      const uint32_t file = U32(r, at) & kModelFile;
      if (IsDlcModel(file) && !files.count(file) && std::find(missing.begin(), missing.end(), file) == missing.end())
        missing.push_back(file);
    }
  return missing;
}

Decision Decide(const uint8_t* r, size_t size, const Known& mine, const Other& other) {
  Decision d;
  // this PC: as made, else in its plain body (a downloadable part whose data this PC lost since Creation made it)
  const std::string why = CheckRecord(r, size, mine, Form::kWhole);
  Form here = Form::kWhole;
  if (!why.empty()) {
    const std::string plain_why = CheckRecord(r, size, mine, Form::kPlain);
    if (!plain_why.empty()) {
      // the plain body's reason when the whole one's was a missing file (it says which levels failed last)
      d.why = why.find("no file") != std::string::npos ? plain_why : why;
      return d;
    }
    here = Form::kPlain;
  }
  // the other PC: the fighter (Dampierre comes with his package), then the downloadable files of each form; the rest
  // of the check is the same there
  if (!other.characters.count(r[kType])) {
    d.why = std::format("character type {} is not on the other PC", r[kType]);
    return d;
  }
  if (other.unlisted_styles.count(r[kStyle])) {
    d.why = std::format("style {} is not listed by the other PC's select screen without its package", r[kStyle]);
    return d;
  }
  const std::vector<uint32_t> whole = MissingFiles(r, Form::kWhole, other.dlc_files);
  if (here == Form::kWhole && whole.empty()) {
    d.ok = true;
    return d;
  }
  const std::vector<uint32_t> plain = MissingFiles(r, Form::kPlain, other.dlc_files);
  if (!plain.empty()) {
    d.why = std::format("downloadable model file {:08X} (break levels 2 and 3, its plain body) has no file on the other PC",
                        plain[0]);
    return d;
  }
  d.ok = true;
  d.form = Form::kPlain;
  if (here == Form::kPlain) {
    const std::vector<uint32_t> own = MissingFiles(r, Form::kWhole, mine.dlc_files);
    d.why = std::format("this PC lacks downloadable model file {:08X}: its plain body", own.empty() ? 0 : own[0]);
  } else {
    d.why = std::format("the other PC lacks downloadable model file {:08X}{}: its plain body", whole[0],
                        whole.size() > 1 ? std::format(" and {} more", whole.size() - 1) : "");
  }
  return d;
}

uint32_t Checksum(const uint8_t* record) {
  // sub_822470B0: each word turned left by an amount the previous one sets, XORed together
  uint32_t acc = 0, rot = 7;
  for (uint32_t j = 0; j < kChecksum / 4; j++) {
    const uint32_t w = std::rotl(U32(record, 4 * j), int(rot));
    acc ^= w;
    const int sum = int((w >> 24) + (w >> 16 & 0xFF) + (w >> 8 & 0xFF) + (w & 0xFF));
    int next = sum % 29 - int(j % 7) + 7;
    if (next < 0) next += 32;
    else if (next > 31) next -= 32;
    rot = uint32_t(next);
  }
  return acc;
}

std::string KnownText(const Known& k) {
  std::string out = "# what a PC's game has for created characters (sc5/src/sc5/creation_check.h, Known)\n";
  // a kind's values, a dozen to a line
  auto lines = [&](const std::string& head, const std::vector<std::string>& values) {
    for (size_t i = 0; i < values.size(); i += 12) {
      out += head;
      for (size_t j = i; j < values.size() && j < i + 12; j++) out += " " + values[j];
      out += "\n";
    }
  };
  std::vector<std::string> v;
  for (int c : k.characters) v.push_back(std::to_string(c));
  lines("characters", v);
  v.clear();
  for (int s : k.styles) v.push_back(std::to_string(s));
  lines("styles", v);
  out += std::format("palette {} {}\n", k.palette_rows, k.palette_cols);
  v.clear();
  for (const auto& [style, variation] : k.weapons) v.push_back(std::format("{}.{}", style, variation));
  lines("weapons", v);
  v.clear();
  for (const auto& [id, texture] : k.stickers) v.push_back(std::format("{:08X}:{:08X}", id, texture));
  lines("stickers", v);
  for (const auto* kind : {&k.parts_sets, &k.models}) {
    for (const auto& [key, ids] : *kind) {
      v.clear();
      for (uint32_t id : ids) v.push_back(std::format("{:08X}", id));
      lines(std::format("{} {} {}", kind == &k.parts_sets ? "sets" : "models", key.first, key.second), v);
    }
  }
  v.clear();
  for (uint32_t f : k.dlc_files) v.push_back(std::format("{:08X}", f));
  lines("dlc", v);
  v.clear();
  for (int s : k.unlisted_styles) v.push_back(std::to_string(s));
  lines("unlisted", v);
  return out;
}

namespace {
// a whole word as a number (decimal or hex), or two around a separator
bool Num(std::string_view s, int base, uint32_t& v) {
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v, base);
  return ec == std::errc() && end == s.data() + s.size() && !s.empty();
}
bool Pair(std::string_view s, char sep, int base, uint32_t& a, uint32_t& b) {
  const size_t at = s.find(sep);
  return at != std::string_view::npos && Num(s.substr(0, at), base, a) && Num(s.substr(at + 1), base, b);
}
}  // namespace

bool ParseKnown(const std::string& text, Known& out) {
  Known k;
  size_t at = 0;
  while (at < text.size()) {
    const size_t end = std::min(text.find('\n', at), text.size());
    std::string_view line(text.data() + at, end - at);
    at = end + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string_view> w;
    for (size_t i = 0; i < line.size();) {
      const size_t j = std::min(line.find(' ', i), line.size());
      if (j > i) w.push_back(line.substr(i, j - i));
      i = j + 1;
    }
    if (w.empty()) continue;
    const std::string_view kind = w[0];
    uint32_t a = 0, b = 0;
    if (kind == "characters" || kind == "styles" || kind == "unlisted") {
      for (size_t i = 1; i < w.size(); i++) {
        if (!Num(w[i], 10, a)) return false;
        (kind == "styles" ? k.styles : kind == "unlisted" ? k.unlisted_styles : k.characters).insert(int(a));
      }
    } else if (kind == "palette" && w.size() == 3) {
      if (!Num(w[1], 10, a) || !Num(w[2], 10, b)) return false;
      k.palette_rows = int(a);
      k.palette_cols = int(b);
    } else if (kind == "weapons") {
      for (size_t i = 1; i < w.size(); i++) {
        if (!Pair(w[i], '.', 10, a, b)) return false;
        k.weapons.insert({int(a), int(b)});
      }
    } else if (kind == "dlc") {
      for (size_t i = 1; i < w.size(); i++) {
        if (!Num(w[i], 16, a)) return false;
        k.dlc_files.insert(a);
      }
    } else if (kind == "stickers") {
      for (size_t i = 1; i < w.size(); i++) {
        if (!Pair(w[i], ':', 16, a, b)) return false;
        k.stickers[a] = b;
      }
    } else if ((kind == "sets" || kind == "models") && w.size() >= 3) {
      if (!Num(w[1], 10, a) || !Num(w[2], 10, b)) return false;
      auto& ids = (kind == "sets" ? k.parts_sets : k.models)[{int(a), int(b)}];
      for (size_t i = 3; i < w.size(); i++) {
        uint32_t id = 0;
        if (!Num(w[i], 16, id)) return false;
        ids.insert(id);
      }
    } else {
      return false;
    }
  }
  out = std::move(k);
  return true;
}

std::string CheckForm(const uint8_t* r, size_t size) {
  if (size != kSize) return std::format("{} bytes, not {}", size, kSize);
  if (U16(r, kMajor) != 11 || U16(r, kMinor) != 0)
    return std::format("version {}.{}, not 11.0", U16(r, kMajor), U16(r, kMinor));
  if (Checksum(r) != U32(r, kChecksum)) return "its checksum is wrong";

  // the names: text the game's own text code can take (an empty one is text too; Creation never makes one)
  for (const auto& [at, n, what] : {std::tuple{kFacialName, kFacialNameSize, "facial motion's name"},
                                    std::tuple{kName, kNameSize, "short name"},
                                    std::tuple{kNameRelease, kNameLongSize, "name"},
                                    std::tuple{kNameRaw, kNameLongSize, "name as typed"}}) {
    if (std::string why = Name(r, at, n, what); !why.empty()) return why;
  }
  return {};
}

std::string CheckRecord(const uint8_t* r, size_t size, const Known& known, Form form) {
  if (std::string why = CheckForm(r, size); !why.empty()) return why;

  // who it is: an original character or a fighter this PC's select screen has, with a style Creation offers
  const int type = r[kType], style = r[kStyle];
  if (!known.characters.count(type)) return std::format("character type {} is not on this PC", type);
  if (!known.styles.count(style)) return std::format("style {} is not one Creation offers here", style);
  if (known.unlisted_styles.count(style))
    return std::format("style {} is not listed by this PC's select screen without its package", style);
  if (U16(r, kWeapon + kWeaponStyle) != uint32_t(style))
    return std::format("its weapon's style {} is not its style {}", U16(r, kWeapon + kWeaponStyle), style);
  const int base = r[kBaseType];
  if (type == kOriginal && base > 1) return std::format("an original character with base type {}", base);
  if (type != kOriginal && base != kFighterBase)
    return std::format("a customized fighter (character type {}) with base type {}", type, base);
  if (type != kOriginal && !KnownVoice(r[kVoice]))
    return std::format("a customized fighter with voice {}, which the game does not know", r[kVoice]);

  // the voice and the body as Creation's sliders make them, which title updates 1 and 3 hold alike (their repair
  // passes, sub_82221D88 and sub_82222B28, clamp to the same limits but for the attack rate, which is not held here:
  // dna::kAttackRate). Creation never makes a record outside them; garbage body sizes (huge, NaN) hang both games.
  // The DLC parts flag (15) is not held either: title update 3 no longer reads it (docs\GAME.md, "Downloadable parts in a
  // fight").
  if (int8_t(r[kPitch]) < -50 || int8_t(r[kPitch]) > 50) return std::format("voice pitch {}", int8_t(r[kPitch]));
  if (int8_t(r[kFormant]) < -50 || int8_t(r[kFormant]) > 50)
    return std::format("voice formant {}", int8_t(r[kFormant]));
  if (r[kFlanger] > 2) return std::format("voice flanger {}", r[kFlanger]);
  static constexpr struct {
    uint32_t at;
    float lo, hi;
    const char* what;
  } kBodyLimits[] = {{kBody + 0, 0.08f, 0.15f, "shoulder breadth"}, {kBody + 4, 0.9f, 1.2f, "upper body scale"},
                     {kBody + 8, 0.9f, 1.1f, "lower body scale"},   {kBody + 12, 0.9f, 1.1f, "face scale"},
                     {kBody + 16, 0.7f, 1.1f, "hand scale"},        {kBody + 20, 0.8f, 1.2f, "body scale"}};
  for (const auto& b : kBodyLimits)
    if (!Within(r, b.at, b.lo, b.hi)) return std::format("its {} {} is out of Creation's range", b.what, F32(r, b.at));
  // the body's rates (blending, humerus ... bust): Creation's sliders give 0 to 1
  for (uint32_t at = kBody + 24; at < kBody + 64; at += 4)
    if (!Within(r, at, 0.0f, 1.0f)) return std::format("a body rate {} (at {}) is out of 0-1", F32(r, at), at);

  // the models it wears: from the tables for its character and base type (an original character: its base type's; a
  // customized fighter: that fighter's own costumes and the fighters' hair), the same on every PC but for the
  // downloadable ones, which the two break levels the match shows need the data of (Known::dlc_files). An unknown
  // model hangs the game at the costume panel (tools\creationfuzz.py, "models"). The record's own list of parts sets
  // is not held to anything: the game ignores ids it lacks ("partsset", fought in sync).
  const auto models = known.models.find({type, base});
  if (models == known.models.end())
    return type == kOriginal ? std::format("base type {} has no parts here", base)
                             : std::format("character type {}'s own costumes are not on this PC", type);
  for (uint32_t level = 0; level < kBreakLevelCount; level++) {
    const bool shown = level >= FirstLevel(form) && level < FirstLevel(form) + 2;
    for (uint32_t k = 0; k < kPartsPerLevel; k++) {
      const uint32_t at = kBreakLevels + level * kBreakLevelSize + k * 8;
      const uint32_t model = U32(r, at);
      if (r[at] & 0x80) continue;  // none (the game's own test: the first byte negative)
      if (IsDlcModel(model)) {
        // a downloadable model: its data, where the fight loads it
        if (shown && !known.dlc_files.count(model & kModelFile))
          return std::format("downloadable model file {:08X} (break level {}) has no file on this PC",
                             model & kModelFile, level);
        continue;
      }
      if (!KnownModel(models->second, model)) return std::format("model {:08X} is not on this PC", model);
    }
  }
  // the fight and Creation list the models of break level 0 (2 when the game shows the record's plain body: title
  // update 3's fallback, creation_check.h) in a list of 20 (sub_82221548 from sub_82221828, which the fight's
  // sub_821485E0 and Creation's sub_821D1728 call; a model once however many segments it wears): a 21st would write
  // over the list's count. The select screen's creation manager lists level 0's for every record (sub_82221978).
  for (const uint32_t level : {0u, 2u}) {
    if (level == 2 && form != Form::kPlain) continue;
    uint32_t files[kMaxModelFiles];
    uint32_t n = 0;
    for (uint32_t k = 0; k < kPartsPerLevel; k++) {
      const uint32_t at = kBreakLevels + level * kBreakLevelSize + k * 8;
      if (r[at] & 0x80) continue;
      const uint32_t file = U32(r, at) & kModelFile;
      if (std::find(files, files + n, file) != files + n) continue;
      if (n == kMaxModelFiles)
        return std::format("it wears more than {} models{}", kMaxModelFiles, level ? " in break level 2" : "");
      files[n++] = file;
    }
  }
  // a customized fighter wears its costume (a model of parts set type 0, which only its own costumes have here) in
  // every break level, as Creation makes it: without one the game does not list it, and the other game would wait
  if (type != kOriginal) {
    for (uint32_t level = 0; level < kBreakLevelCount; level++) {
      bool costume = false;
      for (uint32_t k = 0; k < kPartsPerLevel && !costume; k++)
        costume = r[kBreakLevels + level * kBreakLevelSize + k * 8] == 0;
      if (!costume) return std::format("a customized fighter without its costume in break level {}", level);
    }
  }

  // parts coordinates (CCreationPartsCoordinateInfo: valid, parts id, parent bone, order, 7 vectors): valid 0 or 1,
  // the vectors finite and within +-1000. The game itself skips one whose part is not the one worn or whose bone is
  // not one of its 18, and clamps the offsets to +-50 and +-180 (sub_82243AF8).
  for (uint32_t i = 0; i < kCoordCount; i++) {
    const uint32_t at = kCoords + i * kCoordSize, valid = U32(r, at);
    if (valid > 1) return std::format("parts coordinate {} has validity {}", i, valid);
    if (!valid) continue;
    for (uint32_t f = 12; f < kCoordSize; f += 4)
      if (!std::isfinite(F32(r, at + f)) || std::fabs(F32(r, at + f)) > 1000.0f)
        return std::format("parts coordinate {} has a value {}", i, F32(r, at + f));
  }

  // stickers (the character's and the weapon's): illustrations this PC has
  for (uint32_t k = 0; k < kStickerCount + kWeaponStickerCount; k++) {
    const uint32_t at = k < kStickerCount ? kStickers + k * kStickerSize
                                          : kWeapon + kWeaponStickers + (k - kStickerCount) * kStickerSize;
    const uint32_t id = U32(r, at);
    if (id != 0xFFFFFFFF) {
      const auto it = known.stickers.find(id);
      if (it == known.stickers.end()) return std::format("sticker {:08X} is not on this PC", id);
      const uint32_t texture = uint32_t(r[at + 16]) << 24 | uint32_t(r[at + 17]) << 16 | U16(r, at + 14);
      if (texture != it->second)
        return std::format("sticker {:08X}'s texture {:08X} is not its own ({:08X})", id, texture, it->second);
    }
    for (uint32_t f = 20; f < 60; f += 4)
      if (!std::isfinite(F32(r, at + f)) || std::fabs(F32(r, at + f)) > 1.0e4f)
        return std::format("sticker {} has a value {}", k, F32(r, at + f));
  }

  // colours: the character's, its stickers', the weapon's
  for (uint32_t i = 0; i < kColourCount; i++)
    if (std::string why = Colour(r, kColours + 8 * i, known, std::format("colour {}", i)); !why.empty()) return why;
  for (uint32_t k = 0; k < kStickerCount + kWeaponStickerCount; k++) {
    const uint32_t at = k < kStickerCount ? kStickers + k * kStickerSize
                                          : kWeapon + kWeaponStickers + (k - kStickerCount) * kStickerSize;
    for (uint32_t c = 0; c < 4; c++)
      if (std::string why = Colour(r, at + 60 + 8 * c, known, std::format("sticker {}'s colour {}", k, c));
          !why.empty())
        return why;
  }
  for (uint32_t c = 0; c < 4; c++)
    if (std::string why = Colour(r, kWeapon + kWeaponColours + 8 * c, known, std::format("the weapon's colour {}", c));
        !why.empty())
      return why;
  // the weapon's trail (CCreationTraceColorInfo: 3 colours, then 3 x 16 numbers 0-1) and effect (a colour, an id,
  // 15 numbers)
  for (uint32_t c = 0; c < 3; c++)
    if (std::string why = Colour(r, kWeapon + kWeaponTrace + 8 * c, known, std::format("the trail's colour {}", c));
        !why.empty())
      return why;
  for (uint32_t at = kWeapon + kWeaponTrace + 24; at < kWeapon + kWeaponTrace + 216; at += 4)
    if (!std::isfinite(F32(r, at)) || std::fabs(F32(r, at)) > 16.0f)
      return std::format("the weapon's trail has a value {}", F32(r, at));
  if (std::string why = Colour(r, kWeapon + kWeaponEffectInfo, known, "the weapon effect's colour"); !why.empty())
    return why;
  for (uint32_t at = kWeapon + kWeaponEffectInfo + 16; at < kWeapon + 700; at += 4)
    if (!std::isfinite(F32(r, at)) || std::fabs(F32(r, at)) > 1000.0f)
      return std::format("the weapon's effect has a value {}", F32(r, at));

  // the weapon: one of its style's
  const int variation = int(U16(r, kWeapon + kWeaponVariation));
  if (!known.weapons.count({style, variation}))
    return std::format("weapon {} of style {} is not on this PC", variation, style);
  return {};
}

}  // namespace sc5::creation
