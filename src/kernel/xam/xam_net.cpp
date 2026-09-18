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

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#if REX_PLATFORM_MAC
#include <sys/select.h>
#endif

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xam/private.h>
#include <rex/kernel/xboxkrnl/error.h>
#include <rex/kernel/xboxkrnl/threading.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>
#include <rex/system/xsocket.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#if REX_PLATFORM_WIN32
// NOTE: must be included last as it expects windows.h to already be included.
#define _WINSOCK_DEPRECATED_NO_WARNINGS  // inet_addr
#include <winsock2.h>                    // NOLINT(build/include_order)
#elif REX_PLATFORM_LINUX || REX_PLATFORM_MAC
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <sys/socket.h>
#endif

namespace rex {
namespace kernel {
namespace xam {
using namespace rex::system;
using namespace rex::system::xam;

// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

// https://github.com/pmrowla/hl2sdk-csgo/blob/master/common/xbox/xboxstubs.h
typedef struct {
  // FYI: IN_ADDR should be in network-byte order.
  in_addr ina;                    // IP address (zero if not static/DHCP)
  in_addr inaOnline;              // Online IP address (zero if not online)
  rex::be<uint16_t> wPortOnline;  // Online port
  uint8_t abEnet[6];              // Ethernet MAC address
  uint8_t abOnline[20];           // Online identification
} XNADDR;

typedef struct {
  rex::be<int32_t> status;
  rex::be<uint32_t> cina;
  in_addr aina[8];
} XNDNS;

typedef struct {
  uint8_t flags;
  uint8_t reserved;
  rex::be<uint16_t> probes_xmit;
  rex::be<uint16_t> probes_recv;
  rex::be<uint16_t> data_len;
  rex::be<uint32_t> data_ptr;
  rex::be<uint16_t> rtt_min_in_msecs;
  rex::be<uint16_t> rtt_med_in_msecs;
  rex::be<uint32_t> up_bits_per_sec;
  rex::be<uint32_t> down_bits_per_sec;
} XNQOSINFO;

typedef struct {
  rex::be<uint32_t> count;
  rex::be<uint32_t> count_pending;
  XNQOSINFO info[1];
} XNQOS;

struct Xsockaddr_t {
  rex::be<uint16_t> sa_family;
  char sa_data[14];
};

struct X_WSADATA {
  rex::be<uint16_t> version;
  rex::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  rex::be<uint16_t> max_sockets;
  rex::be<uint16_t> max_udpdg;
  rex::be<uint32_t> vendor_info_ptr;
};

struct XWSABUF {
  rex::be<uint32_t> len;
  rex::be<uint32_t> buf_ptr;
};

struct XWSAOVERLAPPED {
  rex::be<uint32_t> internal;
  rex::be<uint32_t> internal_high;
  union {
    struct {
      rex::be<uint32_t> low;
      rex::be<uint32_t> high;
    } offset;  // must be named to avoid GCC error
    rex::be<uint32_t> pointer;
  };
  rex::be<uint32_t> event_handle;
};

void LoadSockaddr(const uint8_t* ptr, sockaddr* out_addr) {
  out_addr->sa_family = memory::load_and_swap<uint16_t>(ptr + 0);
  switch (out_addr->sa_family) {
    case AF_INET: {
      auto in_addr = reinterpret_cast<sockaddr_in*>(out_addr);
      in_addr->sin_port = memory::load_and_swap<uint16_t>(ptr + 2);
      // Maybe? Depends on type.
      in_addr->sin_addr.s_addr = *(uint32_t*)(ptr + 4);
      break;
    }
    default:
      assert_unhandled_case(out_addr->sa_family);
      break;
  }
}

void StoreSockaddr(const sockaddr& addr, uint8_t* ptr) {
  switch (addr.sa_family) {
    case AF_UNSPEC:
      std::memset(ptr, 0, sizeof(addr));
      break;
    case AF_INET: {
      auto& in_addr = reinterpret_cast<const sockaddr_in&>(addr);
      memory::store_and_swap<uint16_t>(ptr + 0, in_addr.sin_family);
      memory::store_and_swap<uint16_t>(ptr + 2, in_addr.sin_port);
      // Maybe? Depends on type.
      memory::store_and_swap<uint32_t>(ptr + 4, in_addr.sin_addr.s_addr);
      break;
    }
    default:
      assert_unhandled_case(addr.sa_family);
      break;
  }
}

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags;
  uint8_t cfgSockMaxDgramSockets;
  uint8_t cfgSockMaxStreamSockets;
  uint8_t cfgSockDefaultRecvBufsizeInK;
  uint8_t cfgSockDefaultSendBufsizeInK;
  uint8_t cfgKeyRegMax;
  uint8_t cfgSecRegMax;
  uint8_t cfgQosDataLimitDiv4;
  uint8_t cfgQosProbeTimeoutInSeconds;
  uint8_t cfgQosProbeRetries;
  uint8_t cfgQosSrvMaxSimultaneousResponses;
  uint8_t cfgQosPairWaitTimeInSeconds;
};

// Online play against a private server that speaks the title's own protocol.
// When on, the console reports itself online and signed in, the Xbox secure
// address lookups (XLSP service info, XNetServerToInAddr, DNS) all resolve to
// live_server, and the socket layer follows the semantics EA's network library
// expects. Off (the default) keeps the title quietly offline.
REXCVAR_DEFINE_BOOL(live_enabled, false, "Live", "Enable online play against a private server")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(live_server, "127.0.0.1", "Live",
                      "IPv4 address every online service lookup resolves to")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_UINT32(live_service_port, 42124, "Live",
                      "Port returned for XLSP service lookups (the title server's front door)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(live_trace, false, "Live", "Log every online-related kernel call")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Diagnostics: report the console as online (title address, link status).
REXCVAR_DEFINE_UINT32(live_relay_port, 10043, "Live",
                      "UDP port of the server's peer relay for match traffic");
REXCVAR_DEFINE_BOOL(live_report_online, true, "Live",
                    "Report an online title address and an active link while online play is enabled")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(net_loopback_only, false, "Kernel",
                    "Bind sockets the title opens on any address to 127.0.0.1 instead, so an "
                    "offline title never listens on a real interface (no firewall prompt).");

XNetStartupParams xnet_startup_params = {};

u32 NetDll_XNetStartup_entry(u32 caller, ppc_ptr_t<XNetStartupParams> params) {
  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return 0;
}

u32 NetDll_XNetCleanup_entry(u32 caller, mapped_void params) {
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  return 0;
}

u32 NetDll_XNetGetOpt_entry(u32 one, u32 option_id, mapped_void buffer_ptr,
                            mapped_u32 buffer_size) {
  assert_true(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return 0x2738;  // WSAEMSGSIZE
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      return 0;
    default:
      REXKRNL_ERROR("NetDll_XNetGetOpt: option {} unimplemented", option_id);
      return 0x2726;  // WSAEINVAL
  }
}

u32 NetDll_XNetRandom_entry(u32 caller, mapped_void buffer_ptr, u32 length) {
  // For now, constant values.
  // This makes replicating things easier.
  std::memset(buffer_ptr, 0xBB, length);

  return 0;
}

u32 NetDll_WSAStartup_entry(u32 caller, u16 version, ppc_ptr_t<X_WSADATA> data_ptr) {
// TODO(benvanik): abstraction layer needed.
#if REX_PLATFORM_WIN32
  WSADATA wsaData;
  ZeroMemory(&wsaData, sizeof(WSADATA));
  int ret = WSAStartup(version, &wsaData);

  auto data_out = REX_KERNEL_MEMORY()->TranslateVirtual(data_ptr.guest_address());

  if (data_ptr) {
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
    std::memcpy(&data_ptr->description, wsaData.szDescription, 0x100);
    std::memcpy(&data_ptr->system_status, wsaData.szSystemStatus, 0x80);
    data_ptr->max_sockets = wsaData.iMaxSockets;
    data_ptr->max_udpdg = wsaData.iMaxUdpDg;

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    uint32_t vendor_ptr = memory::load_and_swap<uint32_t>(data_out + 0x190);
    memory::store_and_swap<uint32_t>(data_out + 0x190, vendor_ptr);
  }
#else
  int ret = 0;
  if (data_ptr) {
    // Guess these values!
    data_ptr->version = version;
    data_ptr->description[0] = '\0';
    data_ptr->system_status[0] = '\0';
    data_ptr->max_sockets = 100;
    data_ptr->max_udpdg = 1024;
  }
#endif

  // DEBUG
  /*
  auto xam = REX_KERNEL_STATE()->GetKernelModule<XamModule>("xam.xex");
  if (!xam->xnet()) {
    auto xnet = new XNet(REX_KERNEL_STATE());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return ret;
}

u32 NetDll_WSACleanup_entry(u32 caller) {
  // This does nothing. Xenia needs WSA running.
  return 0;
}

u32 NetDll_WSAGetLastError_entry() {
  return XThread::GetLastError();
}

// live_server as a network-order IPv4, loopback when unparseable.
static uint32_t LiveServerNBO() {
  const std::string ip = REXCVAR_GET(live_server);
  if (!ip.empty()) {
    const uint32_t parsed = inet_addr(ip.c_str());
    if (parsed != INADDR_NONE) {
      return parsed;
    }
  }
  return htonl(INADDR_LOOPBACK);
}

static bool LiveTrace() { return REXCVAR_GET(live_enabled) && REXCVAR_GET(live_trace); }

// ---------------------------------------------------------------------------
// Peer relay. Titles address other consoles by XNADDR and talk to them over
// secure UDP; here every console carries a 64-bit relay id in its XNADDR
// (abOnline[0..8]), each peer id maps to a private address (10.77.x.y), and
// datagrams to such an address are wrapped in a relay frame and sent to the
// server's relay port, which forwards them to the peer. Incoming frames are
// unwrapped and presented as coming from the peer's private address, so the
// title's sockets never see the difference.
namespace relay {

constexpr size_t kHeaderSize = 24;  // "FSR1", src id, dst id, src port, dst port
constexpr uint32_t kPrefix = 0x0A4D0000;  // 10.77.0.0/16, host order

struct Peer {
  uint64_t id;
  uint32_t ina;  // network order
};

static std::mutex g_mutex;
static std::vector<Peer> g_peers;
static std::vector<std::pair<uint64_t, uint16_t>> g_bound;  // native handle, port (host order)
static uint64_t g_own_id = 0;
static bool g_keepalive_started = false;

static uint64_t OwnId() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_own_id) {
    // The console identity when one is configured, otherwise a random id for
    // this process; peers learn it through the server anyway.
    const std::string xuid = rex::cvar::Query<std::string>("live_xuid");
    if (!xuid.empty()) {
      g_own_id = std::strtoull(xuid.c_str(), nullptr, 16);
    }
    if (!g_own_id) {
      std::random_device rd;
      g_own_id = (static_cast<uint64_t>(rd()) << 32) | rd();
      if (!g_own_id) g_own_id = 1;
    }
  }
  return g_own_id;
}

static bool IsRelayAddr(uint32_t ina_nbo) { return (ntohl(ina_nbo) & 0xFFFF0000u) == kPrefix; }

// Private address for a peer id, allocated on first sight.
static uint32_t InAddrFor(uint64_t id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const auto& peer : g_peers) {
    if (peer.id == id) return peer.ina;
  }
  const uint32_t n = static_cast<uint32_t>(g_peers.size() + 1) & 0xFFFF;
  const uint32_t ina = htonl(kPrefix | n);
  g_peers.push_back(Peer{id, ina});
  return ina;
}

static bool IdFor(uint32_t ina_nbo, uint64_t* id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (const auto& peer : g_peers) {
    if (peer.ina == ina_nbo) {
      *id = peer.id;
      return true;
    }
  }
  return false;
}

static void Put64(uint8_t* p, uint64_t v) {
  for (int i = 7; i >= 0; --i) {
    p[i] = static_cast<uint8_t>(v);
    v >>= 8;
  }
}
static uint64_t Get64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
  return v;
}

static void WriteHeader(uint8_t* p, uint64_t src, uint64_t dst, uint16_t sport, uint16_t dport) {
  p[0] = 'F'; p[1] = 'S'; p[2] = 'R'; p[3] = '1';
  Put64(p + 4, src);
  Put64(p + 12, dst);
  p[20] = static_cast<uint8_t>(sport >> 8); p[21] = static_cast<uint8_t>(sport);
  p[22] = static_cast<uint8_t>(dport >> 8); p[23] = static_cast<uint8_t>(dport);
}

static bool ParseHeader(const uint8_t* p, size_t len, uint64_t* src, uint64_t* dst,
                        uint16_t* sport, uint16_t* dport) {
  if (len < kHeaderSize || p[0] != 'F' || p[1] != 'S' || p[2] != 'R' || p[3] != '1') return false;
  *src = Get64(p + 4);
  *dst = Get64(p + 12);
  *sport = static_cast<uint16_t>((p[20] << 8) | p[21]);
  *dport = static_cast<uint16_t>((p[22] << 8) | p[23]);
  return true;
}

static sockaddr_in ServerEndpoint() {
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = LiveServerNBO();
  sa.sin_port = htons(static_cast<uint16_t>(REXCVAR_GET(live_relay_port)));
  return sa;
}

static bool FromServer(const N_XSOCKADDR_IN& from) {
  return from.sin_addr == LiveServerNBO() &&
         static_cast<uint16_t>(from.sin_port) == static_cast<uint16_t>(REXCVAR_GET(live_relay_port));
}

// A header-only frame tells the server which endpoint serves this console's
// port, so peers can reach a socket before it has sent anything itself.
static void Hello(uint64_t native_handle, uint16_t port) {
  uint8_t frame[kHeaderSize];
  WriteHeader(frame, OwnId(), 0, port, 0);
  const sockaddr_in to = ServerEndpoint();
  ::sendto(static_cast<SOCKET>(native_handle), reinterpret_cast<const char*>(frame),
           static_cast<int>(sizeof(frame)), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

static void RegisterBound(uint64_t native_handle, uint16_t port) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_bound.emplace_back(native_handle, port);
    if (!g_keepalive_started) {
      g_keepalive_started = true;
      std::thread([] {
        for (;;) {
          std::this_thread::sleep_for(std::chrono::seconds(10));
          std::vector<std::pair<uint64_t, uint16_t>> bound;
          {
            std::lock_guard<std::mutex> lock(g_mutex);
            bound = g_bound;
          }
          for (const auto& entry : bound) Hello(entry.first, entry.second);
        }
      }).detach();
    }
  }
  Hello(native_handle, port);
}

static void UnregisterBound(uint64_t native_handle) {
  std::lock_guard<std::mutex> lock(g_mutex);
  for (auto it = g_bound.begin(); it != g_bound.end();) {
    it = it->first == native_handle ? g_bound.erase(it) : it + 1;
  }
}

// Wraps a datagram for a peer address and sends it to the server. Returns
// the payload length the title expects, or -1.
static int SendTo(XSocket* socket, const uint8_t* buf, uint32_t len, const N_XSOCKADDR_IN& to) {
  uint64_t dst = 0;
  if (!IdFor(to.sin_addr, &dst)) return -1;
  std::vector<uint8_t> frame(kHeaderSize + len);
  WriteHeader(frame.data(), OwnId(), dst, socket->bound_port(), static_cast<uint16_t>(to.sin_port));
  std::memcpy(frame.data() + kHeaderSize, buf, len);
  const sockaddr_in server = ServerEndpoint();
  const int sent = ::sendto(static_cast<SOCKET>(socket->native_handle()),
                            reinterpret_cast<const char*>(frame.data()),
                            static_cast<int>(frame.size()), 0,
                            reinterpret_cast<const sockaddr*>(&server), sizeof(server));
  return sent < 0 ? -1 : static_cast<int>(len);
}

// Unwraps a frame received from the server in place: the payload moves to
// the buffer start and the source becomes the peer's private address.
// Header-only frames (server keepalives) leave nothing to deliver.
static void TranslateIncoming(uint8_t* buf, int* ret, N_XSOCKADDR_IN* from) {
  if (*ret < 0 || !from || !FromServer(*from)) return;
  uint64_t src, dst;
  uint16_t sport, dport;
  if (!ParseHeader(buf, static_cast<size_t>(*ret), &src, &dst, &sport, &dport)) return;
  const int payload = *ret - static_cast<int>(kHeaderSize);
  std::memmove(buf, buf + kHeaderSize, static_cast<size_t>(payload));
  from->sin_addr = InAddrFor(src);
  from->sin_port = sport;
  *ret = payload;
}

}  // namespace relay

// Overlapped receives. Winsock semantics: return 0 with the byte count when
// data is available at once (the overlapped structure and its event are still
// completed), otherwise -1 with WSA_IO_PENDING and completion later, which a
// helper thread does when the host socket becomes readable. Titles then use
// WSAGetOverlappedResult / the event to collect the result.
static void CompleteWsaOverlapped(uint32_t overlapped_guest, uint32_t status, uint32_t bytes) {
  auto* ov = REX_KERNEL_MEMORY()->TranslateVirtual<XWSAOVERLAPPED*>(overlapped_guest);
  ov->internal_high = bytes;
  ov->internal = status;  // last: readers poll this
  if (ov->event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(ov->event_handle);
    if (ev) {
      ev->Set(0, false);
    }
  }
}

struct WsaRecvRequest {
  uint32_t socket_handle;
  std::vector<std::pair<uint32_t, uint32_t>> buffers;  // guest ptr, len
  uint32_t from_guest;                                  // XSOCKADDR_IN* or 0
  uint32_t fromlen_guest;                               // u32* or 0
  uint32_t overlapped_guest;                            // XWSAOVERLAPPED*
  bool datagram;
};

// Performs one receive into the request's buffers. Returns bytes received, 0
// on orderly close, -1 with the Winsock error in *error when nothing could be
// read (WSAEWOULDBLOCK means try again later).
static int PerformWsaRecv(XSocket* socket, const WsaRecvRequest& req, int* error) {
  *error = 0;
  if (req.buffers.empty()) {
    return 0;
  }
  auto* memory = REX_KERNEL_MEMORY();
  // A single receive into the first buffer; the rest stay untouched (titles
  // pass one buffer here).
  uint8_t* buf = memory->TranslateVirtual(req.buffers[0].first);
  const uint32_t len = req.buffers[0].second;
  int ret;
  if (req.datagram) {
    N_XSOCKADDR_IN native_from;
    uint32_t native_fromlen = sizeof(native_from);
    ret = socket->RecvFrom(buf, len, 0, &native_from, &native_fromlen);
    if (REXCVAR_GET(live_enabled) && ret > 0) {
      relay::TranslateIncoming(buf, &ret, &native_from);
      if (ret == 0) {
        ret = -1;  // keepalive from the relay: keep waiting
#if REX_PLATFORM_WIN32
        WSASetLastError(WSAEWOULDBLOCK);
#else
        errno = EWOULDBLOCK;
#endif
      }
    }
    if (ret >= 0 && req.from_guest) {
      auto* from = memory->TranslateVirtual<XSOCKADDR_IN*>(req.from_guest);
      from->sin_family = native_from.sin_family;
      from->sin_port = native_from.sin_port;
      from->sin_addr = native_from.sin_addr;
      std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
      if (req.fromlen_guest) {
        memory::store_and_swap<uint32_t>(memory->TranslateVirtual(req.fromlen_guest), 16);
      }
    }
  } else {
    ret = socket->Recv(buf, len, 0);
  }
  if (ret == -1) {
#if REX_PLATFORM_WIN32
    *error = WSAGetLastError();
#else
    *error = errno == EWOULDBLOCK || errno == EAGAIN ? 0x2733 : 0x2745;
#endif
  }
  return ret;
}

static bool WsaWouldBlock(int error) {
#if REX_PLATFORM_WIN32
  return error == WSAEWOULDBLOCK;
#else
  return error == 0x2733;
#endif
}

// Waits on the host socket and completes the overlapped receive from a helper
// thread. The socket object is kept alive by the reference.
static void StartDeferredWsaRecv(rex::system::object_ref<XSocket> socket, WsaRecvRequest req) {
  std::thread([socket = std::move(socket), req = std::move(req)] {
    for (;;) {
      if (socket->native_handle() == static_cast<uint64_t>(-1)) {
        CompleteWsaOverlapped(req.overlapped_guest, 0x2745 /* WSAECONNABORTED */, 0);
        return;
      }
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(static_cast<SOCKET>(socket->native_handle()), &readfds);
      timeval tv{0, 50000};
      const int ready = ::select(0, &readfds, nullptr, nullptr, &tv);
      if (ready <= 0) {
        continue;
      }
      int error = 0;
      const int got = PerformWsaRecv(socket.get(), req, &error);
      if (got == -1 && WsaWouldBlock(error)) {
        continue;
      }
      CompleteWsaOverlapped(req.overlapped_guest, got < 0 ? static_cast<uint32_t>(error) : 0,
                            got < 0 ? 0 : static_cast<uint32_t>(got));
      return;
    }
  }).detach();
}

