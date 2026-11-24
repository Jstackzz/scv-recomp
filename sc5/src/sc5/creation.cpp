// Created characters online, see creation.h.
//
// savedata.bin (sc5save:\savedata.bin, 0x22C200 bytes, plain) keeps 50 creation slots from 0x516C, 29,832 bytes apart:
// a 132-byte header, the record (TK_CreationCharacterDNA_st, 13,340 bytes) and the picture Creation takes. A fresh save
// has all 50 with the same empty record and a header of zeros; a created character's header ends with +112 a number,
// +116 1 for a customized fighter (+120 1 for an original character), +124 and +128 0x01000000. In memory the save
// object ([0x8273EF10]) keeps the image from +0x48 (its release table starts right after it, +0x22C248: docs\GAME.md,
// "Unlocks"). Creation's list and the select screen read the slots there as they go.
//
// The record (TK_CreationCharacterDNA_st, the game's schema sub_822256B0) field by field: creation_check.h and
// docs\GAME.md, "Created characters online", with the game's tables this file reads and what the check makes of them.

#include "sc5/creation.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

#include <fmt/format.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "sc5/creation_check.h"
#include "sc5/game.h"
#include "sc5/select.h"

REX_EXTERN(sub_82220B50);  // the creation manager: an entry from a record (r3: where its address is, r4: the record)
REX_EXTERN(sub_8245BED0);  // the release state of an id (r3 the release table, r4 the id): 2 released (unlocks.cpp's)
REX_EXTERN(sub_82221D88);  // the repair pass of a save's record (r3 the record, r4 a byte: 0x10 check its checksum)
// a base type's parts sets from the creation data (r3: a table {type, count, entries}, r4: the base type, r5 and r6:
// its data and size): the entries from the game's heap, the table's old ones freed
REX_EXTERN(sub_8221E190);
REX_EXTERN(sub_8223BC98);  // a base type's data in the creation data's archive (r3: the archive's object, r4: the type)
REX_EXTERN(sub_8223BD28);  // and its size
REX_EXTERN(sub_8266A1B0);  // the game's free (r3: a block of its heap)

