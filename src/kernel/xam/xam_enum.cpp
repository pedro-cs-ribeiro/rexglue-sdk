/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/cvar.h>
#include <algorithm>
#include <cstring>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xtypes.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#else
#include <arpa/inet.h>
#endif

#if REX_PLATFORM_WIN32
#include <rex/platform.h>
#endif

#include <fmt/format.h>

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// https://github.com/LestaD/SourceEngine2007/blob/master/se2007/engine/xboxsystem.cpp#L518
uint32_t xeXamEnumerate(uint32_t handle, uint32_t flags, mapped_void buffer_ptr,
                        uint32_t buffer_size, uint32_t* items_returned, uint32_t overlapped_ptr) {
  assert_true(flags == 0);

  auto e = REX_KERNEL_OBJECTS()->LookupObject<XEnumerator>(handle);
  if (!e) {
    return X_ERROR_INVALID_HANDLE;
  }
  if (rex::cvar::Query<bool>("live_trace")) {
    REXKRNL_INFO("[live] XamEnumerate handle={:#x} buffer={:#x} size={} overlapped={:#x}", handle,
                 buffer_ptr.guest_address(), buffer_size, overlapped_ptr);
  }

  auto run = [e, buffer_ptr](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    X_RESULT result;
    uint32_t item_count = 0;
    if (!buffer_ptr) {
      result = X_ERROR_INVALID_PARAMETER;
    } else {
      result = e->WriteItems(buffer_ptr.guest_address(), buffer_ptr.as<uint8_t*>(), &item_count);
    }
    extended_error = X_HRESULT_FROM_WIN32(result);
    length = item_count;
    if (rex::cvar::Query<bool>("live_trace")) {
      REXKRNL_INFO("[live] XamEnumerate completion handle={:#x} items={} result={:#x}", e->handle(),
                   item_count, result);
    }
    return result;
  };

  if (items_returned) {
    assert_true(!overlapped_ptr);
    uint32_t extended_error;
    uint32_t item_count;
    X_RESULT result = run(extended_error, item_count);
    *items_returned = result == X_ERROR_SUCCESS ? item_count : 0;
    return result;
  } else if (overlapped_ptr) {
    assert_true(!items_returned);
    REX_KERNEL_STATE()->CompleteOverlappedDeferredEx(run, overlapped_ptr);
    return X_ERROR_IO_PENDING;
  } else {
    assert_always();
    return X_ERROR_INVALID_PARAMETER;
  }
}

u32 XamEnumerate_entry(u32 handle, u32 flags, mapped_void buffer, u32 buffer_length,
                       mapped_u32 items_returned, ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  uint32_t dummy;
  auto result =
      xeXamEnumerate(handle, flags, buffer, buffer_length,
                     !overlapped.guest_address() ? &dummy : nullptr, overlapped.guest_address());
  if (!overlapped && items_returned) {
    *items_returned = dummy;
  }
  return result;
}

// XLSP title-server enumerator (XTitleServerCreateEnumerator): the title asks
// for the servers of a site name it copies into the enumerator's private
// structure, and enumerates XTITLE_SERVER_INFO { IN_ADDR inaServer; DWORD
// dwFlags; CHAR szServerInfo[200]; } items. With online play enabled the one
// server is live_server; without it the list is empty.
class XTitleServerEnumerator : public XStaticUntypedEnumerator {
 public:
  static constexpr uint32_t kAppId = 0xFC;          // XLIVEBASE
  static constexpr uint32_t kOpenMessage = 0x58039;
  static constexpr size_t kItemSize = 208;

  XTitleServerEnumerator(KernelState* kernel_state, size_t items_per_enumerate)
      : XStaticUntypedEnumerator(kernel_state, items_per_enumerate, kItemSize) {}