static u32 WsaRecvCommon(u32 socket_handle, ppc_ptr_t<XWSABUF> buffers_ptr, u32 buffer_count,
                         mapped_u32 num_bytes_recv, mapped_u32 flags_ptr, uint32_t from_guest,
                         uint32_t fromlen_guest, uint32_t overlapped_guest, bool datagram,
                         const char* name) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);
    return -1;
  }
  WsaRecvRequest req;
  req.socket_handle = socket_handle;
  for (uint32_t i = 0; i < buffer_count && buffers_ptr; ++i) {
    req.buffers.emplace_back(buffers_ptr[i].buf_ptr, buffers_ptr[i].len);
  }
  req.from_guest = from_guest;
  req.fromlen_guest = fromlen_guest;
  req.overlapped_guest = overlapped_guest;
  req.datagram = datagram;
  if (flags_ptr) {
    *flags_ptr = 0;
  }
  if (overlapped_guest) {
    auto* ov = REX_KERNEL_MEMORY()->TranslateVirtual<XWSAOVERLAPPED*>(overlapped_guest);
    ov->internal = 0x103;  // STATUS_PENDING
    ov->internal_high = 0;
  }
  int error = 0;
  const int got = PerformWsaRecv(socket.get(), req, &error);
  if (LiveTrace()) {
    REXKRNL_INFO("[live] {} sock={} bufs={} overlapped={:#x} -> {} err={:#x}", name, socket_handle,
                 buffer_count, overlapped_guest, got, error);
  }
  if (got >= 0) {
    if (num_bytes_recv) {
      *num_bytes_recv = static_cast<uint32_t>(got);
    }
    if (overlapped_guest) {
      CompleteWsaOverlapped(overlapped_guest, 0, static_cast<uint32_t>(got));
    }
    return 0;
  }
  if (WsaWouldBlock(error) && overlapped_guest) {
    StartDeferredWsaRecv(std::move(socket), std::move(req));
    XThread::SetLastError(997);  // WSA_IO_PENDING
    return -1;
  }
  XThread::SetLastError(static_cast<uint32_t>(error));
  return -1;
}