namespace sc5::creation {
namespace {

constexpr uint32_t kSaveObject = 0x8273EF10;
constexpr uint32_t kEntrySize = 13544, kEntryCount = 13476;
constexpr uint32_t kImageAt = 0x48;
constexpr uint32_t kImageSize = 0x22C200;
constexpr uint32_t kFirstSlot = 0x516C;
// the header's words a created character sets (offsets in the slot)
constexpr uint32_t kHeaderNumber = 112, kHeaderFighter = 116, kHeaderOriginal = 120, kHeaderSet1 = 124,
                   kHeaderSet2 = 128;
constexpr uint32_t kNameAt = 13245, kNameSize = 45;

uint8_t* Host(uint32_t guest) { return REX_KERNEL_STATE()->memory()->TranslateVirtual<uint8_t*>(guest); }
uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void PutBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}
// a slot's header (its 132 bytes) says a character is there
bool HeaderUsed(const uint8_t* header) { return Be32(header + kHeaderSet2) == 0x01000000; }

std::string Name(const Record& r) {
  std::string n;
  for (uint32_t i = 0; i < kNameSize && r[kNameAt + i]; i++) n += char(r[kNameAt + i]);
  return n;
}

// What this PC's game can load (ReadKnown), and which of it comes from downloadable content
Known g_known;
bool g_known_read = false;
// DropRefused has run (with the creation manager there), and the player's own slots it took out: their manager
// entries stay empty (SyncEntries)
bool g_dropped = false;
std::set<uint32_t> g_refused;
uint32_t g_dlc_fingerprint = 0;
std::set<uint32_t> g_dlc_parts_sets, g_dlc_models;
std::set<std::pair<int, int>> g_dlc_weapons;
std::set<uint32_t> g_dlc_stickers;

// The stickers Creation offers (the online game never loads Creation's own table): id, release, texture
struct Sticker {
  uint32_t id, release, olt, olt_relation, olt_key;
};
constexpr Sticker kStickers[] = {
#include "sc5/creation_stickers.inc"
};

// The creation context of the select screen ([object + 1264], built by sub_8223BE80 and sub_8223BF60): +12 the
// parts sets of each base type (6 x {type, count, entries of 2,496 bytes}, from its builder's +14788), +28 the
// builder (14,936 bytes; +14860 the weapons' count, +14864 their entries of 160 bytes)
constexpr uint32_t kContextAt = 1264, kContextTables = 12, kContextBuilder = 28;
constexpr uint32_t kBuilderTables = 14788, kBuilderWeaponCount = 14860, kBuilderWeapons = 14864;
constexpr uint32_t kTableCount = 6, kTableSize = 12;
// a parts set (CCreationPartsSetInfo_st): its id, its release, its members (count, then 14 of 9 models: the
// default, 8 variations; each a ModelData and its slot, 8 bytes, none when the first byte is negative)
constexpr uint32_t kSetSize = 2496, kSetRelease = 16, kSetMemberCount = 20, kSetMembers = 24, kMemberSize = 72;
constexpr uint32_t kMaxMembers = 14, kModelsPerMember = 9;
// a weapon (CWeaponInfo_st): its style, variation, release
constexpr uint32_t kWeaponSize = 160, kWeaponRelease = 12;
// the save object's release table (after the save image): a nibble per id, its low 3 bits the state (3:
// downloadable content); sub_8245BED0 answers whether an id is released (unlocks.cpp opens everything else)
constexpr uint32_t kReleaseTable = 0x22C248, kReleaseIds = 0x1D02;
constexpr uint32_t kPalette = 0x82767918;  // the colour palette's header

uint32_t RawRelease(uint32_t id) {
  const uint32_t save = game::ReadU32(kSaveObject);
  if (!save || id >= kReleaseIds) return 0;
  const uint8_t byte = *Host(save + kReleaseTable + (id >> 1));
  return ((id & 1) ? byte >> 4 : byte & 0xF) & 7;
}

// the game's own answer for a release id (no release: 0xFFFF): released on this PC
bool Released(PPCContext& ctx, uint8_t* base, uint32_t id) {
  if (id == 0xFFFF) return true;
  const uint32_t save = game::ReadU32(kSaveObject);
  if (!save) return false;
  const PPCContext saved = ctx;
  ctx.r3.u64 = save + kReleaseTable;
  ctx.r4.u64 = id & 0xFFFF;
  ctx.lr = 0x82189268;  // as from main's loop
  sub_8245BED0(ctx, base);
  const uint32_t state = ctx.r3.u32;
  ctx = saved;
  return (state & 7) == 2;
}

// FNV-1a, a word at a time
uint32_t Fnv(uint32_t h, uint32_t v) {
  for (int i = 0; i < 4; i++) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 16777619u;
  return h;
}

// The fighters' costumes, base type 5. The builder keeps the creation data's archive ([+14868]'s object, its data at
// +12: a count of files, then their offsets) with every base type's parts sets in it, but the select screen reads
// only base types 0 and 1 into its tables; Creation reads 5 as well (sub_82249BB0). In the test PC's game base type 5
// has 118 parts sets, none downloadable: 89 of category 19 (+4), each a fighter's whole costume (id 0x0013vvcc, cc
// the fighter's costume code and vv the costume, with one model 0x00ccvv'00 in slot 31), and the 29 of category 21
// that every base type's table has, the fighters' hair (0x020200cc, models 0x01nn0000 in slot 15). Of the costumes
// only the first two (vv 0 and 1, the select screen's own two) load in a fight: with the third (vv 4, model variation
// 3; Patroklos's 0x00130435, model 0x00350300) both games gave up loading the fight (tools\creationfuzz.py,
// "fighter3p", 2026-11-20); vv 5 (Nightmare's and Cervantes's) is left out with it.
constexpr uint32_t kBuilderArchive = 14868;
constexpr uint32_t kSetCategory = 4, kCostumeCategory = 19, kHairCategory = 21;
constexpr uint32_t kFightCostumes = 2;  // vv below it
// the game's characters (sub_823507F0's table, 33 entries of 36 bytes): the character type at +0, its costume code at
// +4 (which its models' and templates' names carry: Patroklos, 18, has 0x35)
constexpr uint32_t kCharacterTable = 0x82743828, kCharacterCount = 33, kCharacterSize = 36;

// a call into the game's code from the frame's top (main thread): its r3 after
template <typename F>
uint32_t Call(PPCContext& ctx, uint8_t* base, F fn, uint32_t r3, uint32_t r4, uint32_t r5 = 0, uint32_t r6 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  ctx.lr = 0x82189268;  // as from main's loop
  fn(ctx, base);
  const uint32_t r = ctx.r3.u32;
  ctx = saved;
  return r;
}

// a fighter's costume code (-1: not in the game's table)
int CostumeCode(int type) {
  for (uint32_t i = 0; i < kCharacterCount; i++) {
    const uint32_t e = kCharacterTable + i * kCharacterSize;
    if (game::ReadU32(e) == uint32_t(type)) return int(game::ReadU32(e + 4) & 0xFF);
  }
  return -1;
}

// Every customized fighter's own parts into k ((its type, 5) for each fighter in k.characters): base type 5's parts
// sets read with the game's own loader into a table of ours (in the SDK's heap: its entries come from the game's heap
// and go back to it here), each fighter's own two costumes (its code in the set's id) and the hair any fighter may
// wear.
// Downloadable ones (none on the test PC) go into dlc_sets, dlc_models and the fingerprint. The number of parts sets
// read; 0 when the data is not there (customized fighters are then refused).
uint32_t ReadFighters(PPCContext& ctx, uint8_t* base, uint32_t builder, Known& k, std::set<uint32_t>& dlc_sets,
                      std::set<uint32_t>& dlc_models, uint32_t& fp) {
  const uint32_t archive = game::ReadU32(builder + kBuilderArchive);
  const uint32_t data = archive ? Call(ctx, base, sub_8223BC98, archive, kFighterBase) : 0;
  const uint32_t size = data ? Call(ctx, base, sub_8223BD28, archive, kFighterBase) : 0;
  static const uint32_t table = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(16, 0x20);
  if (!data || !size || size > (16u << 20) || !table) return 0;
  PutBe32(Host(table), 0xFFFFFFFF);  // {type, count, entries}: none yet
  PutBe32(Host(table + 4), 0);
  PutBe32(Host(table + 8), 0);
  Call(ctx, base, sub_8221E190, table, kFighterBase, data, size);
  const uint32_t n = game::ReadU32(table + 4), e = game::ReadU32(table + 8);
  struct Parts {
    std::set<uint32_t> sets, models;
  };
  std::map<int, Parts> costumes;  // a fighter's costume code -> its own
  Parts hair;
  uint32_t read = 0;
  for (uint32_t i = 0; e && game::ReadU32(table) == kFighterBase && i < n && i < 5000; i++) {
    const uint32_t set = e + i * kSetSize, id = game::ReadU32(set), release = game::ReadU32(set + kSetRelease);
    const uint8_t category = *Host(set + kSetCategory);
    if ((category != kCostumeCategory && category != kHairCategory) || !Released(ctx, base, release)) continue;
    if (category == kCostumeCategory && ((id >> 8) & 0xFF) >= kFightCostumes) continue;  // not loaded in a fight
    const bool dlc = release != 0xFFFF && RawRelease(release) == 3;
    Parts& to = category == kHairCategory ? hair : costumes[int(id & 0xFF)];
    to.sets.insert(id);
    read++;
    if (dlc) {
      dlc_sets.insert(id);
      fp = Fnv(Fnv(fp, 4), id);
    }
    const uint32_t members = std::min(game::ReadU32(set + kSetMemberCount), kMaxMembers);
    for (uint32_t m = 0; m < members; m++) {
      for (uint32_t v = 0; v < kModelsPerMember; v++) {
        const uint32_t model = game::ReadU32(set + kSetMembers + m * kMemberSize + v * 8);
        if (model & 0x80000000u) continue;  // none
        to.models.insert(model);
        if (dlc) dlc_models.insert(model);
      }
    }
  }
  if (e) Call(ctx, base, sub_8266A1B0, e, 0);
  PutBe32(Host(table + 8), 0);
  for (const int type : k.characters) {
    const auto own = type == kOriginal ? costumes.end() : costumes.find(CostumeCode(type));
    if (own == costumes.end() || own->second.models.empty()) continue;
    auto& sets = k.parts_sets[{type, kFighterBase}];
    auto& models = k.models[{type, kFighterBase}];
    sets = own->second.sets;
    sets.insert(hair.sets.begin(), hair.sets.end());
    models = own->second.models;
    models.insert(hair.models.begin(), hair.models.end());
  }
  return read;
}

// tests: SC5_TEST_KNOWN_OUT=<file>: what this PC's game has, as text (creation_check_test's tests\data\known.txt)
void WriteKnown(const Known& k) {
  const char* path = std::getenv("SC5_TEST_KNOWN_OUT");
  if (!path || !*path) return;
  const std::string text = KnownText(k);
  FILE* f = std::fopen(path, "wb");
  const bool ok = f && std::fwrite(text.data(), 1, text.size(), f) == text.size();
  if (f) std::fclose(f);
  REXLOG_INFO("sc5 creation: SC5_TEST_KNOWN_OUT: this PC's tables {} {}", ok ? "written to" : "could not be written to",
              path);
}

}  // namespace

