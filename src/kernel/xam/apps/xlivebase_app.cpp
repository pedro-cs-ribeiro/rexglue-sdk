/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/cvar.h>
#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xenumerator.h>
#include <rex/thread.h>

#include "../fifa_friends.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fmt/format.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state) : App(kernel_state, 0xFC) {}

namespace {

// XONLINE_FRIEND: { XUID xuid; CHAR szGamertag[16]; DWORD dwFriendState;
// XNKID sessionID; DWORD dwTitleID; FILETIME ftUserTime; XNKID xnkidInvite;
// FILETIME gameinviteTime; DWORD cchRichPresence; WCHAR wszRichPresence[64]; }
constexpr uint32_t kFriendSize = 0xC4;
constexpr uint32_t kFriendStateOnline = 0x1;
constexpr uint32_t kFriendStatePlaying = 0x2;

// XFriendsCreateEnumerator arguments arrive as an XMsg argument block: 16-byte
// entries { DWORD type; DWORD pad; QWORD pointer-to-argument }.
uint32_t BlockArgPointer(memory::Memory* mem, uint32_t block, uint32_t index) {
  auto* entry = mem->TranslateVirtual<const uint8_t*>(block + index * 16);
  return static_cast<uint32_t>(memory::load_and_swap<uint64_t>(entry + 8));
}

}  // namespace

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x0005000C: {
      // XStringVerify / content filter (FilterText): the title checks player
      // strings against the offensive-word service before an online match.
      // We run no filter, so approve everything: the result buffer is an array
      // of per-string HRESULTs (all S_OK = allowed). arg block layout is small
      // and title-specific, so just succeed - the title proceeds with the text.
      REXKRNL_DEBUG("XLiveBaseStringVerify({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058004: {
      // Called on startup, seems to just return a bool in the buffer.
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      assert_true(!buffer_length || buffer_length == 4);
      REXKRNL_DEBUG("XLiveBaseGetNatType({:08X})", buffer_ptr);
      memory::store_and_swap<uint32_t>(buffer + 0, 1);  // XONLINE_NAT_OPEN
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // XOnlineGetServiceInfo(dwServiceId, XONLINE_SERVICE_INFO*): the XLSP
      // lookup a title does to find its publisher's servers. arg1 is the
      // service id by value, arg2 the output structure { DWORD dwServiceId;
      // IN_ADDR inaServer; WORD wPort; WORD wReserved; }. With online play
      // enabled every service is the configured server.
      const uint32_t service_id = buffer_ptr;
      const uint32_t out_guest = buffer_length;
      if (!rex::cvar::Query<bool>("live_enabled")) {
        REXKRNL_DEBUG("CXLiveLogon::GetServiceInfo({:08X}, {:08X}) offline", service_id,
                      out_guest);
        return 0x80151802;  // ERROR_CONNECTION_INVALID
      }
      unsigned a = 127, b = 0, c = 0, d = 1;
      const std::string ip = rex::cvar::Query<std::string>("live_server");
      std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d);
      const uint16_t port = static_cast<uint16_t>(rex::cvar::Query<uint32_t>("live_service_port"));
      if (out_guest >= 0x1000u && out_guest < 0xC0000000u) {
        auto out = memory_->TranslateVirtual(out_guest);
        const uint8_t ipbytes[4] = {static_cast<uint8_t>(a), static_cast<uint8_t>(b),
                                    static_cast<uint8_t>(c), static_cast<uint8_t>(d)};
        memory::store_and_swap<uint32_t>(out + 0, service_id);
        std::memcpy(out + 4, ipbytes, 4);
        memory::store_and_swap<uint16_t>(out + 8, port);
        memory::store_and_swap<uint16_t>(out + 10, 0);
      }
      if (rex::cvar::Query<bool>("live_trace")) {
        REXKRNL_INFO("[live] GetServiceInfo service={:08X} -> {}:{}", service_id, ip, port);
      }
      return X_E_SUCCESS;
    }
    case 0x00058020: {
      // XFriendsCreateEnumerator(dwUserIndex, dwStartingIndex, dwFriendsToReturn,
      // pcbBuffer, phEnum); arg2 is the argument block. The friends are the
      // players the user has met online, served by the online server.
      const uint32_t block = buffer_length;
      if (!block) {
        return X_E_INVALIDARG;
      }
      auto arg = [&](uint32_t i) { return BlockArgPointer(memory_, block, i); };
      const uint32_t user_index = memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(arg(0)));
      const uint32_t start = memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(arg(1)));
      const uint32_t count = memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(arg(2)));
      const uint32_t size_out = arg(3);
      const uint32_t handle_out = arg(4);
      std::vector<FriendEntry> friends;
      if (rex::cvar::Query<bool>("live_enabled") && user_index == 0) {
        auto* profile = kernel_state_->user_profile();
        friends = FsrCachedFriends(profile ? profile->online_xuid() : 0);
      }
      const uint32_t items = std::max<uint32_t>(1, std::min<uint32_t>(count, 100));
      auto* e = new XStaticUntypedEnumerator(kernel_state_, items, kFriendSize);
      if (XFAILED(e->Initialize(user_index, app_id(), 0x58020, 0x58021, 0))) {
        e->Release();
        return X_E_FAIL;
      }
      for (size_t i = start; i < friends.size() && i - start < count; ++i) {
        const FriendEntry& f = friends[i];
        uint8_t* item = e->AppendItem();
        std::memset(item, 0, kFriendSize);
        memory::store_and_swap<uint64_t>(item + 0x00, f.xuid);
        std::memcpy(item + 0x08, f.name.data(), std::min<size_t>(f.name.size(), 15));
        memory::store_and_swap<uint32_t>(
            item + 0x18, f.online ? kFriendStateOnline | kFriendStatePlaying : 0);
        memory::store_and_swap<uint32_t>(item + 0x24, f.online ? kernel_state_->title_id() : 0);
      }
      if (size_out) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(size_out), items * kFriendSize);
      }
      if (handle_out) {
        memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(handle_out), e->handle());
      }
      REXKRNL_INFO("XFriendsCreateEnumerator: {} friend(s) -> handle {:#x}", friends.size(),
                   e->handle());
      return X_E_SUCCESS;
    }
    case 0x00058023: {
      // XInviteGetAcceptedInfo(dwUserIndex, XINVITE_INFO*): argument block like
      // 0x58020's. XINVITE_INFO = { XUID xuidInvitee; XUID xuidInviter; DWORD
      // dwTitleID; XSESSION_INFO hostInfo (XNKID, XNADDR, XNKEY); BOOL
      // fFromGameInvite; } (84 bytes). The title joins the inviter's game by
      // looking the inviter up on the online server, so the host session
      // fields stay zero.
      const uint32_t block = buffer_length;
      AcceptedInvite invite;
      if (!block || !GetAcceptedInvite(&invite)) {
        return X_E_FAIL;
      }
      const uint32_t info_ptr = BlockArgPointer(memory_, block, 1);
      if (!info_ptr) {
        return X_E_INVALIDARG;
      }
      auto* info = memory_->TranslateVirtual<uint8_t*>(info_ptr);
      std::memset(info, 0, 84);
      auto* profile = kernel_state_->user_profile();
      memory::store_and_swap<uint64_t>(info + 0, profile ? profile->online_xuid() : 0);
      memory::store_and_swap<uint64_t>(info + 8, invite.inviter_xuid);
      memory::store_and_swap<uint32_t>(info + 16, kernel_state_->title_id());
      memory::store_and_swap<uint64_t>(info + 20, invite.game_id);  // session id
      memory::store_and_swap<uint32_t>(info + 80, 1);                // fFromGameInvite
      REXKRNL_INFO("XInviteGetAcceptedInfo: inviter {:016X} (game {})", invite.inviter_xuid,
                   invite.game_id);
      return X_E_SUCCESS;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      REXKRNL_DEBUG("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058037: {
      REXKRNL_DEBUG("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x0005800E: {
      // Called ~2.5 min into an online match's Side Select once BOTH players are
      // ready, as part of the match-start/session-finalize; the default X_E_FAIL
      // aborts the start and drops the ready (match never kicks off). Both args
      // are guest pointers (in/out structs); succeed and leave the output as-is.
      REXKRNL_DEBUG("XLiveBaseUnk5800E({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
  }
  // Dump the argument block: it is usually a small struct whose words are
  // guest pointers and sizes, which is what identifies the call.
  std::string words;
  if (buffer_ptr && buffer_length && buffer_length <= 256) {
    auto* p = memory_->TranslateVirtual<const uint8_t*>(buffer_ptr);
    for (uint32_t i = 0; i + 4 <= buffer_length; i += 4) {
      words += fmt::format(" {:02X}{:02X}{:02X}{:02X}", p[i], p[i + 1], p[i + 2], p[i + 3]);
    }
  }
  REXKRNL_ERROR(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}{}",
      app_id(), message, buffer_ptr, buffer_length, words.empty() ? "" : " words:" + words);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