u32 NetDll_WSARecv_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers_ptr,
                         u32 buffer_count, mapped_u32 num_bytes_recv, mapped_u32 flags_ptr,
                         ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                         mapped_void completion_routine_ptr) {
  return WsaRecvCommon(socket_handle, buffers_ptr, buffer_count, num_bytes_recv, flags_ptr, 0, 0,
                       overlapped_ptr.guest_address(), false, "WSARecv");
}

u32 NetDll_WSARecvFrom_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers_ptr,
                             u32 buffer_count, mapped_u32 num_bytes_recv, mapped_u32 flags_ptr,
                             ppc_ptr_t<XSOCKADDR_IN> from_addr, mapped_u32 fromlen_ptr,
                             ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                             mapped_void completion_routine_ptr) {
  return WsaRecvCommon(socket_handle, buffers_ptr, buffer_count, num_bytes_recv, flags_ptr,
                       from_addr.guest_address(), fromlen_ptr.guest_address(),
                       overlapped_ptr.guest_address(), true, "WSARecvFrom");
}

// WSAGetOverlappedResult(socket, overlapped, bytes*, wait, flags*).
u32 NetDll_WSAGetOverlappedResult_entry(u32 caller, u32 socket_handle,
                                        ppc_ptr_t<XWSAOVERLAPPED> overlapped_ptr,
                                        mapped_u32 bytes_ptr, u32 wait, mapped_u32 flags_ptr) {
  if (!overlapped_ptr) {
    XThread::SetLastError(0x2726);  // WSAEINVAL
    return 0;
  }
  while (overlapped_ptr->internal == 0x103) {
    if (!wait) {
      XThread::SetLastError(996);  // WSA_IO_INCOMPLETE
      return 0;
    }
    rex::thread::Sleep(std::chrono::milliseconds(1));
  }
  if (bytes_ptr) {
    *bytes_ptr = static_cast<uint32_t>(overlapped_ptr->internal_high);
  }
  if (flags_ptr) {
    *flags_ptr = 0;
  }
  const uint32_t status = overlapped_ptr->internal;
  if (status != 0) {
    XThread::SetLastError(status);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASendTo_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XWSABUF> buffers,
                           u32 num_buffers, mapped_u32 num_bytes_sent, u32 flags,
                           ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len,
                           ppc_ptr_t<XWSAOVERLAPPED> overlapped, mapped_void completion_routine) {
  assert(!overlapped);
  assert(!completion_routine);

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  // Our sockets implementation doesn't support multiple buffers, so we need
  // to combine the buffers the game has given us!
  std::vector<uint8_t> combined_buffer_mem;
  uint32_t combined_buffer_size = 0;
  uint32_t combined_buffer_offset = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    combined_buffer_size += buffers[i].len;
    combined_buffer_mem.resize(combined_buffer_size);
    uint8_t* combined_buffer = combined_buffer_mem.data();

    std::memcpy(combined_buffer + combined_buffer_offset,
                REX_KERNEL_MEMORY()->TranslateVirtual(buffers[i].buf_ptr), buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  socket->SendTo(combined_buffer_mem.data(), combined_buffer_size, flags, &native_to, to_len);

  // TODO: Instantly complete overlapped

  return 0;
}

u32 NetDll_WSAWaitForMultipleEvents_entry(u32 num_events, mapped_u32 events, u32 wait_all,
                                          u32 timeout, u32 alertable) {
  if (LiveTrace()) {
    static uint32_t calls = 0;
    ++calls;
    if (calls <= 40 || (calls % 500) == 0) {
      REXKRNL_INFO("[live] WSAWaitForMultipleEvents n={} timeout={} (call {})", num_events, timeout,
                   calls);
    }
  }
  if (num_events > 64) {
    XThread::SetLastError(87);  // ERROR_INVALID_PARAMETER
    return ~0u;
  }

  // Kernel wait type: 0 = wait all, 1 = wait any. The timeout is a
  // relative NT interval (negative, 100 ns units).
  const uint32_t wait_type = wait_all ? 0 : 1;
  uint64_t timeout_wait = static_cast<uint64_t>(-static_cast<int64_t>(timeout) * 10000);

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(num_events, events, wait_type, 1, alertable,
                                                    timeout != -1 ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (result == X_STATUS_TIMEOUT) {
    XThread::SetLastError(258);  // WSA_WAIT_TIMEOUT
    return 258;
  }
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return ~0u;
  }
  // WSA_WAIT_EVENT_0 + index of the signalled event (0 for a wait-all).
  return result < num_events ? result : 0;
}

u32 NetDll_WSACreateEvent_entry() {
  XEvent* ev = new XEvent(REX_KERNEL_STATE());
  ev->Initialize(true, false);
  return ev->handle();
}

u32 NetDll_WSACloseEvent_entry(u32 event_handle) {
  X_STATUS result = REX_KERNEL_OBJECTS()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSAResetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

u32 NetDll_WSASetEvent_entry(u32 event_handle) {
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}

struct XnAddrStatus {
  // Address acquisition is not yet complete
  static const uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
  // XNet is uninitialized or no debugger found
  static const uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
  // Host has ethernet address (no IP address)
  static const uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
  // Host has statically assigned IP address
  static const uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
  // Host has DHCP assigned IP address
  static const uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
  // Host has PPPoE assigned IP address
  static const uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
  // Host has one or more gateways configured
  static const uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
  // Host has one or more DNS servers configured
  static const uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
  // Host is currently connected to online service
  static const uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  // Network configuration requires troubleshooting
  static const uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

u32 NetDll_XNetGetTitleXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  // Just return a loopback address atm.
  addr_ptr->ina.s_addr = htonl(INADDR_LOOPBACK);
  addr_ptr->inaOnline.s_addr = 0;
  addr_ptr->wPortOnline = 0;

  // TODO(gibbed): A proper mac address.
  // RakNet's 360 version appears to depend on abEnet to create "random" 64-bit
  // numbers. A zero value will cause RakPeer::Startup to fail. This causes
  // 58411436 to crash on startup.
  // The 360-specific code is scrubbed from the RakNet repo, but there's still
  // traces of what it's doing which match the game code.
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L382
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4527
  // https://github.com/facebookarchive/RakNet/blob/master/Source/RakPeer.cpp#L4467
  // "Mac address is a poor solution because you can't have multiple connections
  // from the same system"
  std::memset(addr_ptr->abEnet, 0xCC, 6);

  std::memset(addr_ptr->abOnline, 0, 20);
  if (REXCVAR_GET(live_enabled)) {
    // The relay id: peers map it to a private address (see relay::).
    const uint64_t id = relay::OwnId();
    relay::Put64(addr_ptr->abOnline, id);
    std::memcpy(addr_ptr->abEnet, addr_ptr->abOnline + 2, 6);
    addr_ptr->abEnet[0] = static_cast<uint8_t>((addr_ptr->abEnet[0] | 0x02) & 0xFE);  // local, unicast
  }

  if (REXCVAR_GET(live_enabled) && REXCVAR_GET(live_report_online)) {
    // Online: a routable-looking address with gateway and DNS, which is what
    // the title's "am I connected" checks look at. The real transport is the
    // socket layer; peers are reached through the server.
    addr_ptr->inaOnline.s_addr = LiveServerNBO();
    addr_ptr->wPortOnline = htons(3074);
    return XnAddrStatus::XNET_GET_XNADDR_ONLINE | XnAddrStatus::XNET_GET_XNADDR_GATEWAY |
           XnAddrStatus::XNET_GET_XNADDR_DNS | XnAddrStatus::XNET_GET_XNADDR_STATIC;
  }
  return XnAddrStatus::XNET_GET_XNADDR_STATIC;
}

u32 NetDll_XNetGetDebugXnAddr_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}

u32 NetDll_XNetXnAddrToMachineId_entry(u32 caller, ppc_ptr_t<XNADDR> addr_ptr, mapped_u32 id_ptr) {
  // Tell the caller we're not signed in to live (non-zero ret)
  return 1;
}

void NetDll_XNetInAddrToString_entry(u32 caller, u32 in_addr, mapped_string string_out,
                                     u32 string_size) {
  rex::string::copy_truncating(string_out, "666.666.666.666", string_size);
}

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address)
u32 NetDll_XNetXnAddrToInAddr_entry(u32 caller, ppc_ptr_t<XNADDR> xn_addr, mapped_void xid,
                                    mapped_void in_addr) {
  if (REXCVAR_GET(live_enabled)) {
    // A peer's XNADDR carries its relay id; it gets a private address that
    // the datagram paths route through the server's relay. Addresses
    // without an id (older peers, servers) resolve to the server itself.
    const uint64_t id = xn_addr ? relay::Get64(xn_addr->abOnline) : 0;
    const uint32_t redirect = id ? relay::InAddrFor(id) : LiveServerNBO();
    if (in_addr) {
      std::memcpy(in_addr.host_address(), &redirect, sizeof(redirect));
    }
    if (LiveTrace()) {
      REXKRNL_INFO("[live] XNetXnAddrToInAddr id={:#x} -> {:#x}", id, ntohl(redirect));
    }
    return 0;
  }
  return 1;
}

// XLSP: the title resolves a server's secure address for a service id. Every
// service lives on live_server.
u32 NetDll_XNetServerToInAddr_entry(u32 caller, u32 server_ina, u32 service_id,
                                    mapped_void in_addr) {
  if (!REXCVAR_GET(live_enabled)) {
    return 1;
  }
  const uint32_t redirect = LiveServerNBO();
  if (in_addr) {
    std::memcpy(in_addr.host_address(), &redirect, sizeof(redirect));
  }
  if (LiveTrace()) {
    REXKRNL_INFO("[live] XNetServerToInAddr ina={:#x} service={:#x} -> server", server_ina,
                 service_id);
  }
  return 0;
}

struct XConnectStatus {
  static const uint32_t XNET_CONNECT_STATUS_IDLE = 0;
  static const uint32_t XNET_CONNECT_STATUS_PENDING = 1;
  static const uint32_t XNET_CONNECT_STATUS_CONNECTED = 2;
  static const uint32_t XNET_CONNECT_STATUS_LOST = 3;
};

// Secure links are not emulated; report them established at once so the
// title proceeds to its sockets.
u32 NetDll_XNetConnect_entry(u32 caller, u32 ina) {
  if (LiveTrace()) {
    REXKRNL_INFO("[live] XNetConnect ina={:#x}", ina);
  }
  return 0;
}

u32 NetDll_XNetGetConnectStatus_entry(u32 caller, u32 ina) {
  if (LiveTrace()) {
    static uint32_t calls = 0;
    ++calls;
    if (calls <= 40 || (calls % 500) == 0) {
      REXKRNL_INFO("[live] XNetGetConnectStatus ina={:#x} (call {})", ina, calls);
    }
  }
  return REXCVAR_GET(live_enabled) ? XConnectStatus::XNET_CONNECT_STATUS_CONNECTED
                                   : XConnectStatus::XNET_CONNECT_STATUS_IDLE;
}

// Key registration for secure associations: accepted and ignored.
u32 NetDll_XNetRegisterKey_entry(u32 caller, mapped_void xnkid, mapped_void xnkey) {
  if (LiveTrace()) {
    REXKRNL_INFO("[live] XNetRegisterKey");
  }
  return 0;
}

u32 NetDll_XNetUnregisterKey_entry(u32 caller, mapped_void xnkid) {
  return 0;
}

u32 NetDll_XNetUnregisterInAddr_entry(u32 caller, u32 ina) {
  return 0;
}

u32 NetDll_XNetCreateKey_entry(u32 caller, mapped_void xnkid, mapped_void xnkey) {
  // 8-byte id + 16-byte key, random.
  if (xnkid) {
    auto* id = reinterpret_cast<uint8_t*>(xnkid.host_address());
    for (int i = 0; i < 8; ++i) {
      id[i] = static_cast<uint8_t>(std::rand());
    }
  }
  if (xnkey) {
    auto* key = reinterpret_cast<uint8_t*>(xnkey.host_address());
    for (int i = 0; i < 16; ++i) {
      key[i] = static_cast<uint8_t>(std::rand());
    }
  }
  return 0;
}

// Does the reverse of the above.
// FIXME: Arguments may not be correct.
u32 NetDll_XNetInAddrToXnAddr_entry(u32 caller, u32 in_addr, ppc_ptr_t<XNADDR> xn_addr,
                                    mapped_void xid) {
  uint64_t id = 0;
  if (!REXCVAR_GET(live_enabled) || !relay::IsRelayAddr(in_addr) || !relay::IdFor(in_addr, &id)) {
    return 1;
  }
  if (xn_addr) {
    xn_addr.Zero();
    xn_addr->ina.s_addr = in_addr;
    xn_addr->inaOnline.s_addr = in_addr;
    xn_addr->wPortOnline = htons(3074);
    relay::Put64(xn_addr->abOnline, id);
    std::memcpy(xn_addr->abEnet, xn_addr->abOnline + 2, 6);
    xn_addr->abEnet[0] = static_cast<uint8_t>((xn_addr->abEnet[0] | 0x02) & 0xFE);
  }
  if (xid) {
    std::memset(xid.host_address(), 0, 8);
  }
  return 0;
}

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
u32 NetDll_XNetSetSystemLinkPort_entry(u32 caller, u32 port) {
  return 1;
}

// https://github.com/ILOVEPIE/Cxbx-Reloaded/blob/master/src/CxbxKrnl/EmuXOnline.h#L39
struct XEthernetStatus {
  static const uint32_t XNET_ETHERNET_LINK_ACTIVE = 0x01;
  static const uint32_t XNET_ETHERNET_LINK_100MBPS = 0x02;
  static const uint32_t XNET_ETHERNET_LINK_10MBPS = 0x04;
  static const uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
  static const uint32_t XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;
};

u32 NetDll_XNetGetEthernetLinkStatus_entry(u32 caller) {
  if (REXCVAR_GET(live_enabled) && REXCVAR_GET(live_report_online)) {
    return XEthernetStatus::XNET_ETHERNET_LINK_ACTIVE |
           XEthernetStatus::XNET_ETHERNET_LINK_100MBPS |
           XEthernetStatus::XNET_ETHERNET_LINK_FULL_DUPLEX;
  }
  return 0;
}

u32 NetDll_XNetDnsLookup_entry(u32 caller, mapped_string host, u32 event_handle, mapped_u32 pdns) {
  // TODO(gibbed): actually implement this
  if (pdns) {
    auto dns_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNDNS));
    auto dns = REX_KERNEL_MEMORY()->TranslateVirtual<XNDNS*>(dns_guest);
    if (REXCVAR_GET(live_enabled)) {
      // Every host name the title looks up lives on the server.
      dns->status = 0;
      dns->cina = 1;
      dns->aina[0].s_addr = LiveServerNBO();
      if (LiveTrace()) {
        REXKRNL_INFO("[live] XNetDnsLookup '{}' -> server", host ? host.value() : "");
      }
    } else {
      dns->status = 1;  // non-zero = error
      dns->cina = 0;
    }
    *pdns = dns_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

u32 NetDll_XNetDnsRelease_entry(u32 caller, ppc_ptr_t<XNDNS> dns) {
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(dns.guest_address());
  return 0;
}

u32 NetDll_XNetQosServiceLookup_entry(u32 caller, u32 flags, u32 event_handle, mapped_u32 pqos) {
  if (LiveTrace()) {
    REXKRNL_INFO("[live] XNetQosServiceLookup flags={:#x}", flags);
  }
  // Set pqos as some games will try accessing it despite non-successful result
  if (pqos) {
    auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(sizeof(XNQOS));
    auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
    qos->count = qos->count_pending = 0;
    if (REXCVAR_GET(live_enabled)) {
      // A complete, healthy probe: open connection, low latency. Without it
      // the title decides it has no usable internet connection.
      qos->count = 1;
      qos->count_pending = 0;
      XNQOSINFO& q = qos->info[0];
      q.flags = 0x0B;  // complete, target contacted, data received
      q.reserved = 0;
      q.probes_xmit = 4;
      q.probes_recv = 4;
      q.data_len = 0;
      q.data_ptr = 0;
      q.rtt_min_in_msecs = 10;
      q.rtt_med_in_msecs = 15;
      q.up_bits_per_sec = 10000000;
      q.down_bits_per_sec = 10000000;
    }
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    assert_not_null(ev);
    ev->Set(0, false);
  }
  return 0;
}

// XNetQosLookup(cxnqos, apxnqos, apxnaddr, apxnkey, cina, aina, adwServiceId,
// cProbes, dwBitsPerSec, dwFlags, hEvent, ppxnqos): QoS probes against peers
// (by secure address) and title servers (by address + service id). No probes
// are sent; every target reports a complete, healthy result at once, which is
// what a title needs to proceed to connect.
u32 NetDll_XNetQosLookup_entry(u32 caller, u32 cxnqos, u32 apxnqos, u32 apxnaddr, u32 apxnkey,
                               u32 cina, u32 aina, u32 adwServiceId, u32 probe_count,
                               u32 bits_per_sec, u32 flags, u32 event_handle, mapped_u32 pqos) {
  const uint32_t count = std::max<uint32_t>(1, cxnqos + cina);
  if (LiveTrace()) {
    REXKRNL_INFO("[live] XNetQosLookup peers={} servers={} probes={} flags={:#x} event={:#x}",
                 cxnqos, cina, probe_count, flags, event_handle);
  }
  if (pqos) {
    const uint32_t size =
        static_cast<uint32_t>(sizeof(XNQOS) + (count - 1) * sizeof(XNQOSINFO));
    auto qos_guest = REX_KERNEL_MEMORY()->SystemHeapAlloc(size);
    auto qos = REX_KERNEL_MEMORY()->TranslateVirtual<XNQOS*>(qos_guest);
    std::memset(qos, 0, size);
    qos->count = count;
    qos->count_pending = 0;
    const uint16_t probes = static_cast<uint16_t>(std::max<uint32_t>(1, probe_count));
    for (uint32_t i = 0; i < count; ++i) {
      XNQOSINFO& q = qos->info[i];
      q.flags = 0x0B;  // complete, target contacted, data received
      q.probes_xmit = probes;
      q.probes_recv = probes;
      q.rtt_min_in_msecs = 10;
      q.rtt_med_in_msecs = 15;
      q.up_bits_per_sec = 10000000;
      q.down_bits_per_sec = 10000000;
    }
    *pqos = qos_guest;
  }
  if (event_handle) {
    auto ev = REX_KERNEL_OBJECTS()->LookupObject<XEvent>(event_handle);
    if (ev) {
      ev->Set(0, false);
    }
  }
  return 0;
}

u32 NetDll_XNetQosRelease_entry(u32 caller, ppc_ptr_t<XNQOS> qos) {
  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  REX_KERNEL_MEMORY()->SystemHeapFree(qos.guest_address());
  return 0;
}

u32 NetDll_XNetQosListen_entry(u32 caller, mapped_void id, mapped_void data, u32 data_size, u32 r7,
                               u32 flags) {
  return REXCVAR_GET(live_enabled) ? 0 : X_ERROR_FUNCTION_FAILED;
}

u32 NetDll_inet_addr_entry(mapped_string addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  uint32_t addr = inet_addr(addr_ptr);
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  // Based on console research it seems like x360 uses old version of inet_addr
  // In case of empty string it return 0 instead of -1
  if (addr == -1 && !addr_ptr.value().length()) {
    return 0;
  }

  return rex::byte_swap(addr);
}

u32 NetDll_socket_entry(u32 caller, u32 af, u32 type, u32 protocol) {
  if (LiveTrace()) {
    REXKRNL_INFO("[live] socket af={} type={} proto={}", af, type, protocol);
  }
  XSocket* socket = new XSocket(REX_KERNEL_STATE());
  X_STATUS result =
      socket->Initialize(XSocket::AddressFamily((uint32_t)af), XSocket::Type((uint32_t)type),
                         XSocket::Protocol((uint32_t)protocol));

  if (XFAILED(result)) {
    socket->Release();

    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return -1;
  }

  return socket->handle();
}

u32 NetDll_closesocket_entry(u32 caller, u32 socket_handle) {
  if (LiveTrace()) {
    REXKRNL_INFO("[live] closesocket sock={}", socket_handle);
  }
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }
  relay::UnregisterBound(socket->native_handle());

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  socket->Close();
  socket->ReleaseHandle();
  return 0;
}

