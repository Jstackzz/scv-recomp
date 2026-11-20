// Tests of what a created character from another PC must pass (src\sc5\creation_check.cpp, src\sc5\net\
// created_transfer.h), with the game's own records (tests\data: an original character and a customized Patroklos, made
// in Creation) and records broken on purpose: each field the check covers set wrong (refused, for the right reason),
// many records garbled at random with their checksum made right (refused or passed, never a crash); the same records
// against the real tables a PC's game read (tests\data\known.txt: a customized fighter in its own costumes only); the
// transfer's chunks in order, out of order, again, and wrong (another count, an out-of-range number, another length,
// other bytes again), then at random.
//
//     creation_check_test <tests\data folder>        (exit code 0 when every test passes)
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "sc5/creation_check.h"
#include "sc5/net/created_transfer.h"

using namespace sc5::creation;
using Record = std::vector<uint8_t>;

namespace {

int g_failed = 0;

void Expect(bool ok, const std::string& what) {
  if (!ok) {
    g_failed++;
    std::printf("FAIL %s\n", what.c_str());
  }
}

Record Load(const std::string& path) {
  Record r;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    r.resize(dna::kSize + 1);
    r.resize(std::fread(r.data(), 1, r.size(), f));
    std::fclose(f);
  }
  return r;
}

void Put32(Record& r, uint32_t at, uint32_t v) {
  r[at] = uint8_t(v >> 24);
  r[at + 1] = uint8_t(v >> 16);
  r[at + 2] = uint8_t(v >> 8);
  r[at + 3] = uint8_t(v);
}
void PutF(Record& r, uint32_t at, float v) {
  uint32_t u;
  std::memcpy(&u, &v, 4);
  Put32(r, at, u);
}
uint32_t Get32(const Record& r, uint32_t at) {
  return uint32_t(r[at]) << 24 | uint32_t(r[at + 1]) << 16 | uint32_t(r[at + 2]) << 8 | r[at + 3];
}
void Seal(Record& r) { Put32(r, dna::kChecksum, Checksum(r.data())); }

// what a PC with the test records' parts has: their own ids, and a few more
Known KnownFor(const std::vector<Record>& good) {
  Known k;
  k.characters = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 31, 32};
  k.styles = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26};
  for (const Record& r : good) {
    const std::pair<int, int> key{r[dna::kType], r[dna::kBaseType]};
    for (uint32_t i = 0; i < dna::kPartsSetCount; i++)
      if (Get32(r, dna::kPartsSets + 4 * i) != 0xFFFFFFFF) k.parts_sets[key].insert(Get32(r, dna::kPartsSets + 4 * i));
    k.parts_sets[key].insert(0x00010001);
    for (uint32_t level = 0; level < dna::kBreakLevelCount; level++)
      for (uint32_t p = 0; p < dna::kPartsPerLevel; p++) {
        const uint32_t at = dna::kBreakLevels + level * dna::kBreakLevelSize + p * 8;
        if (!(r[at] & 0x80)) k.models[key].insert(Get32(r, at));
      }
    k.weapons.insert({r[dna::kStyle], int(r[dna::kWeapon + 2] << 8 | r[dna::kWeapon + 3])});
  }
  k.stickers = {{0x00000001, 0x66000001}, {0x00010002, 0x67000002}};
  for (const Record& r : good)
    for (uint32_t s = 0; s < dna::kStickerCount; s++) {
      const uint32_t at = dna::kStickers + s * dna::kStickerSize;
      if (Get32(r, at) != 0xFFFFFFFF)
        k.stickers[Get32(r, at)] =
            uint32_t(r[at + 16]) << 24 | uint32_t(r[at + 17]) << 16 | uint32_t(r[at + 14]) << 8 | r[at + 15];
    }
  k.palette_rows = 40;
  k.palette_cols = 27;
  return k;
}