  uint32_t WriteItems(uint32_t buffer_ptr, uint8_t* buffer_data,
                      uint32_t* written_count) override {
    if (!filled_) {
      filled_ = true;
      if (rex::cvar::Query<bool>("live_enabled")) {
        uint8_t* item = AppendItem();
        std::memset(item, 0, kItemSize);
        const std::string ip = rex::cvar::Query<std::string>("live_server");
        uint32_t address = inet_addr(ip.c_str());
        if (address == INADDR_NONE) {
          address = htonl(INADDR_LOOPBACK);
        }
        std::memcpy(item, &address, 4);           // inaServer, network order
        memory::store_and_swap<uint32_t>(item + 4, 0);  // dwFlags
        // The site name the title wrote after XamGetPrivateEnumStructureFromHandle.
        const char* site = reinterpret_cast<const char*>(
            memory()->TranslateVirtual(guest_object() + sizeof(X_KENUMERATOR)));
        std::string description = site && *site ? std::string(site, strnlen(site, 199))
                                                : std::string("live server");
        std::memcpy(item + 8, description.c_str(), description.size());
      }
    }
    const uint32_t result =
        XStaticUntypedEnumerator::WriteItems(buffer_ptr, buffer_data, written_count);
    if (rex::cvar::Query<bool>("live_trace")) {
      const char* site = reinterpret_cast<const char*>(
          memory()->TranslateVirtual(guest_object() + sizeof(X_KENUMERATOR)));
      REXKRNL_INFO("[live] title servers for site '{}': wrote {} item(s) -> {:#x}",
                   site ? std::string(site, strnlen(site, 199)) : std::string(),
                   written_count ? *written_count : 0, result);
    }
    return result;
  }

 private:
  bool filled_ = false;
};

// XamCreateEnumeratorHandle(user_index, app_id, open_message, close_message,
// extra_size, item_count, flags, out_handle): an enumerator whose items come
// from a system app. Only the enumerators titles are known to create are
// implemented; others are refused with a log line naming them.
u32 XamCreateEnumeratorHandle_entry(u32 user_index, u32 app_id, u32 open_message,
                                    u32 close_message, u32 extra_size, u32 item_count,
                                    u32 flags, mapped_u32 handle_out) {
  if (!handle_out) {
    return X_ERROR_INVALID_PARAMETER;
  }
  const uint32_t items = std::max<uint32_t>(1, std::min<uint32_t>(item_count, 1000));
  XEnumerator* e = nullptr;
  if (app_id == XTitleServerEnumerator::kAppId && open_message == XTitleServerEnumerator::kOpenMessage) {
    e = new XTitleServerEnumerator(REX_KERNEL_STATE(), items);
  } else {
    REXKRNL_WARN("XamCreateEnumeratorHandle: no enumerator for app {:#x} message {:#x}", app_id,
                 open_message);
    return X_ERROR_INVALID_PARAMETER;
  }
  void* extra = nullptr;
  const X_STATUS status =
      e->Initialize(user_index, app_id, open_message, close_message, flags, extra_size, &extra);
  if (XFAILED(status)) {
    e->Release();
    return X_ERROR_FUNCTION_FAILED;
  }
  if (extra && extra_size) {
    std::memset(extra, 0, extra_size);
  }
  *handle_out = e->handle();
  if (rex::cvar::Query<bool>("live_trace")) {
    REXKRNL_INFO("[live] XamCreateEnumeratorHandle app={:#x} msg={:#x} items={} -> handle {:#x}",
                 app_id, open_message, item_count, e->handle());
  }
  return X_ERROR_SUCCESS;
}

u32 XamGetPrivateEnumStructureFromHandle_entry(u32 handle, mapped_u32 out_object_ptr) {
  auto e = REX_KERNEL_OBJECTS()->LookupObject<XEnumerator>(handle);
  if (!e) {
    return X_STATUS_INVALID_HANDLE;
  }

  // Caller takes the reference.
  // It's released in ObDereferenceObject.
  e->RetainHandle();

  if (out_object_ptr.guest_address()) {
    *out_object_ptr = e->guest_object();
  }

  return X_STATUS_SUCCESS;
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__XamEnumerate, rex::kernel::xam::XamEnumerate_entry)
REX_EXPORT(__imp__XamCreateEnumeratorHandle, rex::kernel::xam::XamCreateEnumeratorHandle_entry)
REX_EXPORT(__imp__XamGetPrivateEnumStructureFromHandle,
           rex::kernel::xam::XamGetPrivateEnumStructureFromHandle_entry)