i32 NetDll_shutdown_entry(u32 caller, u32 socket_handle, i32 how) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
#if REX_PLATFORM_WIN32
    uint32_t error_code = WSAGetLastError();
    XThread::SetLastError(error_code);
#else
    XThread::SetLastError(0x0);
#endif
  }
  return ret;
}

u32 NetDll_setsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval_ptr, u32 optlen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  return XSUCCEEDED(status) ? 0 : -1;
}

u32 NetDll_ioctlsocket_entry(u32 caller, u32 socket_handle, u32 cmd, mapped_void arg_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  // TODO
  return 0;
}

u32 NetDll_bind_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);
  if (REXCVAR_GET(net_loopback_only) && !REXCVAR_GET(live_enabled) && native_name.sin_addr == 0) {
    // INADDR_ANY -> loopback: keeps the socket usable for the title without
    // exposing a listener on the host network. Online play needs real binds.
    native_name.sin_addr = 0x7F000001;
  }
  if (LiveTrace()) {
    REXKRNL_INFO("[live] bind sock={} port={}", socket_handle, static_cast<uint16_t>(native_name.sin_port));
  }
  X_STATUS status = socket->Bind(&native_name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }
  if (REXCVAR_GET(live_enabled)) {
    int type = 0;
    int type_len = sizeof(type);
    if (getsockopt(static_cast<SOCKET>(socket->native_handle()), SOL_SOCKET, SO_TYPE,
                   reinterpret_cast<char*>(&type), &type_len) == 0 &&
        type == SOCK_DGRAM) {
      // Announce this port to the peer relay so other consoles can reach it.
      relay::RegisterBound(socket->native_handle(), socket->bound_port());
    }
  }

  return 0;
}

