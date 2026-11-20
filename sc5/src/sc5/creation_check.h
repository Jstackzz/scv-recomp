#pragma once
// The check a created character from another PC passes before anything loads it (creation.cpp): the record
// (TK_CreationCharacterDNA_st, 13,340 bytes, big-endian as the save keeps it) field by field, against the limits the
// game itself keeps to and, for every id, against the game's own tables on this PC (Known). Plain C++: the tests build
// it alone (sc5\tests\creation_check_test.cpp).
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace sc5::creation {

// The record's fields (the game's schema, sub_822256B0, and its parts: tools\py and docs\GAME.md, "Created
// characters").
namespace dna {
constexpr uint32_t kSize = 13340;
constexpr uint32_t kMajor = 0, kMinor = 2;  // 11, 0 (u16)
constexpr uint32_t kSelectNo = 4, kSelectVariation = 5, kModelIndex = 6, kBaseType = 7, kFacialId = 8;
constexpr uint32_t kType = 9;  // the character type: 32 an original character, else the fighter it customizes
constexpr uint32_t kVoice = 10, kPitch = 11, kFormant = 12, kFlanger = 13, kEyebrow = 14, kDlcFlag = 15;
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

// The game's checksum of a record (sub_82245F98): over its first 13,336 bytes
uint32_t Checksum(const uint8_t* record);

// A record's character type and base type: an original character (listed under the Creation slot) has base type 0
// (male) or 1 (female); a customized fighter (a fighter re-dressed in Creation, one of its costumes) has the fighter's
// own type and base type 5, whose table (the fighters' costumes) the game reads for Creation only (creation.cpp)
constexpr int kOriginal = 32, kFighterBase = 5;

// What this PC's game has, read from its own tables (creation.cpp). An empty set refuses every id of its kind.
struct Known {
  std::set<int> characters;                    // character types the select screen lists here (32 too)
  std::set<int> styles;                        // the styles Creation offers
  // (character type, base type) -> the parts sets' ids (enCreationPartsSetID) a record may wear and every model
  // (ModelData) they use: an original character's (32, 0) and (32, 1), its base type's whole table; a customized
  // fighter's (its type, 5), the fighter's own costumes and the pieces any fighter may wear (the fighters' hair)
  std::map<std::pair<int, int>, std::set<uint32_t>> parts_sets;
  std::map<std::pair<int, int>, std::set<uint32_t>> models;
  // the illustrations (uniqueID) and the texture each is: OLTIndex << 24 | OLTRelationIndex << 16 | OLTKeyIndex, as
  // a record's sticker repeats them (+16, +17, +14)
  std::map<uint32_t, uint32_t> stickers;
  std::set<std::pair<int, int>> weapons;       // (style, variation)
  // the colour palette's last row and column (sub_8222C450 takes a place up to them)
  int palette_rows = 0, palette_cols = 0;
};

// Known as text and back: a line per kind ("characters", "styles", "palette", "weapons" style.variation, "stickers"
// id:texture, "sets"/"models" with their character and base type), the ids in hex. Tests: the game writes its own with
// SC5_TEST_KNOWN_OUT=<file>, and creation_check_test checks records against that (tests\data\known.txt).
std::string KnownText(const Known& known);
bool ParseKnown(const std::string& text, Known& out);

// The record's form alone (its size, version, checksum and names), what a PC can check before it has read its
// tables: "" when it passes
std::string CheckForm(const uint8_t* record, size_t size);
// Everything: "" when the record passes, else why not (for the error line and the log)
std::string CheckRecord(const uint8_t* record, size_t size, const Known& known);

}  // namespace sc5::creation