struct Case {
  const char* name;
  std::function<void(Record&)> spoil;
  const char* why;  // a piece of the reason the check must give
};

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "tests/data";
  // an original character, a customized Patroklos, an original character with a sticker (Motif 1 on the head)
  const std::vector<Record> good = {Load(dir + "/original.rec"), Load(dir + "/patroklos.rec"),
                                    Load(dir + "/sticker.rec")};
  for (const Record& r : good) {
    if (r.size() != dna::kSize) {
      std::printf("FAIL the test records are not in %s\n", dir.c_str());
      return 1;
    }
  }
  const Known known = KnownFor(good);

  // the game's own records pass, and their checksums are the game's
  for (const Record& r : good) {
    Expect(Checksum(r.data()) == Get32(r, dna::kChecksum), "the checksum of a record from the game");
    const std::string why = CheckRecord(r.data(), r.size(), known);
    Expect(why.empty(), "a record from the game is refused: " + why);
  }

  // each field the check covers, set wrong
  const Case cases[] = {
      {"short", [](Record& r) { r.pop_back(); }, "bytes"},
      {"version", [](Record& r) { r[1] = 10; }, "version"},
      {"checksum", [](Record& r) { r[200] ^= 1; }, "checksum"},
      {"no name", [](Record& r) { std::memset(&r[dna::kNameRelease], 0, dna::kNameLongSize); }, "no name"},
      {"name without end", [](Record& r) { std::memset(&r[dna::kNameRelease], 'A', dna::kNameLongSize); }, "no end"},
      {"name control", [](Record& r) { r[dna::kNameRelease + 1] = 0x0A; }, "control"},
      {"name percent", [](Record& r) { r[dna::kNameRelease + 2] = '%'; }, "'%'"},
      {"name broken UTF-8", [](Record& r) { r[dna::kNameRelease + 1] = 0xC3, r[dna::kNameRelease + 2] = 0x41; },
       "UTF-8"},
      {"name overlong UTF-8", [](Record& r) { r[dna::kNameRelease + 1] = 0xC0, r[dna::kNameRelease + 2] = 0xAF; },
       "UTF-8"},
      {"facial name without end", [](Record& r) { std::memset(&r[dna::kFacialName], 'm', dna::kFacialNameSize); },
       "no end"},
      {"character type", [](Record& r) { r[dna::kType] = 13; }, "character type"},
      {"style", [](Record& r) { r[dna::kStyle] = 40; }, "style"},
      {"weapon style", [](Record& r) { r[dna::kWeapon + 1] = 3; }, "weapon's style"},
      {"original base type", [](Record& r) { r[dna::kType] = 32; r[dna::kBaseType] = 2; }, "base type"},
      {"customized fighter", [](Record& r) { r[dna::kType] = 18; r[dna::kBaseType] = 9; }, "customized fighter"},
      {"attack rate", [](Record& r) { r[dna::kAttackRate] = 201; }, "attack rate"},
      {"pitch", [](Record& r) { r[dna::kPitch] = 51; }, "pitch"},
      {"formant", [](Record& r) { r[dna::kFormant] = uint8_t(-51); }, "formant"},
      {"flanger", [](Record& r) { r[dna::kFlanger] = 3; }, "flanger"},
      {"DLC flag", [](Record& r) { r[dna::kDlcFlag] = 2; }, "DLC"},
      {"shoulders", [](Record& r) { PutF(r, dna::kBody, 0.2f); }, "shoulder"},
      {"shoulders NaN", [](Record& r) { Put32(r, dna::kBody, 0x7FC00000); }, "shoulder"},
      {"hand scale", [](Record& r) { PutF(r, dna::kBody + 16, 0.5f); }, "hand"},
      {"body rate", [](Record& r) { PutF(r, dna::kBody + 40, 1.5f); }, "rate"},
      {"body rate infinite", [](Record& r) { Put32(r, dna::kBody + 60, 0x7F800000); }, "rate"},
      {"parts set", [](Record& r) { Put32(r, dna::kPartsSets + 8, 0x00FF1234); }, "parts set"},
      {"model", [](Record& r) { Put32(r, dna::kBreakLevels + 2 * dna::kBreakLevelSize + 16, 0x02FFFFFF); }, "model"},
      {"sticker", [](Record& r) { Put32(r, dna::kStickers + 5 * dna::kStickerSize, 0x00001234); }, "sticker"},
      {"sticker NaN", [](Record& r) { Put32(r, dna::kStickers + dna::kStickerSize + 24, 0x7FC00000); }, "sticker"},
      {"sticker texture", [](Record& r) {
         Put32(r, dna::kStickers + 7 * dna::kStickerSize, 0x00000001);
         r[dna::kStickers + 7 * dna::kStickerSize + 16] = 113;
       }, "texture"},
      {"weapon sticker", [](Record& r) { Put32(r, dna::kWeapon + dna::kWeaponStickers, 0x00005555); }, "sticker"},
      {"weapon", [](Record& r) { r[dna::kWeapon + 3] = 99; }, "weapon"},
      {"coordinate valid", [](Record& r) { Put32(r, dna::kCoords + 3 * dna::kCoordSize, 2); }, "validity"},
      {"coordinate NaN", [](Record& r) {
         Put32(r, dna::kCoords, 1);
         Put32(r, dna::kCoords + 4, 0x00010001);
         Put32(r, dna::kCoords + 40, 0x7FC00000);
       }, "parts coordinate 0 has a value"},
      {"palette", [](Record& r) { r[dna::kColours + 8] = 4; }, "palette"},
      {"palette row", [](Record& r) { r[dna::kColours + 8] = 1; r[dna::kColours + 10] = 41; }, "off the palette"},
      {"palette column", [](Record& r) { r[dna::kColours + 8] = 1; r[dna::kColours + 11] = 0x80; }, "off the palette"},
      {"saturation", [](Record& r) { r[dna::kColours + 8] = 1; PutF(r, dna::kColours + 12, 1.5f); }, "saturation"},
      {"weapon colour", [](Record& r) { r[dna::kWeapon + dna::kWeaponColours] = 7; }, "weapon's colour"},
      {"trail colour", [](Record& r) { r[dna::kWeapon + dna::kWeaponTrace + 8] = 5; }, "trail's colour"},
      {"trail value", [](Record& r) { Put32(r, dna::kWeapon + dna::kWeaponTrace + 40, 0x7F800000); }, "trail"},
      {"effect colour", [](Record& r) { r[dna::kWeapon + dna::kWeaponEffectInfo] = 6; }, "effect's colour"},
      {"effect value", [](Record& r) { Put32(r, dna::kWeapon + dna::kWeaponEffectInfo + 20, 0x7FC00000); }, "effect"},
      {"sticker colour", [](Record& r) { r[dna::kStickers + 60] = 9; }, "sticker 0's colour"},
  };
  for (const Record& base : good) {
    for (const Case& c : cases) {
      Record r = base;
      c.spoil(r);
      if (std::string(c.name) != "checksum" && r.size() == dna::kSize) Seal(r);
      const std::string why = CheckRecord(r.data(), r.size(), known);
      Expect(why.find(c.why) != std::string::npos,
             std::format("{}: refused for \"{}\", not \"{}\"", c.name, why, c.why));
    }
  }

  // records garbled at random, their checksum made right: each one refused or passed, never a crash; a garbled id the
  // game does not have is refused
  std::mt19937 rnd(1);
  int refused = 0, passed = 0;
  for (int i = 0; i < 20000; i++) {
    Record r = good[i % 2];
    const int spots = 1 + int(rnd() % 8);
    for (int s = 0; s < spots; s++) {
      const uint32_t at = rnd() % dna::kChecksum;
      const uint32_t n = 1 + rnd() % 16;
      for (uint32_t k = 0; k < n && at + k < dna::kChecksum; k++) r[at + k] = uint8_t(rnd());
    }
    if (rnd() % 4) Seal(r);
    const std::string why = CheckRecord(r.data(), r.size(), known);
    (why.empty() ? passed : refused)++;
  }
  std::printf("%d garbled records: %d refused, %d passed\n", refused + passed, refused, passed);

  // against this PC's real tables, as the game read them at the select screen (tests\data\known.txt, written by the
  // game with SC5_TEST_KNOWN_OUT): the game's own records pass, a customized fighter only in its own costumes
  Known real;
  {
    std::string text;
    if (FILE* f = std::fopen((dir + "/known.txt").c_str(), "rb")) {
      char buf[65536];
      for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);
      std::fclose(f);
    }
    Expect(!text.empty() && ParseKnown(text, real), "the real tables (known.txt) are there and read");
    Known again;
    Expect(ParseKnown(KnownText(real), again) && KnownText(again) == KnownText(real), "the real tables to text and back");
    Expect(real.parts_sets.count({18, kFighterBase}) && real.models.count({18, kFighterBase}),
           "Patroklos's own costumes are in the real tables");
    Expect(!ParseKnown("sets 18 5 00130035 zz\n", again) && !ParseKnown("weapons 18\n", again), "broken text is refused");
  }
  for (const Record& r : good) {
    const std::string why = CheckRecord(r.data(), r.size(), real);
    Expect(why.empty(), "a record from the game is refused by the real tables: " + why);
  }
  // the customized Patroklos (character type 18, base type 5: his first costume, parts set 00130035 and model 00350000
  // in part 29 of every break level), changed: what may be and what may not
  auto wear = [](Record& r, uint32_t part, uint32_t model, uint8_t slot) {
    for (uint32_t level = 0; level < dna::kBreakLevelCount; level++) {
      const uint32_t at = dna::kBreakLevels + level * dna::kBreakLevelSize + part * 8;
      Put32(r, at, model);
      r[at + 4] = slot;
    }
  };
  const uint32_t last_set = dna::kPartsSets + 4 * (dna::kPartsSetCount - 1);
  const struct {
    const char* name;
    std::function<void(Record&)> change;
    const char* why;  // a piece of the reason; "" when it passes
  } fighter_cases[] = {
      {"his second costume", [&](Record& r) { Put32(r, last_set, 0x00130135), wear(r, 29, 0x00350100, 0x1F); }, ""},
      // his third does not load in a fight (tools\creationfuzz.py, "fighter3p"): not among his here
      {"his third costume", [&](Record& r) { Put32(r, last_set, 0x00130435), wear(r, 29, 0x00350300, 0x1F); },
       "parts set 00130435"},
      {"his third costume's model", [&](Record& r) { wear(r, 29, 0x00350300, 0x1F); }, "model 00350300"},
      {"Mitsurugi's hair", [&](Record& r) { Put32(r, dna::kPartsSets + 4, 0x02020001), wear(r, 33, 0x01000000, 0x0F); },
       ""},
      {"Mitsurugi's costume", [&](Record& r) { wear(r, 29, 0x00010000, 0x1F); }, "model 00010000"},
      {"Mitsurugi's costume's parts set", [&](Record& r) { Put32(r, last_set, 0x00130001); }, "parts set 00130001"},
      {"an original character's body", [&](Record& r) { wear(r, 0, 0x02000000, 0x00); }, "model 02000000"},
      {"an original character's parts set", [&](Record& r) { Put32(r, dna::kPartsSets, 0x00010008); },
       "parts set 00010008"},
      {"a costume no one has", [&](Record& r) { wear(r, 29, 0x00350700, 0x1F); }, "model 00350700"},
      {"base type 0", [&](Record& r) { r[dna::kBaseType] = 0; },
       "customized fighter (character type 18) with base type 0"},
      {"base type 1", [&](Record& r) { r[dna::kBaseType] = 1; }, "with base type 1"},
      {"Mitsurugi in Patroklos's costume", [&](Record& r) { r[dna::kType] = 0; }, "parts set 00130035"},
      {"a character the select screen lacks", [&](Record& r) { r[dna::kType] = 27; }, "character type 27"},
      {"no parts", [&](Record& r) { wear(r, 29, 0xFFFFFFFF, 0xFF); }, "without its costume in break level 0"},
      {"only hair", [&](Record& r) { wear(r, 29, 0xFFFFFFFF, 0xFF), wear(r, 33, 0x01000000, 0x0F); },
       "without its costume"},
      {"no costume after the first break level", [&](Record& r) {
         for (uint32_t level = 1; level < dna::kBreakLevelCount; level++)
           Put32(r, dna::kBreakLevels + level * dna::kBreakLevelSize + 29 * 8, 0xFFFFFFFF);
       }, "without its costume in break level 1"},
      {"no voice", [&](Record& r) { r[dna::kVoice] = 255; }, ""},
      {"a created character's voice", [&](Record& r) { r[dna::kVoice] = 96; }, ""},
      {"Mitsurugi's voice", [&](Record& r) { r[dna::kVoice] = 1; }, ""},
      {"a voice the game does not know", [&](Record& r) { r[dna::kVoice] = 254; }, "voice 254"},
      {"voice 0", [&](Record& r) { r[dna::kVoice] = 0; }, "voice 0"},
      {"voice 67", [&](Record& r) { r[dna::kVoice] = 67; }, "voice 67"},
  };
  for (const auto& c : fighter_cases) {
    Record r = good[1];
    c.change(r);
    Seal(r);
    const std::string why = CheckRecord(r.data(), r.size(), real);
    Expect(*c.why ? why.find(c.why) != std::string::npos : why.empty(),
           std::format("customized Patroklos, {}: \"{}\", not \"{}\"", c.name, why, *c.why ? c.why : "it passes"));
  }
  {
    Record r = good[0];
    wear(r, 29, 0x00350000, 0x1F);
    Seal(r);
    const std::string why = CheckRecord(r.data(), r.size(), real);
    Expect(why.find("model 00350000") != std::string::npos, "an original character in Patroklos's costume: " + why);
  }
  {
    Record r = good[0];
    r[dna::kVoice] = 254;  // the game takes it for an original character (tools\creationfuzz.py, "header")
    Seal(r);
    const std::string why = CheckRecord(r.data(), r.size(), real);
    Expect(why.empty(), "an original character with voice 254 is refused: " + why);
  }
  // all three garbled at random against the real tables: refused or passed, never a crash
  int real_refused = 0, real_passed = 0;
  for (int i = 0; i < 30000; i++) {
    Record r = good[i % 3];
    const int spots = 1 + int(rnd() % 8);
    for (int s = 0; s < spots; s++) {
      const uint32_t at = rnd() % dna::kChecksum;
      const uint32_t n = 1 + rnd() % 16;
      for (uint32_t k = 0; k < n && at + k < dna::kChecksum; k++) r[at + k] = uint8_t(rnd());
    }
    if (rnd() % 4) Seal(r);
    (CheckRecord(r.data(), r.size(), real).empty() ? real_passed : real_refused)++;
  }
  std::printf("%d garbled records against the real tables: %d refused, %d passed\n", real_refused + real_passed,
              real_refused, real_passed);

  // the transfer: a record in chunks as netplay sends them
  using sc5::net::Incoming;
  using sc5::net::kCreatedChunk;
  using sc5::net::kCreatedChunks;
  const Record& sent = good[0];
  auto chunk_len = [](int c) { return std::min(kCreatedChunk, int(dna::kSize) - c * kCreatedChunk); };
  auto send = [&](Incoming& in, int c, int count = kCreatedChunks, int len = -1, const uint8_t* data = nullptr) {
    in.Take(c, count, data ? data : sent.data() + c * kCreatedChunk, len < 0 ? chunk_len(c) : len);
  };
  {
    Incoming in;
    for (int c = 0; c < kCreatedChunks; c++) send(in, c);
    Expect(in.Whole() && in.Joined() == sent, "a record sent in order is the record");
  }
  {
    Incoming in;
    for (int c = kCreatedChunks - 1; c >= 0; c--) send(in, c), send(in, c);
    Expect(in.Whole() && in.Joined() == sent, "a record sent backwards, each chunk twice, is the record");
  }
  {
    Incoming in;
    for (int c = 0; c < kCreatedChunks - 1; c++) send(in, c);
    Expect(!in.Whole() && in.refused.empty(), "a record without its last chunk is not whole yet");
  }
  const struct {
    const char* name;
    std::function<void(Incoming&)> wrong;
  } transfers[] = {
      {"another chunk count", [&](Incoming& in) { send(in, 0, 13); }},
      {"chunk 12", [&](Incoming& in) { send(in, 12, kCreatedChunks, 140); }},
      {"chunk -1", [&](Incoming& in) { send(in, -1, kCreatedChunks, 1200); }},
      {"a short chunk", [&](Incoming& in) { send(in, 3, kCreatedChunks, 1199); }},
      {"a long last chunk", [&](Incoming& in) { send(in, 11, kCreatedChunks, 1200); }},
      {"a chunk again with other bytes", [&](Incoming& in) {
         send(in, 5);
         Record other(sent.begin() + 5 * kCreatedChunk, sent.begin() + 6 * kCreatedChunk);
         other[7] ^= 0x40;
         send(in, 5, kCreatedChunks, kCreatedChunk, other.data());
       }},
  };
  for (const auto& t : transfers) {
    Incoming in;
    t.wrong(in);
    for (int c = 0; c < kCreatedChunks; c++) send(in, c);  // the right chunks after it change nothing
    Expect(!in.refused.empty() && !in.Whole(), std::format("transfer: {} is refused", t.name));
  }
  // at random: never a crash; whole only with exactly the record's bytes
  int whole = 0, refused_t = 0;
  for (int i = 0; i < 20000; i++) {
    Incoming in;
    if (rnd() % 2) {
      // every chunk in some order, now and then one changed on the way
      int order[kCreatedChunks];
      for (int c = 0; c < kCreatedChunks; c++) order[c] = c;
      std::shuffle(order, order + kCreatedChunks, rnd);
      for (int c : order) {
        Record bytes(sent.begin() + c * kCreatedChunk, sent.begin() + c * kCreatedChunk + chunk_len(c));
        if (rnd() % 24 == 0) bytes[rnd() % bytes.size()] ^= uint8_t(1 + rnd() % 255);
        in.Take(c, kCreatedChunks, bytes.data(), int(bytes.size()));
      }
    }
    const int n = int(rnd() % 40);
    for (int k = 0; k < n; k++) {
      const int c = int(rnd() % 16) - 2, count = rnd() % 8 ? kCreatedChunks : int(rnd() % 20);
      const int len = rnd() % 6 ? (c >= 0 && c < kCreatedChunks ? chunk_len(c) : kCreatedChunk) : int(rnd() % 1300);
      std::vector<uint8_t> data(size_t(std::max(len, 0)) + 1);
      if (c >= 0 && c < kCreatedChunks && len == chunk_len(c))
        std::memcpy(data.data(), sent.data() + c * kCreatedChunk, size_t(len));
      if (rnd() % 10 == 0 && len > 0) data[rnd() % size_t(len)] ^= 1;
      in.Take(c, count, data.data(), len);
    }
    if (in.Whole()) {
      // the right size from right-sized chunks; bytes changed on the way are the checksum's to catch
      whole++;
      const Record got = in.Joined();
      Expect(got.size() == dna::kSize, "a transfer at random is whole at another size");
      if (got != sent) Expect(!CheckForm(got.data(), got.size()).empty(), "a record changed on the way passes");
    }
    if (!in.refused.empty()) refused_t++;
  }
  std::printf("20000 transfers at random: %d whole, %d refused\n", whole, refused_t);

  std::printf(g_failed ? "%d FAILED\n" : "all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
