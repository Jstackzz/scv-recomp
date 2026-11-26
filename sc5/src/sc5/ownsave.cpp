// The player's own save in an online session, see ownsave.h.
//
// The save object ([0x8274EF10], 0x82A2D970 in every run so far) keeps the save image from +0x48: savedata.bin as it
// is on disk, 0x22C200 bytes, which the game reads and writes in place as it goes (not a copy made for a save): the
// play time in seconds at +0x180 (sub_8246D668, every second), VS Battle's time at +0x51CC (sub_82164650, every second
// in VS Battle), a match's counts at its end (+0x4CC from the battle's state machine, sub_82332108; +0x554 at its
// result, sub_82166EC0), the flag at +0x22C1F8 that a save is due, the options (+0x18 brightness, +0x19 subtitles,
// +0x1A Battle Messages, +0x1B Custom Effects, +0x1C on the sound; +0x90 the Control Settings of four pads, 60 bytes
// each, whose live copy is 0x82925630), the 50 creation slots from +0x516C. The release table follows the image
// (+0x22C248, 0x1D02 ids of 4 bits). Found with hardware write watches on the bytes one online match had changed in
// the player's file (2026-11-26: +0x183, +0x4CF, +0x557, +0x51C9-0x51D7, +0x22C1F8) and memory dumps before and after
// the Options were changed.
//
// The save task writes the files in two routines (sub_8249FD78 and sub_824A0990, the second through message boxes when
// something fails), each opening savedata.bin and replay.bin with CREATE_ALWAYS and writing them whole through
// sub_824D83A8 (r3 the file, r4 the buffer, r5 its size; called from 0x8249FFE4 and 0x824A0E98): the game saved on its
// way back to the main menu after an online match (2026-11-26, the play time, VS Battle's counts, the options as the
// session had them). Online, savedata.bin's write takes the image as the session found it in place of the image now:
// the game takes the save as done, and the file gets what it would have got before the session.

#include "sc5/ownsave.h"

#include <atomic>
#include <cstring>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "sc5/creation.h"
#include "sc5/frame.h"
#include "sc5/unlocks.h"

REX_EXTERN(__imp__sub_824D83A8);  // a file's write (r3 the file, r4 the buffer, r5 its size, r6 the bytes written)