uint32_t SaveImage() {
  const uint32_t save = game::ReadU32(kSaveObject);
  if (!save) return 0;
  const uint32_t image = save + kImageAt;
  // the image's own header: its version and size (00010000 0022C200)
  if (game::ReadU32(image) != 0x00010000 || game::ReadU32(image + 4) != kImageSize) return 0;
  return image;
}

uint32_t Slot(uint32_t n) {
  const uint32_t image = SaveImage();
  return image && n < kSlots ? image + kFirstSlot + n * kSlotSize : 0;
}

bool Used(uint32_t n) {
  const uint32_t s = Slot(n);
  return s && HeaderUsed(Host(s));
}

int TypeOf(const uint8_t* record) { return record[9]; }

namespace {
// the used slots whose record joins `character`'s costume panel, in slot order (the panel's order)
std::vector<int> CreatedOf(int character) {
  std::vector<int> created;
  for (uint32_t n = 0; n < kSlots; n++)
    if (Used(n) && TypeOf(Host(Slot(n) + kRecordAt)) == character) created.push_back(int(n));
  return created;
}
}  // namespace

int SlotOfPick(int character, int costume, int costumes) {
  const std::vector<int> created = CreatedOf(character);
  const int standard = costumes - int(created.size());
  if (costume < standard || costume >= costumes || standard < 0) return -1;
  return created[size_t(costume - standard)];
}

int CostumeOfSlot(int character, uint32_t n, int costumes) {
  const std::vector<int> created = CreatedOf(character);
  const int standard = costumes - int(created.size());
  for (size_t i = 0; i < created.size(); i++)
    if (created[i] == int(n)) return standard >= 0 ? standard + int(i) : -1;
  return -1;
}

int SlotOfChecksum(uint32_t checksum) {
  for (uint32_t n = 0; n < RemoteSlot(0); n++)
    if (Used(n) && Be32(Host(Slot(n) + kRecordAt + dna::kChecksum)) == checksum) return int(n);
  return -1;
}

uint32_t ChecksumOf(const Record& r) { return r.size() == kRecordSize ? Be32(&r[dna::kChecksum]) : 0; }

bool ReadRecord(uint32_t n, Record& out) {
  if (!Used(n)) return false;
  const uint8_t* p = Host(Slot(n) + kRecordAt);
  out.assign(p, p + kRecordSize);
  return true;
}

// Tests: SC5_TEST_CREATION_UNCHECKED=1 takes any record of the right size (fuzzing the game's own handling); =check
// skips only this file's check, the game's own repair pass still runs (testing that one alone)
namespace {
const char* TestUnchecked() {
  static const char* v = std::getenv("SC5_TEST_CREATION_UNCHECKED");
  return v;
}
}  // namespace