u32 NetDll_connect_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> name, u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_name(name);
  if (LiveTrace()) {
    const auto* in = reinterpret_cast<const sockaddr_in*>(&native_name);
    const uint8_t* ab = reinterpret_cast<const uint8_t*>(&in->sin_addr);
    REXKRNL_INFO("[live] connect sock={} -> {}.{}.{}.{}:{}", socket_handle, ab[0], ab[1], ab[2],
                 ab[3], ntohs(in->sin_port));
  }
  X_STATUS status = socket->Connect(&native_name, namelen);
  if (XFAILED(status)) {
#if REX_PLATFORM_WIN32
    // A non-blocking connect is "in progress": the title polls select() and
    // expects WSAEWOULDBLOCK, not a generic failure.
    const int nerr = WSAGetLastError();
    if (nerr == WSAEWOULDBLOCK || nerr == WSAEINPROGRESS || nerr == WSAEALREADY) {
      XThread::SetLastError(0x2733);
      return -1;
    }
#endif
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

// Guest XSOCKADDR_IN: be16 family, then port and address already in network
// order (copied raw, not swapped).
static void StoreGuestSockaddr(ppc_ptr_t<XSOCKADDR_IN> name, const sockaddr_in& addr) {
  auto* p = reinterpret_cast<uint8_t*>(name.host_address());
  memory::store_and_swap<uint16_t>(p + 0, 2 /* AF_INET */);
  std::memcpy(p + 2, &addr.sin_port, 2);
  std::memcpy(p + 4, &addr.sin_addr.s_addr, 4);
  std::memset(p + 8, 0, 8);
}

// EA's network library validates a connection with getpeername right after
// connect and checks SO_ERROR; stubs here made it drop the socket unused.
u32 NetDll_getpeername_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                             mapped_u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);
    return -1;
  }
  sockaddr_in peer = {};
