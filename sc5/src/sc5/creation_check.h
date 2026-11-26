#pragma once
// The check a created character passes before a match loads it (creation.cpp): the record (TK_CreationCharacterDNA_st,
// 13,340 bytes, big-endian as the save keeps it) field by field, for what would hang, crash or part the two games, and
// every id against what this PC's game can load (Known); and the choice, the same in both games, of how a player's
// character fights when the two PCs' downloadable parts differ (Decide). Plain C++: the tests build it alone
// (sc5\tests\creation_check_test.cpp).
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sc5::creation {

// The record's fields (the game's schema, sub_82226450, and its parts: tools\py and docs\GAME.md, "Created
// characters").
namespace dna {
constexpr uint32_t kSize = 13340;
constexpr uint32_t kMajor = 0, kMinor = 2;  // 11, 0 (u16)
constexpr uint32_t kSelectNo = 4, kSelectVariation = 5, kModelIndex = 6, kBaseType = 7, kFacialId = 8;
constexpr uint32_t kType = 9;  // the character type: 32 an original character, else the fighter it customizes
constexpr uint32_t kVoice = 10, kPitch = 11, kFormant = 12, kFlanger = 13, kEyebrow = 14, kDlcFlag = 15;
// The attack rate (17) is not checked: the creation manager makes it the fight's damage factor as it is (rate x 0.01,
// sub_82221978, at its entry's +13536), in both games from the same byte. Title update 3's repair pass would clamp it
// to 80-111 (title update 1's to 80-200), but the game runs that pass only on its templates and on records from other
// places (sub_822223D8 and others), never on a save's own characters (sub_8231BD10 copies them as they are), so a
// character made under title update 1 fights offline at its own rate too.
constexpr uint32_t kStyle = 16, kAttackRate = 17;
constexpr uint32_t kPartsSets = 20, kPartsSetCount = 20;  // enCreationPartsSetID[20] (u32, -1 none)
constexpr uint32_t kBody = 100;                            // TK_CreationBodyFeature_st: 16 floats, 4 bytes
constexpr uint32_t kColours = 168, kColourCount = 120;     // TK_CreationColorData_st[120], 8 bytes each
constexpr uint32_t kBreakLevels = 1128, kBreakLevelCount = 4, kBreakLevelSize = 288;
constexpr uint32_t kPartsPerLevel = 34;                    // CCreationPartsInfo_st (ModelData u32, slot u8, 3 bytes)
constexpr uint32_t kCategoryDiff = 2280;                   // 8 bytes
constexpr uint32_t kPartsSe = 2288, kPartsSeCount = 20;    // CCreationPartsSEInfo (material, exist, priority u16)
constexpr uint32_t kCoords = 2368, kCoordCount = 19, kCoordSize = 96;  // CCreationPartsCoordinateInfo
constexpr uint32_t kWeapon = 4192;                         // CWeaponDNA, 700 bytes
constexpr uint32_t kTime = 4892;                           // TK_TimeClass: 7 u32
constexpr uint32_t kBreakEffects = 4920, kBreakEffectSets = 3, kBreakEffectsPerSet = 16;  // 12 bytes each
constexpr uint32_t kStickers = 5496, kStickerCount = 84, kStickerSize = 92;  // CCreationBakingInfo
constexpr uint32_t kHeight = 13224;
constexpr uint32_t kFacialName = 13225, kFacialNameSize = 5;
constexpr uint32_t kName = 13230, kNameSize = 15;  // empty in the game's own created characters
constexpr uint32_t kNameRelease = 13245, kNameRaw = 13290, kNameLongSize = 45;  // UTF-8, the shown name first
constexpr uint32_t kChecksum = 13336;
// the weapon's parts (from kWeapon)
constexpr uint32_t kWeaponStyle = 0, kWeaponVariation = 2, kWeaponEffect = 4, kWeaponColours = 8, kWeaponStickers = 40,
                   kWeaponStickerCount = 4, kWeaponTrace = 408, kWeaponEffectInfo = 624;
}  // namespace dna

// The game's checksum of a record (sub_822470B0): over its first 13,336 bytes
uint32_t Checksum(const uint8_t* record);

// A record's character type and base type: an original character (listed under the Creation slot) has base type 0
// (male) or 1 (female); a customized fighter (a fighter re-dressed in Creation, one of its costumes) has the fighter's
// own type and base type 5, whose table (the fighters' costumes) the game reads for Creation only (creation.cpp)
constexpr int kOriginal = 32, kFighterBase = 5;

// The select screen lists a save's character of style 13 only when release 49 (downloadable content) is released here
// (sub_8231BD10), though Creation offers the style on every PC
constexpr int kStyle13 = 13;
constexpr uint32_t kStyle13Release = 49;

// A model (ModelData) is the parts set type, the table index, the variation and the segment: the game loads the file
// the first three name (sub_822A4530: the model table's row and column; 0x7F a downloadable one, its package group and
// entry) and in each break level shows the segment the last names (sub_82221828, from sub_821485E0 in a fight and
// sub_821D1728 in Creation), or none when it is negative. Creation writes segment 0xFF (as its own "none", FF0000FF)
// for a piece it keeps worn but does not show: the parts tables never list that form (seen 2026-11-20, a player's new
// female characters: model 1B0800FF, the innerwear inner_006_f, parts set 01050006, whose four segments the table
// lists as 1B080000-1B080003).
//
// A downloadable model is 0x7F, its package group, its entry, the segment: the game loads it from that package's data
// (docs\GAME.md, "A 360 DLC package is a key, and data"), which one PC may have and the other not.
//
// Title update 3's fallback: a fight shows break levels 0 and 1 (whole, then broken), and when a character's models
// cannot be loaded (a downloadable part whose data this PC lacks) the game shows levels 2 and 3 in their place, which
// Creation fills with the plain body (docs\GAME.md, "Downloadable parts in a fight"). Online both games must show the
// same two levels: the Form of a match's created character, chosen alike in both (Decide), and the fight then loads
// only those levels (creation.cpp makes the game's own fallback happen in both games for kPlain).
constexpr uint32_t kModelFile = 0xFFFFFF00, kNoSegment = 0xFF;
constexpr bool IsDlcModel(uint32_t model) { return model >> 24 == 0x7F; }
// the most models (files, whatever their segments) break level 0 (or 2) may wear: the list of them has room for 20
constexpr uint32_t kMaxModelFiles = 20;

// which two break levels a match shows: 0 and 1 (the character as made) or 2 and 3 (its plain body)
enum class Form { kWhole, kPlain };
constexpr uint32_t FirstLevel(Form f) { return f == Form::kWhole ? 0 : 2; }

// What this PC's game has, read from its own tables (creation.cpp). An empty set refuses every id of its kind.
// Everything but the downloadable files and the characters is the same on every PC (the disc's, each pack's parts on
// the disc counted whether its package is there or not); those two are what Decide weighs.
struct Known {
  std::set<int> characters;                    // character types the select screen lists here (32 too)
  std::set<int> styles;                        // the styles Creation offers
  // (character type, base type) -> the parts sets' ids (enCreationPartsSetID) Creation offers and every model
  // (ModelData) they use: an original character's (32, 0) and (32, 1), its base type's whole table; a customized
  // fighter's (its type, 5), the fighter's own costumes and the pieces any fighter may wear (the fighters' hair).
  // The parts sets are for the record only: the game itself ignores a record's list of them (tools\creationfuzz.py,
  // "partsset"), and the check does not hold a record to it.
  std::map<std::pair<int, int>, std::set<uint32_t>> parts_sets;
  std::map<std::pair<int, int>, std::set<uint32_t>> models;
  // the downloadable model files (model & kModelFile) this PC's game loads: listed in its tables and their data
  // installed (the DLC manager's entry has its file)
  std::set<uint32_t> dlc_files;
  // styles Creation offers that this PC's select screen does not list (kStyle13 without its release)
  std::set<int> unlisted_styles;
  // the illustrations (uniqueID) and the texture each is: OLTIndex << 24 | OLTRelationIndex << 16 | OLTKeyIndex, as
  // a record's sticker repeats them (+16, +17, +14)
  std::map<uint32_t, uint32_t> stickers;
  std::set<std::pair<int, int>> weapons;       // (style, variation)
  // the colour palette's last row and column (sub_8222D210 takes a place up to them)
  int palette_rows = 0, palette_cols = 0;
};

// Known as text and back: a line per kind ("characters", "styles", "palette", "weapons" style.variation, "stickers"
// id:texture, "sets"/"models" with their character and base type, "dlc" the downloadable files), the ids in hex.
// Tests: the game writes its own with SC5_TEST_KNOWN_OUT=<file>, and creation_check_test checks records against that
// (tests\data\known.txt).
std::string KnownText(const Known& known);
bool ParseKnown(const std::string& text, Known& out);

// The record's form alone (its size, version, checksum and names), what a PC can check before it has read its
// tables: "" when it passes
std::string CheckForm(const uint8_t* record, size_t size);
// Everything, for a match that shows `form`'s two break levels: "" when this PC's game can load the record and fight
// with it, else why not (for the error line and the log). The models of the two levels not shown are held to the
// tables too, but a downloadable one there needs no file (the fight never loads those levels).
std::string CheckRecord(const uint8_t* record, size_t size, const Known& known, Form form = Form::kWhole);
// a model a record may wear among `models` (a Known's for its character and base type): one of them, or one of their
// files with segment 0xFF (shown nowhere; the file is one this PC has)
bool KnownModel(const std::set<uint32_t>& models, uint32_t model);
// the downloadable files (model & kModelFile) `form`'s break levels wear that `files` does not have
std::vector<uint32_t> MissingFiles(const uint8_t* record, Form form, const std::set<uint32_t>& files);

// What the other PC's game has that may differ from this one's (Known): its characters (from its select screen) and
// its downloadable files, as the netplay brings them before the pick
struct Other {
  std::set<int> characters;
  std::set<uint32_t> dlc_files;
  std::set<int> unlisted_styles;
};
// How this PC's player's own character fights in a match against `other`: as made when both games load break levels 0
// and 1, else in its plain body when both load 2 and 3 (both games show that alike: title update 3's own fallback),
// else not at all. Both games come to the same answer about the same record: everything else the check holds is the
// same on every PC, and the other game checks what it is sent with the same rules (CheckRecord, `form`).
struct Decision {
  bool ok = false;
  Form form = Form::kWhole;
  // not ok: why not (the check's words, PlainWhy says them plainly); kPlain: which files the other PC lacks
  std::string why;
};
Decision Decide(const uint8_t* record, size_t size, const Known& mine, const Other& other);
// why a record was refused, or fights in its plain body (CheckRecord's, CheckForm's, Decide's), in plain words for
// the player
std::string PlainWhy(const std::string& why);

}  // namespace sc5::creation