std::string Check(const Record& r) {
  if (TestUnchecked()) {
    REXLOG_WARN("sc5 creation: SC5_TEST_CREATION_UNCHECKED: a record taken without the check (tests only)");
    return r.size() == kRecordSize ? "" : "not a record";
  }
  if (!g_known_read) return "this PC has not read its own tables yet";
  return CheckRecord(r.data(), r.size(), g_known);
}

bool KnownRead() { return g_known_read && g_dropped; }

std::string GameRepairCheck(PPCContext& ctx, uint8_t* base, const Record& r) {
  if (TestUnchecked() && std::strcmp(TestUnchecked(), "check") != 0) return {};
  // the copy, and before it the byte of the pass's flags (0x10: check the checksum, refuse a wrong one)
  static const uint32_t scratch = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(16 + kRecordSize, 0x20);
  if (!scratch || r.size() != kRecordSize) return "there is no room to try the game's repair pass in";
  uint8_t* p = Host(scratch);
  std::memset(p, 0, 16);
  p[0] = 0x10;
  std::memcpy(p + 16, r.data(), kRecordSize);
  const PPCContext saved = ctx;
  ctx.r3.u64 = scratch + 16;
  ctx.r4.u64 = scratch;
  ctx.lr = 0x82189268;  // as from main's loop
  sub_82221D88(ctx, base);
  const uint32_t took = ctx.r3.u32;  // 0: a wrong checksum, or no character (base type -1)
  ctx = saved;
  if (!took) return "the game's own repair pass refuses it";
  if (std::memcmp(p + 16, r.data(), kRecordSize) != 0) return "the game's own repair pass would change it";
  return {};
}

bool ReadKnown(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  if (g_known_read) return true;
  const uint32_t c = obj ? game::ReadU32(obj + kContextAt) : 0;
  const uint32_t d = c ? game::ReadU32(c + kContextBuilder) : 0;
  const uint32_t tables = c ? game::ReadU32(c + kContextTables) : 0;
  // the context is whole (sub_8223BF60) and its tables are its builder's
  if (!d || tables != d + kBuilderTables || !SaveImage()) return false;
  Known k;
  std::set<uint32_t> dlc_sets, dlc_models;
  uint32_t fp = 2166136261u, sets = 0, models = 0;
  for (uint32_t t = 0; t < kTableCount; t++) {
    const uint32_t type = game::ReadU32(tables + t * kTableSize), n = game::ReadU32(tables + t * kTableSize + 4),
                   e = game::ReadU32(tables + t * kTableSize + 8);
    if (type == 0xFFFFFFFF || !n || !e) continue;
    if (type > 255 || n > 5000) return false;  // not the tables
    if (type > 1) continue;  // an original character's are base types 0 and 1 (a customized fighter's: ReadFighters)
    for (uint32_t i = 0; i < n; i++) {
      const uint32_t set = e + i * kSetSize, id = game::ReadU32(set), release = game::ReadU32(set + kSetRelease);
      if (!Released(ctx, base, release)) continue;
      const bool dlc = release != 0xFFFF && RawRelease(release) == 3;
      k.parts_sets[{kOriginal, int(type)}].insert(id);
      sets++;
      if (dlc) {
        dlc_sets.insert(id);
        fp = Fnv(Fnv(fp, 1), id);
      }
      const uint32_t members = std::min(game::ReadU32(set + kSetMemberCount), kMaxMembers);
      for (uint32_t m = 0; m < members; m++) {
        for (uint32_t v = 0; v < kModelsPerMember; v++) {
          const uint32_t at = set + kSetMembers + m * kMemberSize + v * 8;
          const uint32_t model = game::ReadU32(at);
          if (model & 0x80000000u) continue;  // none
          if (k.models[{kOriginal, int(type)}].insert(model).second) models++;
          if (dlc) dlc_models.insert(model);
        }
      }
    }
  }
  if (!sets) return false;  // not loaded yet
  const uint32_t wn = game::ReadU32(d + kBuilderWeaponCount), we = game::ReadU32(d + kBuilderWeapons);
  if (!wn || wn > 2000 || !we) return false;
  std::set<std::pair<int, int>> dlc_weapons;
  for (uint32_t i = 0; i < wn; i++) {
    const uint32_t w = we + i * kWeaponSize, release = game::ReadU32(w + kWeaponRelease);
    const int style = int(game::ReadU32(w)), variation = int(game::ReadU32(w + 4));
    if (style < 0 || style > 255 || variation < 0 || variation > 255 || !Released(ctx, base, release)) continue;
    k.weapons.insert({style, variation});
    k.styles.insert(style);  // a style Creation offers has its weapons
    if (release != 0xFFFF && RawRelease(release) == 3) {
      dlc_weapons.insert({style, variation});
      fp = Fnv(Fnv(fp, 2), uint32_t(style) << 16 | uint32_t(variation));
    }
  }
  // the colour palette's size (sub_8222C450 reads it at [0x82767918]: +8 the last row, +10 the last column); the game
  // checks a colour's place against it itself, so without it any place up to 63 does
  const uint32_t palette = game::ReadU32(kPalette);
  k.palette_rows = palette ? int(game::ReadU32(palette + 8) >> 16) : 63;
  k.palette_cols = palette ? int(game::ReadU32(palette + 8) & 0xFFFF) : 63;
  // the characters: the select screen's (Dampierre only with his package), and original characters
  for (int s = 0; s < select::kSlots; s++)
    if (select::Pickable(s) && s != select::kCreationSlot) k.characters.insert(select::IdOfSlot(s));
  k.characters.insert(kOriginal);
  std::set<uint32_t> dlc_stickers;
  for (const Sticker& st : kStickers) {
    if (!Released(ctx, base, st.release)) continue;
    k.stickers[st.id] = st.olt << 24 | st.olt_relation << 16 | st.olt_key;
    if (RawRelease(st.release) == 3) {
      dlc_stickers.insert(st.id);
      fp = Fnv(Fnv(fp, 3), st.id);
    }
  }
  // the customized fighters' own costumes (base type 5), which the select screen does not read itself
  const uint32_t fighter_sets = ReadFighters(ctx, base, d, k, dlc_sets, dlc_models, fp);
  int fighters = 0;
  for (const auto& [key, ids] : k.parts_sets) fighters += key.first != kOriginal;
  if (!fighters) REXLOG_WARN("sc5 creation: no fighters' costumes (base type 5) here: customized fighters stay out");
  WriteKnown(k);  // tests: SC5_TEST_KNOWN_OUT
  g_known = std::move(k);
  g_dlc_stickers = std::move(dlc_stickers);
  g_dlc_parts_sets = std::move(dlc_sets);
  g_dlc_models = std::move(dlc_models);
  g_dlc_weapons = std::move(dlc_weapons);
  g_dlc_fingerprint = fp;
  g_known_read = true;
  REXLOG_INFO("sc5 creation: this PC's parts for created characters: {} parts sets, {} models, {} weapons in {} "
              "styles, {} stickers, {} characters, a palette of {}x{}; {} fighters' own costumes ({} parts sets of base "
              "type 5); downloadable: {} sets, {} weapons, {} stickers (fingerprint {:08X})",
              sets, models, g_known.weapons.size(), g_known.styles.size(), g_known.stickers.size(),
              g_known.characters.size(), g_known.palette_rows + 1, g_known.palette_cols + 1, fighters, fighter_sets,
              g_dlc_parts_sets.size(), g_dlc_weapons.size(), g_dlc_stickers.size(), fp);
  return true;
}