#if REX_PLATFORM_WIN32
  int len = static_cast<int>(sizeof(peer));
  const int ret = ::getpeername(static_cast<SOCKET>(socket->native_handle()),
                                reinterpret_cast<sockaddr*>(&peer), &len);
#else
  socklen_t len = sizeof(peer);
  const int ret = ::getpeername(static_cast<int>(socket->native_handle()),
                                reinterpret_cast<sockaddr*>(&peer), &len);
#endif
  if (ret != 0) {
    XThread::SetLastError(0x2749);  // WSAENOTCONN
    return -1;
  }
  if (name) {
    StoreGuestSockaddr(name, peer);
  }
  if (namelen) {
    *namelen = 16u;
  }
  return 0;
}

u32 NetDll_getsockname_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR_IN> name,
                             mapped_u32 namelen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);
    return -1;
  }
  sockaddr_in local = {};
#if REX_PLATFORM_WIN32
  int len = static_cast<int>(sizeof(local));
  const int ret = ::getsockname(static_cast<SOCKET>(socket->native_handle()),
                                reinterpret_cast<sockaddr*>(&local), &len);
#else
  socklen_t len = sizeof(local);
  const int ret = ::getsockname(static_cast<int>(socket->native_handle()),
                                reinterpret_cast<sockaddr*>(&local), &len);
#endif
  if (ret != 0) {
    XThread::SetLastError(0x2749);
    return -1;
  }
  if (name) {
    StoreGuestSockaddr(name, local);
  }
  if (namelen) {
    *namelen = 16u;
  }
  return 0;
}

u32 NetDll_getsockopt_entry(u32 caller, u32 socket_handle, u32 level, u32 optname,
                            mapped_void optval, mapped_u32 optlen) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(0x2736);
    return -1;
  }
  // Every option reads as zero; for SO_ERROR that means "connected fine".
  if (optval) {
    memory::store_and_swap<uint32_t>(reinterpret_cast<uint8_t*>(optval.host_address()), 0);
  }
  if (optlen) {
    *optlen = 4u;
  }
  if (LiveTrace()) {
    REXKRNL_INFO("[live] getsockopt sock={} level={:#x} opt={:#x} -> 0", socket_handle, level,
                 optname);
  }
  return 0;
}

u32 NetDll_listen_entry(u32 caller, u32 socket_handle, i32 backlog) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    XThread::SetLastError(xboxkrnl::xeRtlNtStatusToDosError(status));
    return -1;
  }

  return 0;
}

u32 NetDll_accept_entry(u32 caller, u32 socket_handle, ppc_ptr_t<XSOCKADDR> addr_ptr,
                        mapped_u32 addrlen_ptr) {
  if (!addr_ptr) {
    // WSAEFAULT
    XThread::SetLastError(0x271E);
    return -1;
  }

  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = *addrlen_ptr;
  auto new_socket = socket->Accept(&native_addr, &native_len);
  if (new_socket) {
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data, *addrlen_ptr - 2);
    *addrlen_ptr = native_len;

    return new_socket->handle();
  } else {
    return -1;
  }
}

struct x_fd_set {
  rex::be<uint32_t> fd_count;
  rex::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[64];

  void Load(const x_fd_set* guest_set) {
    assert_true(guest_set->fd_count < 64);
    this->count = guest_set->fd_count;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        this->count = i;
        break;
      }
      // Convert from Xenia -> native
      auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
      assert_not_null(socket);
      this->sockets[i] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = socket->handle();
    }
  }

  void Store(fd_set* native_set) {
    FD_ZERO(native_set);
    for (uint32_t i = 0; i < this->count; ++i) {
      FD_SET(this->sockets[i]->native_handle(), native_set);
    }
  }

  void UpdateFrom(fd_set* native_set) {
    uint32_t new_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      if (FD_ISSET(socket->native_handle(), native_set)) {
        this->sockets[new_count++] = socket;
      }
    }
    this->count = new_count;
  }
};

