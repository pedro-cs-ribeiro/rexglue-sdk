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
#include <rex/thread.h>

#include <cstdio>
#include <cstring>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;
namespace apps {
using namespace rex::system;

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state) : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
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
      // 0x00058004 is called right before this.
      // We should create a XamEnumerate-able empty list here, but I'm not
      // sure of the format.
      // buffer_length seems to be the same ptr sent to 0x00058004.
      REXKRNL_DEBUG("CXLiveFriends::Enumerate({:08X}, {:08X}) unimplemented", buffer_ptr,
                    buffer_length);
      return X_E_FAIL;
    }
    case 0x00058023: {
      REXKRNL_DEBUG(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}) "
          "unimplemented",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
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
  }
  REXKRNL_ERROR(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace rex