uint32_t DlcFingerprint() { return g_known_read ? g_dlc_fingerprint : 0; }

bool UsesDlc(const Record& r) {
  if (r.size() != kRecordSize) return false;
  for (uint32_t i = 0; i < dna::kPartsSetCount; i++)
    if (g_dlc_parts_sets.count(Be32(&r[dna::kPartsSets + 4 * i]))) return true;
  for (uint32_t level = 0; level < dna::kBreakLevelCount; level++)
    for (uint32_t p = 0; p < dna::kPartsPerLevel; p++) {
      const uint32_t at = dna::kBreakLevels + level * dna::kBreakLevelSize + p * 8;
      if (!(r[at] & 0x80) && g_dlc_models.count(Be32(&r[at]))) return true;
    }
  for (uint32_t k = 0; k < dna::kStickerCount + dna::kWeaponStickerCount; k++) {
    const uint32_t at = k < dna::kStickerCount
                            ? dna::kStickers + k * dna::kStickerSize
                            : dna::kWeapon + dna::kWeaponStickers + (k - dna::kStickerCount) * dna::kStickerSize;
    if (g_dlc_stickers.count(Be32(&r[at]))) return true;
  }
  const int style = r[dna::kStyle], variation = int(r[dna::kWeapon + 2] << 8 | r[dna::kWeapon + 3]);
  return g_dlc_weapons.count({style, variation}) > 0;
}

void DropRefused(PPCContext& ctx, uint8_t* base, uint32_t manager) {
  // once the tables are read and the creation manager is there: the select screen sets it a few frames in, and an
  // entry left filled stays on the costume panel (sub_82221408 lists every entry with parts), which shifts every later
  // costume's slot
  if (g_dropped || !g_known_read || !manager) return;
  g_dropped = true;
  if (std::getenv("SC5_TEST_CREATION_UNCHECKED")) return;  // tests: the game's own handling of whatever is there
  for (uint32_t n = 0; n < RemoteSlot(0); n++) {
    Record r;
    if (!ReadRecord(n, r)) continue;
    // the check, then the game's own repair pass on a copy
    std::string why = Check(r);
    if (why.empty()) why = GameRepairCheck(ctx, base, r);
    if (why.empty()) continue;
    PutBe32(Host(Slot(n)) + kHeaderSet2, 0);  // no screen lists it from here on
    PutBe32(Host(manager + 4 + n * kEntrySize + kEntryCount), 0);
    g_refused.insert(n);
    REXLOG_WARN("sc5 creation: slot {} ({}) stays out online: {}", n + 1, Name(r), why);
  }
}