i32 NetDll_select_entry(i32 caller, i32 nfds, ppc_ptr_t<x_fd_set> readfds,
                        ppc_ptr_t<x_fd_set> writefds, ppc_ptr_t<x_fd_set> exceptfds,
                        mapped_void timeout_ptr) {
  host_set host_readfds = {};
  fd_set native_readfds = {};
  if (readfds) {
    host_readfds.Load(readfds);
    host_readfds.Store(&native_readfds);
  }
  host_set host_writefds = {};
  fd_set native_writefds = {};
  if (writefds) {
    host_writefds.Load(writefds);
    host_writefds.Store(&native_writefds);
  }
  host_set host_exceptfds = {};
  fd_set native_exceptfds = {};
  if (exceptfds) {
    host_exceptfds.Load(exceptfds);
    host_exceptfds.Store(&native_exceptfds);
  }
  timeval* timeout_in = nullptr;
  timeval timeout;
  if (timeout_ptr) {
    timeout = {static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[0]),
               static_cast<int32_t>(timeout_ptr.as_array<int32_t>()[1])};
    chrono::Clock::ScaleGuestDurationTimeval(reinterpret_cast<int32_t*>(&timeout.tv_sec),
                                             reinterpret_cast<int32_t*>(&timeout.tv_usec));
    timeout_in = &timeout;
  }
  int ret = select(nfds, readfds ? &native_readfds : nullptr, writefds ? &native_writefds : nullptr,
                   exceptfds ? &native_exceptfds : nullptr, timeout_in);
  if (readfds) {
    host_readfds.UpdateFrom(&native_readfds);
    host_readfds.Store(readfds);
  }
  if (writefds) {
    host_writefds.UpdateFrom(&native_writefds);
    host_writefds.Store(writefds);
  }
  if (exceptfds) {
    host_exceptfds.UpdateFrom(&native_exceptfds);
    host_exceptfds.Store(exceptfds);
  }

  // TODO(gibbed): modify ret to be what's actually copied to the guest fd_sets?
  return ret;
}

u32 NetDll_recv_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  const int ret = socket->Recv(buf_ptr, buf_len, flags);
  if (LiveTrace()) {
    REXKRNL_INFO("[live] recv sock={} len={} -> {}", socket_handle, buf_len, ret);
  }
  if (ret == -1) {
    // Like recvfrom: the title drains until "would block" and checks the
    // last error for exactly that; a stale error tears the connection down.
#if REX_PLATFORM_WIN32
    XThread::SetLastError(WSAGetLastError());
#else
    XThread::SetLastError(0x2733);
#endif
  }
  return ret;
}

u32 NetDll_recvfrom_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len,
                          u32 flags, ppc_ptr_t<XSOCKADDR_IN> from_ptr, mapped_u32 fromlen_ptr) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  int ret =
      socket->RecvFrom(buf_ptr, buf_len, flags, &native_from, fromlen_ptr ? &native_fromlen : 0);
  if (REXCVAR_GET(live_enabled) && ret > 0) {
    relay::TranslateIncoming(buf_ptr, &ret, &native_from);
    if (ret == 0) {
      // A relay keepalive: nothing for the title.
      XThread::SetLastError(0x2733);  // WSAEWOULDBLOCK
      return -1;
    }
  }

  if (from_ptr) {
    from_ptr->sin_family = native_from.sin_family;
    from_ptr->sin_port = native_from.sin_port;
    from_ptr->sin_addr = native_from.sin_addr;
    std::memset(from_ptr->x_sin_zero, 0, sizeof(from_ptr->x_sin_zero));
  }
  if (fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  if (ret == -1) {
// TODO: Better way of getting the error code
#if REX_PLATFORM_WIN32
    uint32_t error_code = WSAGetLastError();
    XThread::SetLastError(error_code);
#else
    XThread::SetLastError(0x0);
#endif
  }

  return ret;
}

u32 NetDll_send_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  const int ret = socket->Send(buf_ptr, buf_len, flags);
  if (LiveTrace()) {
    REXKRNL_INFO("[live] send sock={} len={} -> {}", socket_handle, buf_len, ret);
  }
  return ret;
}

u32 NetDll_sendto_entry(u32 caller, u32 socket_handle, mapped_void buf_ptr, u32 buf_len, u32 flags,
                        ppc_ptr_t<XSOCKADDR_IN> to_ptr, u32 to_len) {
  auto socket = REX_KERNEL_OBJECTS()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    // WSAENOTSOCK
    XThread::SetLastError(0x2736);
    return -1;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  if (REXCVAR_GET(live_enabled) && relay::IsRelayAddr(native_to.sin_addr)) {
    const int sent = relay::SendTo(socket.get(), buf_ptr, buf_len, native_to);
    if (sent < 0) {
      XThread::SetLastError(0x2751);  // WSAEHOSTUNREACH
      return -1;
    }
    return sent;
  }
  return socket->SendTo(buf_ptr, buf_len, flags, &native_to, to_len);
}

u32 NetDll___WSAFDIsSet_entry(u32 socket_handle, ppc_ptr_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count = std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}

void NetDll_WSASetLastError_entry(u32 error_code) {
  XThread::SetLastError(error_code);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex

REX_EXPORT(__imp__NetDll_XNetStartup, rex::kernel::xam::NetDll_XNetStartup_entry)
REX_EXPORT(__imp__NetDll_XNetCleanup, rex::kernel::xam::NetDll_XNetCleanup_entry)
REX_EXPORT(__imp__NetDll_XNetGetOpt, rex::kernel::xam::NetDll_XNetGetOpt_entry)
REX_EXPORT(__imp__NetDll_XNetRandom, rex::kernel::xam::NetDll_XNetRandom_entry)
REX_EXPORT(__imp__NetDll_WSAStartup, rex::kernel::xam::NetDll_WSAStartup_entry)
REX_EXPORT(__imp__NetDll_WSACleanup, rex::kernel::xam::NetDll_WSACleanup_entry)
REX_EXPORT(__imp__NetDll_WSAGetLastError, rex::kernel::xam::NetDll_WSAGetLastError_entry)
REX_EXPORT(__imp__NetDll_WSARecvFrom, rex::kernel::xam::NetDll_WSARecvFrom_entry)
REX_EXPORT(__imp__NetDll_WSASendTo, rex::kernel::xam::NetDll_WSASendTo_entry)
REX_EXPORT(__imp__NetDll_WSAWaitForMultipleEvents,
           rex::kernel::xam::NetDll_WSAWaitForMultipleEvents_entry)
REX_EXPORT(__imp__NetDll_WSACreateEvent, rex::kernel::xam::NetDll_WSACreateEvent_entry)
REX_EXPORT(__imp__NetDll_WSACloseEvent, rex::kernel::xam::NetDll_WSACloseEvent_entry)
REX_EXPORT(__imp__NetDll_WSAResetEvent, rex::kernel::xam::NetDll_WSAResetEvent_entry)
REX_EXPORT(__imp__NetDll_WSASetEvent, rex::kernel::xam::NetDll_WSASetEvent_entry)
REX_EXPORT(__imp__NetDll_XNetGetTitleXnAddr, rex::kernel::xam::NetDll_XNetGetTitleXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetGetDebugXnAddr, rex::kernel::xam::NetDll_XNetGetDebugXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToMachineId,
           rex::kernel::xam::NetDll_XNetXnAddrToMachineId_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToString, rex::kernel::xam::NetDll_XNetInAddrToString_entry)
REX_EXPORT(__imp__NetDll_XNetXnAddrToInAddr, rex::kernel::xam::NetDll_XNetXnAddrToInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetInAddrToXnAddr, rex::kernel::xam::NetDll_XNetInAddrToXnAddr_entry)
REX_EXPORT(__imp__NetDll_XNetSetSystemLinkPort,
           rex::kernel::xam::NetDll_XNetSetSystemLinkPort_entry)
REX_EXPORT(__imp__NetDll_XNetGetEthernetLinkStatus,
           rex::kernel::xam::NetDll_XNetGetEthernetLinkStatus_entry)
REX_EXPORT(__imp__NetDll_XNetDnsLookup, rex::kernel::xam::NetDll_XNetDnsLookup_entry)
REX_EXPORT(__imp__NetDll_XNetDnsRelease, rex::kernel::xam::NetDll_XNetDnsRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosServiceLookup, rex::kernel::xam::NetDll_XNetQosServiceLookup_entry)
REX_EXPORT(__imp__NetDll_XNetQosRelease, rex::kernel::xam::NetDll_XNetQosRelease_entry)
REX_EXPORT(__imp__NetDll_XNetQosListen, rex::kernel::xam::NetDll_XNetQosListen_entry)
REX_EXPORT(__imp__NetDll_inet_addr, rex::kernel::xam::NetDll_inet_addr_entry)
REX_EXPORT(__imp__NetDll_socket, rex::kernel::xam::NetDll_socket_entry)
REX_EXPORT(__imp__NetDll_closesocket, rex::kernel::xam::NetDll_closesocket_entry)
REX_EXPORT(__imp__NetDll_shutdown, rex::kernel::xam::NetDll_shutdown_entry)
REX_EXPORT(__imp__NetDll_setsockopt, rex::kernel::xam::NetDll_setsockopt_entry)
REX_EXPORT(__imp__NetDll_ioctlsocket, rex::kernel::xam::NetDll_ioctlsocket_entry)
REX_EXPORT(__imp__NetDll_bind, rex::kernel::xam::NetDll_bind_entry)
REX_EXPORT(__imp__NetDll_connect, rex::kernel::xam::NetDll_connect_entry)
REX_EXPORT(__imp__NetDll_listen, rex::kernel::xam::NetDll_listen_entry)
REX_EXPORT(__imp__NetDll_accept, rex::kernel::xam::NetDll_accept_entry)
REX_EXPORT(__imp__NetDll_select, rex::kernel::xam::NetDll_select_entry)
REX_EXPORT(__imp__NetDll_recv, rex::kernel::xam::NetDll_recv_entry)
REX_EXPORT(__imp__NetDll_recvfrom, rex::kernel::xam::NetDll_recvfrom_entry)
REX_EXPORT(__imp__NetDll_send, rex::kernel::xam::NetDll_send_entry)
REX_EXPORT(__imp__NetDll_sendto, rex::kernel::xam::NetDll_sendto_entry)
REX_EXPORT(__imp__NetDll___WSAFDIsSet, rex::kernel::xam::NetDll___WSAFDIsSet_entry)
REX_EXPORT(__imp__NetDll_WSASetLastError, rex::kernel::xam::NetDll_WSASetLastError_entry)

REX_EXPORT_STUB(__imp__NetDll_UpnpActionCalculateWorkBufferSize);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpActionGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpCleanup);
REX_EXPORT_STUB(__imp__NetDll_UpnpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpDescribeGetResults);
REX_EXPORT_STUB(__imp__NetDll_UpnpDoWork);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventGetCurrentState);
REX_EXPORT_STUB(__imp__NetDll_UpnpEventUnsubscribe);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchCreate);
REX_EXPORT_STUB(__imp__NetDll_UpnpSearchGetDevices);
REX_EXPORT_STUB(__imp__NetDll_UpnpStartup);
REX_EXPORT_STUB(__imp__NetDll_WSACancelOverlappedIO);
REX_EXPORT_STUB(__imp__NetDll_WSAEventSelect);
REX_EXPORT(__imp__NetDll_WSAGetOverlappedResult,
           rex::kernel::xam::NetDll_WSAGetOverlappedResult_entry)
