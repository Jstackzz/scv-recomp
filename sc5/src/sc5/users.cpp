// A second, local profile signed in as user 1 for the offline versus (SC5_VERSUS=1, or SC5_TEST_USER2=1 in tests):
// VS Battle offers "Player VS Player" only with a second user signed in (the SDK knows one profile, user 0). User 0
// is answered as the SDK does.
#include <cstdlib>
#include <cstring>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xtypes.h>

namespace {

constexpr uint32_t kSuccess = 0, kNoSuchUser = 0x80070525, kInvalidArg = 0x80070057;
constexpr uint64_t kUser2Xuid = 0xE000000000000002ull;  // an offline XUID
constexpr const char* kUser2Name = "Player 2";

bool User2() {
  static const bool on = [] {
    const char* v = std::getenv("SC5_TEST_USER2");
    const char* versus = std::getenv("SC5_VERSUS");
    const char* net = std::getenv("SC5_NET");
    return (v && v[0] == '1') || (versus && versus[0] == '1' && !(net && net[0] == '1'));
  }();
  return on;
}

struct SigninInfo {
  rex::be<uint64_t> xuid;
  rex::be<uint32_t> unk08;
  rex::be<uint32_t> signin_state;
  rex::be<uint32_t> unk10;
  rex::be<uint32_t> unk14;
  char name[16];
};
static_assert(sizeof(SigninInfo) == 40);

void Told(const char* what, uint32_t user) {
  static int n = 0;
  if (user == 1 && n++ < 40) REXLOG_INFO("sc5 users: {} for user 1", what);
}

uint32_t Sc5XamUserGetSigninState(rex::u32 user_index) {
  Told("XamUserGetSigninState", user_index);
  if (user_index == 0) return REX_KERNEL_STATE()->user_profile()->signin_state();
  if (user_index == 1 && User2()) return 1;
  return 0;
}

uint32_t Sc5XamUserGetSigninInfo(rex::u32 user_index, rex::u32 flags, ppc_ptr_t<SigninInfo> info) {
  Told("XamUserGetSigninInfo", user_index);
  if (!info) return kInvalidArg;
  std::memset(info, 0, sizeof(SigninInfo));
  if (user_index == 0) {
    const auto* p = REX_KERNEL_STATE()->user_profile();
    const uint32_t state = p->signin_state();
    info->xuid = ((flags & 1) && state != 2) ? 0 : p->xuid();
    info->signin_state = state;
    std::strncpy(info->name, p->name().c_str(), sizeof(info->name) - 1);
    return kSuccess;
  }
  if (user_index == 1 && User2()) {
    info->xuid = (flags & 1) ? 0 : kUser2Xuid;
    info->signin_state = 1;
    std::strncpy(info->name, kUser2Name, sizeof(info->name) - 1);
    return kSuccess;
  }
  return kNoSuchUser;
}

uint32_t Sc5XamUserGetXUID(rex::u32 user_index, rex::u32 type_mask, mapped_u64 xuid_ptr) {
  Told("XamUserGetXUID", user_index);
  if (!xuid_ptr) return kInvalidArg;
  if (user_index >= 4) {
    *xuid_ptr = 0;
    return kInvalidArg;
  }
  if (user_index == 0) {
    const auto* p = REX_KERNEL_STATE()->user_profile();
    const bool ok = (p->type() & type_mask) != 0;
    *xuid_ptr = ok ? p->xuid() : 0;
    return ok ? kSuccess : kNoSuchUser;
  }
  if (user_index == 1 && User2() && (type_mask & 1)) {
    *xuid_ptr = kUser2Xuid;
    return kSuccess;
  }
  *xuid_ptr = 0;
  return kNoSuchUser;
}

uint32_t Sc5XamUserGetName(rex::u32 user_index, mapped_string buffer, rex::u32 buffer_len) {
  Told("XamUserGetName", user_index);
  if (user_index >= 4) return kInvalidArg;
  const char* name = nullptr;
  std::string user0;
  if (user_index == 0) {
    user0 = REX_KERNEL_STATE()->user_profile()->name();
    name = user0.c_str();
  } else if (user_index == 1 && User2()) {
    name = kUser2Name;
  }
  if (!name) return kNoSuchUser;
  const uint32_t n = std::min<uint32_t>(buffer_len, 16);
  if (n) {
    std::strncpy(buffer, name, n - 1);
    buffer[n - 1] = 0;
  }
  return kSuccess;
}

}  // namespace

REX_HOOK(__imp__XamUserGetSigninState, Sc5XamUserGetSigninState)
REX_HOOK(__imp__XamUserGetSigninInfo, Sc5XamUserGetSigninInfo)
REX_HOOK(__imp__XamUserGetXUID, Sc5XamUserGetXUID)
REX_HOOK(__imp__XamUserGetName, Sc5XamUserGetName)