namespace {

// The card the VS screen shows for a created character from the other PC. The VS screen (from its copy of the slot's
// picture, 0x82909C60 for player 2's) and Creation's list draw a character's card from the picture in its slot: a file
// from +13,576 (16 bytes: its kind, a byte the kind uses, its size, width and height, a checksum of its data checked
// when the kind has 0x80; then its data), which sub_8218B1C0 loads when its size is not 0 and sub_8218B298 uploads.
// Creation keeps the character it shot there as kind 0x80 (172x256, a compressed "PTC+MSHM" file of up to 16,240
// bytes), which does not come with the record, so the card was black. In its place a picture of our own: SCV Online's
// navy and gold, a figure with a question mark, made once.
//
// Its form is the loader's kind 2 (sub_8218B860; byte 1 = 3: an A8R8G8B8 texture, linear, one of the forms
// sub_8218A238 takes), whose data the upload decodes (sub_824FFA68): a 10-byte little-endian header (width, row pitch in
// bytes, height, flags: 0x18 four channels, 0x20 each coded on its own with no colour transform), then each channel (A,
// R, G, B: the texture's bytes in that order) as sub_824FF048 reads it: a 64-bit little-endian mask of the coefficients
// present, in the codec's order (the game's table at 0x82702660: k -> row * 8 + column), a byte added after the inverse
// DCT, 64 quantization multipliers (row-major), then each present coefficient's quantized value for every 8x8 block in
// turn (signed bytes; 0x80 n repeats the last value n times). The inverse DCT is MPEG's reference integer one
// (sub_82503F28), so the forward one here is the standard DCT. 128x192 (rows of 32 texels), which the VS screen
// stretches over the card as it does Creation's 172x256; JPEG's luminance quantization, coarser until the file fits.
constexpr uint32_t kCardAt = 13576;
constexpr int kCardW = 128, kCardH = 192;
constexpr uint32_t kZigzag = 0x82702660;

struct Rgb {
  float r, g, b;
};

// the card at a point of it, in Creation's 172x256
Rgb CardAt(float x, float y) {
  constexpr Rgb kGold{226, 182, 82}, kDarkGold{150, 118, 52}, kFigure{200, 210, 230}, kInk{20, 32, 70};
  constexpr float kW = 172, kH = 256, kHeadX = 86, kHeadY = 96, kHeadR = 34, kBodyY = 236, kBodyRx = 70, kBodyRy = 82;
  const float edge = std::min(std::min(x, kW - x), std::min(y, kH - y));
  if (edge < 6) return kGold;
  if (edge >= 10 && edge < 12) return kDarkGold;
  if (edge >= 10) {
    // a question mark over the head: an arc from the left over the top down to below its middle, a stem, a dot
    const float dx = x - kHeadX, dy = y - 84, r = std::sqrt(dx * dx + dy * dy);
    if (r >= 8 && r <= 16 && std::atan2(-dy, dx) * 57.29578f >= -90) return kInk;
    if (x >= kHeadX - 4 && x <= kHeadX + 4 && y >= 92 && y <= 106) return kInk;
    if (dx * dx + (y - 115) * (y - 115) <= 5.5f * 5.5f) return kInk;
    const float hy = y - kHeadY, bx = dx / kBodyRx, by = (y - kBodyY) / kBodyRy;
    if (dx * dx + hy * hy <= kHeadR * kHeadR || bx * bx + by * by <= 1) return kFigure;
  }
  const float t = y / kH;  // a navy gradient, lighter at the top
  return {28 - 18 * t, 44 - 30 * t, 92 - 56 * t};
}

void PutLe16(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x));
  v.push_back(uint8_t(x >> 8));
}

// one channel of the card (kCardW x kCardH values) in the codec, onto `out`
void EncodeChannel(const std::vector<float>& plane, const uint8_t* q, const uint32_t* zigzag, std::vector<uint8_t>& out) {
  constexpr int kAcross = kCardW / 8, kBlocks = kAcross * (kCardH / 8);
  static const std::array<float, 64> cosines = [] {
    std::array<float, 64> c{};
    for (int u = 0; u < 8; u++)
      for (int x = 0; x < 8; x++) c[size_t(u * 8 + x)] = std::cos(float((2 * x + 1) * u) * 3.14159265f / 16);
    return c;
  }();
  std::vector<std::array<int8_t, 64>> coefficients(kBlocks);
  for (int b = 0; b < kBlocks; b++) {
    const int left = (b % kAcross) * 8, top = (b / kAcross) * 8;
    for (int v = 0; v < 8; v++) {
      for (int u = 0; u < 8; u++) {
        float sum = 0;
        for (int y = 0; y < 8; y++)
          for (int x = 0; x < 8; x++)
            sum += (plane[size_t((top + y) * kCardW + left + x)] - 128) * cosines[size_t(u * 8 + x)] *
                   cosines[size_t(v * 8 + y)];
        const float f = 0.25f * (u ? 1 : 0.70710678f) * (v ? 1 : 0.70710678f) * sum / q[v * 8 + u];
        coefficients[size_t(b)][size_t(v * 8 + u)] = int8_t(std::clamp(int(std::lround(f)), -127, 127));
      }
    }
  }
  uint64_t mask = 0;
  std::vector<uint8_t> planes;
  for (int k = 0; k < 64; k++) {
    const uint32_t at = zigzag[k];
    bool any = false;
    for (const auto& c : coefficients) any = any || c[at] != 0;
    if (!any) continue;
    mask |= uint64_t(1) << k;
    int8_t last = 0;
    for (int b = 0; b < kBlocks;) {
      if (coefficients[size_t(b)][at] != last) {
        last = coefficients[size_t(b)][at];
        planes.push_back(uint8_t(last));
        b++;
        continue;
      }
      int n = 0;
      for (; b < kBlocks && coefficients[size_t(b)][at] == last && n < 255; b++) n++;
      planes.push_back(0x80);
      planes.push_back(uint8_t(n));
    }
  }
  for (int i = 0; i < 8; i++) out.push_back(uint8_t(mask >> (8 * i)));
  out.push_back(128);
  out.insert(out.end(), q, q + 64);
  out.insert(out.end(), planes.begin(), planes.end());
}