REX_EXPORT(__imp__NetDll_WSARecv, rex::kernel::xam::NetDll_WSARecv_entry)
REX_EXPORT_STUB(__imp__NetDll_WSASend);
REX_EXPORT_STUB(__imp__NetDll_WSAStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XHttpCloseHandle);
REX_EXPORT_STUB(__imp__NetDll_XHttpConnect);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCrackUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrl);
REX_EXPORT_STUB(__imp__NetDll_XHttpCreateUrlW);
REX_EXPORT_STUB(__imp__NetDll_XHttpDoWork);
REX_EXPORT_STUB(__imp__NetDll_XHttpGetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpen);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpOpenRequestUsingMemory);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryAuthSchemes);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryHeaders);
REX_EXPORT_STUB(__imp__NetDll_XHttpQueryOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpReadData);
REX_EXPORT_STUB(__imp__NetDll_XHttpReceiveResponse);
REX_EXPORT_STUB(__imp__NetDll_XHttpResetPerfCounters);
REX_EXPORT_STUB(__imp__NetDll_XHttpSendRequest);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetCredentials);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetOption);
REX_EXPORT_STUB(__imp__NetDll_XHttpSetStatusCallback);
REX_EXPORT_STUB(__imp__NetDll_XHttpShutdown);
REX_EXPORT_STUB(__imp__NetDll_XHttpStartup);
REX_EXPORT_STUB(__imp__NetDll_XHttpWriteData);
REX_EXPORT(__imp__NetDll_XNetConnect, rex::kernel::xam::NetDll_XNetConnect_entry)
REX_EXPORT(__imp__NetDll_XNetCreateKey, rex::kernel::xam::NetDll_XNetCreateKey_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseLookup);
REX_EXPORT_STUB(__imp__NetDll_XNetDnsReverseRelease);
REX_EXPORT_STUB(__imp__NetDll_XNetGetBroadcastVersionStatus);
REX_EXPORT(__imp__NetDll_XNetGetConnectStatus, rex::kernel::xam::NetDll_XNetGetConnectStatus_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetGetSystemLinkPort);
REX_EXPORT_STUB(__imp__NetDll_XNetGetXnAddrPlatform);
REX_EXPORT_STUB(__imp__NetDll_XNetInAddrToServer);
REX_EXPORT_STUB(__imp__NetDll_XNetQosGetListenStats);
REX_EXPORT(__imp__NetDll_XNetQosLookup, rex::kernel::xam::NetDll_XNetQosLookup_entry)
REX_EXPORT(__imp__NetDll_XNetRegisterKey, rex::kernel::xam::NetDll_XNetRegisterKey_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetReplaceKey);
REX_EXPORT(__imp__NetDll_XNetServerToInAddr, rex::kernel::xam::NetDll_XNetServerToInAddr_entry)
REX_EXPORT_STUB(__imp__NetDll_XNetSetOpt);
REX_EXPORT_STUB(__imp__NetDll_XNetStartupEx);
REX_EXPORT_STUB(__imp__NetDll_XNetTsAddrToInAddr);
REX_EXPORT(__imp__NetDll_XNetUnregisterInAddr, rex::kernel::xam::NetDll_XNetUnregisterInAddr_entry)
REX_EXPORT(__imp__NetDll_XNetUnregisterKey, rex::kernel::xam::NetDll_XNetUnregisterKey_entry)
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadContinue);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetParseTime);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadGetReceivedDataSize);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStart);
REX_EXPORT_STUB(__imp__NetDll_XmlDownloadStop);
REX_EXPORT_STUB(__imp__NetDll_XnpCapture);
REX_EXPORT_STUB(__imp__NetDll_XnpConfig);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnP);
REX_EXPORT_STUB(__imp__NetDll_XnpConfigUPnPPortAndExternalAddr);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptRecv);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptSetExtendedReceiveCallback);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmit);
REX_EXPORT_STUB(__imp__NetDll_XnpEthernetInterceptXmitAsIp);
REX_EXPORT_STUB(__imp__NetDll_XnpGetActiveSocketList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetConfigStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpGetKeyList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetQosLookupList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetSecAssocList);
REX_EXPORT_STUB(__imp__NetDll_XnpGetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpLoadMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonClearQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetChallenge);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonGetStatus);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetChallengeResponse);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetPState);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQEvent);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQFlags);
REX_EXPORT_STUB(__imp__NetDll_XnpLogonSetQVals);
REX_EXPORT_STUB(__imp__NetDll_XnpNoteSystemTime);
REX_EXPORT_STUB(__imp__NetDll_XnpPersistTitleState);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetAggregateMeasurement);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryGetEntries);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistoryLoad);
REX_EXPORT_STUB(__imp__NetDll_XnpQosHistorySaveMeasurements);
REX_EXPORT_STUB(__imp__NetDll_XnpRegisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpReplaceKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveConfigParams);
REX_EXPORT_STUB(__imp__NetDll_XnpSaveMachineAccount);
REX_EXPORT_STUB(__imp__NetDll_XnpSetVlanXboxName);
REX_EXPORT_STUB(__imp__NetDll_XnpToolIpProxyInject);
REX_EXPORT_STUB(__imp__NetDll_XnpToolSetCallbacks);
REX_EXPORT_STUB(__imp__NetDll_XnpUnregisterKeyForCallerType);
REX_EXPORT_STUB(__imp__NetDll_XnpUpdateConfigParams);
REX_EXPORT(__imp__NetDll_getpeername, rex::kernel::xam::NetDll_getpeername_entry)
REX_EXPORT(__imp__NetDll_getsockname, rex::kernel::xam::NetDll_getsockname_entry)
REX_EXPORT(__imp__NetDll_getsockopt, rex::kernel::xam::NetDll_getsockopt_entry)
