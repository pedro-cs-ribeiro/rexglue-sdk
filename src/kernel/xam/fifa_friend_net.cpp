// FIFA Street: friend-picker network helpers. See fifa_friends.h.
//
// A tiny blocking HTTP/1.0 client that only ever talks to our own online
// server (live_server) over the easw HTTP port. Winsock is included first and
// in isolation so it does not fight the SDK's own socket headers.

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>

#include <rex/cvar.h>

#include "fifa_friends.h"

#pragma comment(lib, "ws2_32.lib")

namespace rex {
namespace kernel {
namespace xam {

const char16_t* xeXamGetCountryString(uint8_t id);  // xam_locale.cpp

namespace {

// ISO code of the console's country (user_country), e.g. "PT"; empty when the
// id has no code.
std::string ConsoleCountryCode() {
  const uint32_t id = rex::cvar::Query<uint32_t>("user_country");
  const char16_t* code = id <= 0xFF ? xeXamGetCountryString(static_cast<uint8_t>(id)) : nullptr;
  if (!code || !code[0] || !code[1] || (code[0] == u'Z' && code[1] == u'Z')) {
    return {};
  }
  return {static_cast<char>(code[0]), static_cast<char>(code[1])};
}

// The easw HTTP surface (which also serves the /fsr/ control routes) listens
// on port 80.
constexpr uint16_t kControlPort = 80;

void EnsureWinsock() {
  static bool started = false;
  if (!started) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    started = true;
  }
}

// Perform one blocking HTTP request and return the response body (after the
// header block), or an empty string on any error. Timeouts are short so a
// down server never wedges the calling game thread.
std::string HttpRequest(const std::string& method, const std::string& path,
                        const std::string& body) {
  std::string host = rex::cvar::GetFlagByName("live_server");
  if (host.empty()) {
    host = "127.0.0.1";
  }
  EnsureWinsock();
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) {
    return {};
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kControlPort);
  if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    closesocket(s);
    return {};
  }
  DWORD timeout_ms = 2000;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms),
             sizeof(timeout_ms));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms),
             sizeof(timeout_ms));
  if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    closesocket(s);
    return {};
  }
  std::ostringstream req;
  req << method << " " << path << " HTTP/1.0\r\n"
      << "Host: " << host << "\r\n"
      << "Content-Type: application/x-www-form-urlencoded\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n\r\n"
      << body;
  const std::string request = req.str();
  if (send(s, request.data(), static_cast<int>(request.size()), 0) == SOCKET_ERROR) {
    closesocket(s);
    return {};
  }
  std::string response;
  char buf[2048];
  int n;
  while ((n = recv(s, buf, sizeof(buf), 0)) > 0) {
    response.append(buf, static_cast<size_t>(n));
  }
  closesocket(s);
  auto pos = response.find("\r\n\r\n");
  return pos == std::string::npos ? std::string{} : response.substr(pos + 4);
}

}  // namespace

namespace {
// Parses "<id>\t<name>\t<xuid_hex>[\t<online 0|1>]" lines.
std::vector<FriendEntry> ParsePlayerLines(const std::string& body) {
  std::vector<FriendEntry> out;
  std::istringstream lines(body);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    // "<id>\t<name>\t<xuid_hex>"
    size_t t1 = line.find('\t');
    if (t1 == std::string::npos) {
      continue;
    }
    size_t t2 = line.find('\t', t1 + 1);
    if (t2 == std::string::npos) {
      continue;
    }
    FriendEntry e;
    e.id = static_cast<uint32_t>(std::strtoul(line.substr(0, t1).c_str(), nullptr, 10));
    e.name = line.substr(t1 + 1, t2 - t1 - 1);
    size_t t3 = line.find('\t', t2 + 1);
    e.xuid = std::strtoull(line.substr(t2 + 1, t3 == std::string::npos ? std::string::npos
                                                                        : t3 - t2 - 1)
                               .c_str(),
                           nullptr, 16);
    e.online = t3 != std::string::npos && line.compare(t3 + 1, 1, "1") == 0;
    out.push_back(std::move(e));
  }
  return out;
}
}  // namespace

std::vector<FriendEntry> FsrFetchPlayers(uint64_t self_xuid) {
  char path[64];
  std::snprintf(path, sizeof(path), "/fsr/players?self=%016llx",
                static_cast<unsigned long long>(self_xuid));
  return ParsePlayerLines(HttpRequest("GET", path, {}));
}

std::vector<FriendEntry> FsrFetchFriends(uint64_t self_xuid) {
  char path[64];
  std::snprintf(path, sizeof(path), "/fsr/friends?self=%016llx",
                static_cast<unsigned long long>(self_xuid));
  return ParsePlayerLines(HttpRequest("GET", path, {}));
}

std::vector<FriendEntry> FsrCachedFriends(uint64_t self_xuid) {
  static std::mutex mutex;
  static std::vector<FriendEntry> cached;
  static uint64_t cached_for = 0;
  static std::chrono::steady_clock::time_point fetched;
  std::lock_guard<std::mutex> lock(mutex);
  const auto now = std::chrono::steady_clock::now();
  if (cached_for != self_xuid || now - fetched > std::chrono::seconds(10)) {
    cached = FsrFetchFriends(self_xuid);
    cached_for = self_xuid;
    fetched = now;
  }
  return cached;
}

void FsrSendInvite(uint64_t from_xuid, uint32_t to_id) {
  char body[96];
  std::snprintf(body, sizeof(body), "from=%016llx&to=%u",
                static_cast<unsigned long long>(from_xuid), to_id);
  HttpRequest("POST", "/fsr/invite", body);
}

std::vector<InviteInfo> FsrFetchInvites(uint64_t self_xuid) {
  // The poll also tells the server the console's country (the title reports
  // an unknown country in its Blaze locale).
  char path[96];
  std::snprintf(path, sizeof(path), "/fsr/invites?self=%016llx&country=%s",
                static_cast<unsigned long long>(self_xuid), ConsoleCountryCode().c_str());
  std::string body = HttpRequest("GET", path, {});
  std::vector<InviteInfo> out;
  std::istringstream lines(body);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    // "!<text>": a notice for this player.
    if (line[0] == '!') {
      InviteInfo n;
      n.notice = line.substr(1);
      out.push_back(std::move(n));
      continue;
    }
    // "<from_id>\t<from_name>\t<game_id>"
    size_t t1 = line.find('\t');
    if (t1 == std::string::npos) {
      continue;
    }
    size_t t2 = line.find('\t', t1 + 1);
    if (t2 == std::string::npos) {
      continue;
    }
    InviteInfo e;
    e.from_id = static_cast<uint32_t>(std::strtoul(line.substr(0, t1).c_str(), nullptr, 10));
    e.from_name = line.substr(t1 + 1, t2 - t1 - 1);
    e.game_id = static_cast<uint32_t>(std::strtoul(line.substr(t2 + 1).c_str(), nullptr, 10));
    out.push_back(std::move(e));
  }
  return out;
}

void FsrAccept(uint64_t self_xuid, uint32_t from_id) {
  char body[96];
  std::snprintf(body, sizeof(body), "self=%016llx&from=%u",
                static_cast<unsigned long long>(self_xuid), from_id);
  HttpRequest("POST", "/fsr/accept", body);
}

}  // namespace xam
}  // namespace kernel
}  // namespace rex