// the card's picture file, made once (empty if it does not fit)
const std::vector<uint8_t>& Card() {
  static std::vector<uint8_t> file;
  static bool made = false;
  if (made) return file;
  made = true;
  uint32_t zigzag[64];
  uint64_t seen = 0;
  for (int k = 0; k < 64; k++) {
    zigzag[k] = game::ReadU32(kZigzag + 4 * uint32_t(k));
    if (zigzag[k] < 64) seen |= uint64_t(1) << zigzag[k];
  }
  if (seen != ~uint64_t(0) || zigzag[0] != 0) {  // not the codec's order: another build of the game
    REXLOG_WARN("sc5 creation: the picture codec's coefficient order is not at {:08X}: no VS card", kZigzag);
    return file;
  }
  // the card at its size, each texel the average of 16 points of it
  std::vector<float> planes[3];
  for (auto& p : planes) p.resize(size_t(kCardW * kCardH));
  for (int y = 0; y < kCardH; y++) {
    for (int x = 0; x < kCardW; x++) {
      Rgb sum{0, 0, 0};
      for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
          const Rgb c = CardAt((x + (sx + 0.5f) / 4) * 172 / kCardW, (y + (sy + 0.5f) / 4) * 256 / kCardH);
          sum = {sum.r + c.r, sum.g + c.g, sum.b + c.b};
        }
      }
      planes[0][size_t(y * kCardW + x)] = sum.r / 16;
      planes[1][size_t(y * kCardW + x)] = sum.g / 16;
      planes[2][size_t(y * kCardW + x)] = sum.b / 16;
    }
  }
  static constexpr uint8_t kJpeg[64] = {16, 11, 10, 16, 24,  40,  51,  61,  12, 12, 14, 19, 26,  58,  60,  55,
                                        14, 13, 16, 24, 40,  57,  69,  56,  14, 17, 22, 29, 51,  87,  80,  62,
                                        18, 22, 37, 56, 68,  109, 103, 77,  24, 35, 55, 64, 81,  104, 113, 92,
                                        49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
  for (const float scale : {1.0f, 1.5f, 2.0f, 3.0f, 4.0f}) {
    uint8_t q[64];
    for (int i = 0; i < 64; i++) q[i] = uint8_t(std::clamp(int(std::lround(kJpeg[i] * scale)), 1, 255));
    std::vector<uint8_t> data(16, 0);
    PutLe16(data, kCardW);
    PutLe16(data, kCardW * 4);
    PutLe16(data, kCardH);
    PutLe16(data, 0x38);
    PutLe16(data, 0);
    // alpha: no coefficients, 255 everywhere
    data.insert(data.end(), 8, uint8_t(0));
    data.push_back(255);
    data.insert(data.end(), 64, uint8_t(1));
    for (const auto& p : planes) EncodeChannel(p, q, zigzag, data);
    if (data.size() > kSlotSize - kCardAt) continue;
    data[0] = 2;  // kind 2, no checksum
    data[1] = 3;  // A8R8G8B8
    data[2] = uint8_t(data.size() >> 8);
    data[3] = uint8_t(data.size());
    data[5] = kCardW;
    data[7] = kCardH;
    file = std::move(data);
    REXLOG_INFO("sc5 creation: the VS card for the other PC's created characters: {} bytes (quantization x{})",
                file.size(), scale);
    return file;
  }
  REXLOG_WARN("sc5 creation: the VS card for the other PC's created characters does not fit its slot");
  return file;
}

}  // namespace

bool PlaceRemote(int side, const Record& r) {
  const uint32_t s = Slot(RemoteSlot(side));
  if (!s || !Check(r).empty()) return false;
  uint8_t* slot = Host(s);
  std::memset(slot, 0, kSlotSize);
  // the other PC's picture does not come: ours on its card
  if (const std::vector<uint8_t>& card = Card(); !card.empty()) std::memcpy(slot + kCardAt, card.data(), card.size());
  std::memcpy(slot + kRecordAt, r.data(), kRecordSize);
  PutBe32(slot + kHeaderNumber, 1);
  PutBe32(slot + (TypeOf(r.data()) == kOriginal ? kHeaderOriginal : kHeaderFighter), 1);
  PutBe32(slot + kHeaderSet1, 0x01000000);
  PutBe32(slot + kHeaderSet2, 0x01000000);  // last: from here on the screens list it
  REXLOG_INFO("sc5 creation: player {}'s {} from the other PC in slot {} (type {}, style {})", side + 1, Name(r),
              RemoteSlot(side) + 1, TypeOf(r.data()), r[16]);
  return true;
}

void ClearRemote() {
  for (int side = 0; side < 2; side++) {
    const uint32_t s = Slot(RemoteSlot(side));
    if (!s) return;
    uint8_t* slot = Host(s);
    PutBe32(slot + kHeaderSet2, 0);  // first: no screen lists it from here on
    std::memset(slot, 0, kRecordAt);
  }
}

bool FillEntry(PPCContext& ctx, uint8_t* base, uint32_t manager, uint32_t n) {
  if (!manager || n >= kSlots || !Used(n)) return false;
  const uint32_t entry = manager + 4 + n * kEntrySize;
  const uint32_t holder = Slot(n) + kPictureAt;  // 4 bytes of the slot's picture (it has none) hold the entry's address
  PutBe32(Host(holder), entry);
  const PPCContext saved = ctx;
  ctx.r3.u64 = holder;
  ctx.r4.u64 = Slot(n) + kRecordAt;
  ctx.lr = 0x82189268;  // as from main's loop
  sub_82220B50(ctx, base);
  ctx = saved;
  PutBe32(Host(holder), 0);
  REXLOG_INFO("sc5 creation: the manager's entry {} filled (parts {}, type {})", n + 1,
              game::ReadU32(entry + kEntryCount), game::ReadU32(entry + 13480));
  return true;
}