namespace sc5::ownsave {
namespace {

constexpr uint32_t kImageSize = 0x22C200;
constexpr uint32_t kReleaseSize = (0x1D02 + 1) / 2;  // the release table right after the image
// The options as the game uses them: a table of 92-byte objects at 0x827551E8, each one's value at +8, which
// sub_8246C258 copies into the save image before a save (+0x18 on: brightness, subtitles, Battle Messages, Custom
// Effects, then the sound's) with the Control Settings' live copy (0x82925630, 240 bytes, to +0x90). The two display
// options that could reach the fight are held at their defaults online (0: Battle Messages on, Custom Effects "Show
// All"); brightness and subtitles only change what is drawn, the sound's how loud.
constexpr uint32_t kOptions = 0x827551E8, kOptionSize = 92, kOptionValue = 8;
constexpr uint32_t kPinned[] = {2, 3};  // Battle Messages, Custom Effects
constexpr const char* kPinnedNames[] = {"Battle Messages", "Custom Effects"};

std::atomic<bool> g_online{false};
uint32_t g_image = 0;          // where the kept image goes back to
std::vector<uint8_t> g_kept;   // the image and the release table as the session found them
uint32_t g_options[2] = {0, 0};  // the pinned options' own values
std::atomic<int> g_held_back{0};  // the game's saves of this session that wrote the kept image
uint32_t g_guest_copy = 0;        // a guest buffer for the kept image (the write's buffer must be guest memory)

uint8_t* Host(uint32_t guest) { return REX_KERNEL_STATE()->memory()->TranslateVirtual<uint8_t*>(guest); }
uint32_t Get32(uint32_t a) { return __builtin_bswap32(*REX_KERNEL_STATE()->memory()->TranslateVirtual<uint32_t*>(a)); }
void Put32(uint32_t a, uint32_t v) { *REX_KERNEL_STATE()->memory()->TranslateVirtual<uint32_t*>(a) = __builtin_bswap32(v); }
uint32_t OptionAt(uint32_t i) { return kOptions + i * kOptionSize + kOptionValue; }

}  // namespace

void BeginOnline() {
  if (g_online) return;
  g_held_back = 0;
  g_image = creation::SaveImage();
  if (g_image) {
    const uint8_t* p = Host(g_image);
    g_kept.assign(p, p + kImageSize + kReleaseSize);
    REXLOG_INFO("sc5 own save: an online session at frame {}: the save kept aside (play time {} s)", frame::Count(),
                Get32(g_image + 0x180));
  } else {
    g_kept.clear();
    REXLOG_WARN("sc5 own save: an online session without a save image in memory: nothing kept (its writes held back)");
  }
  for (size_t i = 0; i < 2; i++) {
    g_options[i] = Get32(OptionAt(kPinned[i]));
    if (g_options[i]) REXLOG_INFO("sc5 own save: {} at its default for the session (the player's {})", kPinnedNames[i],
                                  g_options[i]);
    Put32(OptionAt(kPinned[i]), 0);
  }
  g_online = true;
  unlocks::SetOnline(true);
  creation::BeginOnline();
}

void EndOnline() {
  if (!g_online) return;
  creation::EndOnline();
  for (size_t i = 0; i < 2; i++) Put32(OptionAt(kPinned[i]), g_options[i]);
  if (g_image && g_kept.size() == size_t(kImageSize + kReleaseSize) && creation::SaveImage() == g_image) {
    uint8_t* image = Host(g_image);
    size_t changed = 0;
    for (size_t i = 0; i < g_kept.size(); i++) changed += image[i] != g_kept[i];
    std::memcpy(image, g_kept.data(), g_kept.size());
    REXLOG_INFO("sc5 own save: the online session is over at frame {}: the save put back as it was ({} bytes the "
                "session had changed; {} of the game's saves wrote the save as it was)",
                frame::Count(), changed, g_held_back.load());
  } else if (g_image) {
    REXLOG_WARN("sc5 own save: the online session is over but the save image moved: not put back");
  }
  g_kept.clear();
  g_kept.shrink_to_fit();
  unlocks::SetOnline(false);
  g_online = false;
}

bool Online() { return g_online; }

// The buffer savedata.bin is written from: the image the session found in place of the image now (ownsave.cpp)
uint32_t WriteBuffer(uint32_t lr, uint32_t buffer, uint32_t size) {
  if (!g_online || (lr != 0x8249FFE8 && lr != 0x824A0E9C) || !g_image || buffer != g_image || size != kImageSize ||
      g_kept.size() < kImageSize)
    return buffer;
  if (!g_guest_copy) g_guest_copy = REX_KERNEL_STATE()->memory()->SystemHeapAlloc(kImageSize, 0x1000);
  if (!g_guest_copy) {
    REXLOG_ERROR("sc5 own save: no guest memory for the kept save: this save writes the session's");
    return buffer;
  }
  std::memcpy(Host(g_guest_copy), g_kept.data(), kImageSize);
  REXLOG_INFO("sc5 own save: the game saves in an online session ({}): the save as it was before the session written",
              ++g_held_back);
  return g_guest_copy;
}

}  // namespace sc5::ownsave

// A file's write: the game's save of savedata.bin in an online session writes the kept image (ownsave.h)
REX_HOOK_RAW(sub_824D83A8) {
  const uint32_t buffer = ctx.r4.u32;
  const uint32_t use = sc5::ownsave::WriteBuffer(ctx.lr ? uint32_t(ctx.lr) : 0, buffer, ctx.r5.u32);
  if (use != buffer) ctx.r4.u64 = use;
  __imp__sub_824D83A8(ctx, base);
}
