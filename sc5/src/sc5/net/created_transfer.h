#pragma once
// A created character's record on its way from the other PC (netplay.cpp, CREATED packets): 12 chunks, 1,200 bytes
// each and the last 140, 13,340 bytes in all. Exactly those bytes from in-range chunk numbers are taken; anything
// else (another chunk count, a chunk number out of range, a chunk of another length, a chunk again with other bytes)
// refuses the record for the match. Plain C++: the tests build it alone (sc5\tests\creation_check_test.cpp).
#include <array>
#include <cstdint>
#include <cstring>
#include <format>
#include <string>
#include <vector>

namespace sc5::net {

constexpr int kCreatedChunk = 1200, kCreatedChunks = 12, kCreatedSize = 13340;

struct Incoming {
  std::array<std::vector<uint8_t>, kCreatedChunks> chunks;
  uint16_t mask = 0;     // the chunks taken
  std::string refused;   // why the record was refused ("" not)

  bool Whole() const { return refused.empty() && mask == (1u << kCreatedChunks) - 1; }

  // a chunk as it came: its number, the chunk count the sender gave, its bytes
  void Take(int index, int count, const uint8_t* data, int len) {
    if (!refused.empty()) return;
    const int want = index == kCreatedChunks - 1 ? kCreatedSize - (kCreatedChunks - 1) * kCreatedChunk : kCreatedChunk;
    if (count != kCreatedChunks) {
      refused = std::format("{} chunks, not {}", count, kCreatedChunks);
    } else if (index < 0 || index >= kCreatedChunks) {
      refused = std::format("chunk {}", index);
    } else if (len != want || !data) {
      refused = std::format("chunk {} of {} bytes, not {}", index, len, want);
    } else if (mask >> index & 1) {
      if (std::memcmp(chunks[size_t(index)].data(), data, size_t(len)) != 0)
        refused = std::format("chunk {} again with other bytes", index);
    } else {
      chunks[size_t(index)].assign(data, data + len);
      mask = uint16_t(mask | (1u << index));
    }
  }

  // the record, once Whole
  std::vector<uint8_t> Joined() const {
    std::vector<uint8_t> out;
    for (const auto& c : chunks) out.insert(out.end(), c.begin(), c.end());
    return out;
  }
};

}  // namespace sc5::net