void SyncEntries(uint32_t manager) {
  if (!manager) return;
  for (int side = 0; side < 2; side++) {
    const uint32_t n = RemoteSlot(side), count = manager + 4 + n * kEntrySize + kEntryCount;
    if (Used(n) || !game::ReadU32(count)) continue;
    PutBe32(Host(count), 0);
    REXLOG_INFO("sc5 creation: the manager's entry {} (an earlier match's) dropped", n + 1);
  }
  // the player's own characters the check refused: their entries stay empty, should the game fill them again
  for (const uint32_t n : g_refused) {
    const uint32_t count = manager + 4 + n * kEntrySize + kEntryCount;
    if (!game::ReadU32(count)) continue;
    PutBe32(Host(count), 0);
    REXLOG_INFO("sc5 creation: the manager's entry {} (refused here) dropped again", n + 1);
  }
}

namespace {

// a whole save file, read only and shared with any writer (the player's own save: never written, never locked)
bool ReadWhole(const char* utf8, std::vector<uint8_t>& out) {
  const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
  std::wstring path(size_t(n > 0 ? n : 1), L'\0');
  if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, utf8, -1, path.data(), n)) return false;
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  out.assign(kImageSize, 0);
  DWORD got = 0;
  const BOOL ok = ReadFile(f, out.data(), kImageSize, &got, nullptr);
  CloseHandle(f);
  return ok && got == kImageSize;
}

void ImportOwn(uint32_t image) {
  const char* path = std::getenv("SC5_CREATION_SAVE");
  if (!path || !*path) return;
  std::vector<uint8_t> save;
  if (!ReadWhole(path, save) || Be32(save.data()) != 0x00010000 || Be32(save.data() + 4) != kImageSize) {
    REXLOG_WARN("sc5 creation: the player's save {} cannot be read as a save: no created characters", path);
    return;
  }
  int taken = 0, refused = 0;
  for (uint32_t n = 0; n < kSlots; n++) {
    const uint8_t* slot = save.data() + kFirstSlot + n * kSlotSize;
    if (!HeaderUsed(slot)) continue;
    const Record r(slot + kRecordAt, slot + kRecordAt + kRecordSize);
    if (n >= RemoteSlot(0)) {
      REXLOG_WARN("sc5 creation: slot {} ({}) stays out online: slots 49 and 50 hold the other player's", n + 1,
                  Name(r));
      continue;
    }
    // its form here; everything else at the select screen, once this PC's tables are read (DropRefused)
    if (const std::string why = CheckForm(r.data(), r.size()); !why.empty()) {
      REXLOG_WARN("sc5 creation: slot {} ({}) stays out: {}", n + 1, Name(r), why);
      refused++;
      continue;
    }
    std::memcpy(Host(image + kFirstSlot + n * kSlotSize), slot, kSlotSize);
    taken++;
  }
  REXLOG_INFO("sc5 creation: {} of the player's created characters in, {} refused", taken, refused);
}

}  // namespace

void OnHoldPoint() {
  static bool done = false;
  if (done) return;
  done = true;
  const uint32_t image = SaveImage();
  if (!image) {
    REXLOG_WARN("sc5 creation: no save image at the main menu: no created characters");
    return;
  }
  ImportOwn(image);
  Card();  // made now, not during a select screen
  // tests: SC5_TEST_CREATION_RECORD=<file>: a record (13,340 bytes) placed as the other PC's would be, for player 2
  if (const char* rec = std::getenv("SC5_TEST_CREATION_RECORD"); rec && *rec) {
    Record r(kRecordSize);
    FILE* f = std::fopen(rec, "rb");
    const size_t got = f ? std::fread(r.data(), 1, r.size(), f) : 0;
    if (f) std::fclose(f);
    REXLOG_INFO("sc5 creation: test record {} ({} bytes): {}", rec, got,
                got == kRecordSize && PlaceRemote(1, r) ? "placed" : "not placed");
  }
  // tests: whole slots into the first slots
  const char* list = std::getenv("SC5_TEST_CREATION_SLOT");
  if (!list || !*list) return;
  const std::string all = list;
  uint32_t n = 0;
  for (size_t at = 0; at <= all.size() && n < RemoteSlot(0);) {
    const size_t comma = all.find(',', at);
    const std::string path = all.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
    at = comma == std::string::npos ? all.size() + 1 : comma + 1;
    if (path.empty()) continue;
    std::vector<uint8_t> slot(kSlotSize);
    FILE* f = std::fopen(path.c_str(), "rb");
    const size_t got = f ? std::fread(slot.data(), 1, slot.size(), f) : 0;
    if (f) std::fclose(f);
    if (got != kSlotSize) {
      REXLOG_WARN("sc5 creation: {} is not a creation slot ({} bytes)", path, got);
      continue;
    }
    std::memcpy(Host(Slot(n)), slot.data(), kSlotSize);
    const Record r(slot.begin() + kRecordAt, slot.begin() + kRecordAt + kRecordSize);
    REXLOG_INFO("sc5 creation: slot {} from {} ({}, type {}, style {})", n + 1, path, Name(r), TypeOf(r.data()), r[16]);
    n++;
  }
}

}  // namespace sc5::creation
