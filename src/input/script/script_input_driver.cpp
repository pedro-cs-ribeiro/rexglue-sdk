#include <rex/input/script/script_input_driver.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

#include <rex/input/flags.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(input_script_anchor, "", "Input",
                      "Screen anchors as TOKEN[:TIME] pairs, comma-separated (e.g. "
                      "SCRN-SCEN-MainMenu:100,SCRN-SCEN-OnlineHub:200). Each time the game "
                      "reports a matching UI screen, the script clock is (re)set to that TIME, so "
                      "navigation presses fire relative to the screen actually appearing - robust "
                      "to variable login/boot time and to menus that flash before they settle. A "
                      "bare token uses input_script_anchor_time. Between anchors the clock is held "
                      "just below the next anchor's time so later presses never fire early.");
REXCVAR_DEFINE_UINT32(input_script_anchor_time, 100, "Input",
                      "Default anchor time (seconds) for a bare token in input_script_anchor.");

namespace rex::input::script {

namespace {

constexpr rex::input::DeviceId kScriptDevice = static_cast<rex::input::DeviceId>(0x53435200);

// Screen-anchoring state (one script driver per process). Each anchor maps a UI
// screen token to a point on the script timeline. When the game reports a
// screen, the clock is re-based to that anchor's time; the reported time then
// advances from there, capped just below the next anchor so presses meant for a
// later screen wait until that screen appears. Re-basing on every occurrence
// makes it self-correcting: a premature/flashing menu keeps resetting until it
// settles, and a bounce back to an earlier screen retries that screen's nav.
struct AnchorPoint {
  std::string token;
  double time;
};
std::mutex g_anchor_mutex;
std::vector<AnchorPoint> g_anchors;  // sorted by time ascending
bool g_anchor_enabled = false;
double g_base_time = 0.0;
std::chrono::steady_clock::time_point g_base_point;

// Smallest anchor time strictly greater than t, or +inf if none.
double NextAnchorTimeAbove(double t) {
  double next = std::numeric_limits<double>::infinity();
  for (const auto& a : g_anchors) {
    if (a.time > t + 1e-6 && a.time < next) next = a.time;
  }
  return next;
}

}  // namespace

// Called by the game (from the online-log hook) whenever a UI screen event is
// seen. Exported so the recompiled engine can reach it.
void NotifyGuestScreen(const char* name) {
  if (!name) return;
  std::lock_guard<std::mutex> lock(g_anchor_mutex);
  if (!g_anchor_enabled) return;
  // Re-base to the highest-time anchor whose token is a substring of the event
  // (normally exactly one matches).
  const AnchorPoint* match = nullptr;
  for (const auto& a : g_anchors) {
    if (std::strstr(name, a.token.c_str()) && (!match || a.time > match->time)) match = &a;
  }
  if (match) {
    if (std::abs(g_base_time - match->time) > 1e-6) {
      REXLOG_INFO("Script input: anchored on screen '{}' -> timeline t={}s", match->token,
                  match->time);
    }
    g_base_time = match->time;
    g_base_point = std::chrono::steady_clock::now();
  }
}

ScriptInputDriver::ScriptInputDriver(rex::ui::Window* window, size_t window_z_order,
                                     std::string path)
    : InputDriver(window, window_z_order), path_(std::move(path)) {}

ScriptInputDriver::~ScriptInputDriver() = default;

X_STATUS ScriptInputDriver::Setup() {
  if (!LoadFile()) {
    return X_STATUS_UNSUCCESSFUL;
  }
  start_ = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lock(g_anchor_mutex);
    g_anchors.clear();
    const std::string spec = REXCVAR_GET(input_script_anchor);
    const double default_time = static_cast<double>(REXCVAR_GET(input_script_anchor_time));
    size_t pos = 0;
    while (pos <= spec.size()) {
      size_t comma = spec.find(',', pos);
      std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
      pos = comma == std::string::npos ? spec.size() + 1 : comma + 1;
      // trim spaces
      while (!item.empty() && std::isspace(static_cast<unsigned char>(item.front()))) item.erase(item.begin());
      while (!item.empty() && std::isspace(static_cast<unsigned char>(item.back()))) item.pop_back();
      if (item.empty()) continue;
      size_t colon = item.find(':');
      AnchorPoint a;
      if (colon == std::string::npos) {
        a.token = item;
        a.time = default_time;
      } else {
        a.token = item.substr(0, colon);
        try {
          a.time = std::stod(item.substr(colon + 1));
        } catch (...) {
          a.time = default_time;
        }
      }
      if (!a.token.empty()) g_anchors.push_back(std::move(a));
    }
    std::sort(g_anchors.begin(), g_anchors.end(),
              [](const AnchorPoint& x, const AnchorPoint& y) { return x.time < y.time; });
    g_anchor_enabled = !g_anchors.empty();
    g_base_time = 0.0;
    g_base_point = start_;
  }
  if (g_anchor_enabled) {
    std::string list;
    for (const auto& a : g_anchors) list += (list.empty() ? "" : ", ") + a.token + ":" + std::to_string(a.time);
    REXLOG_INFO("Script input: navigation anchored to screens [{}]", list);
  }
  REXLOG_INFO("Script input: replaying {} entries from {}", entries_.size(), path_);
  return X_STATUS_SUCCESS;
}

bool ScriptInputDriver::LoadFile() {
  std::ifstream file(path_);
  if (!file) {
    REXLOG_ERROR("Script input: cannot open {}", path_);
    return false;
  }
  std::string line;
  size_t line_number = 0;
  while (std::getline(file, line)) {
    ++line_number;
    auto hash = line.find('#');
    if (hash != std::string::npos) {
      line.erase(hash);
    }
    std::istringstream in(line);
    Entry entry;
    std::string buttons;
    if (!(in >> entry.time >> buttons)) {
      continue;  // blank or comment-only line
    }
    entry.buttons = static_cast<uint16_t>(std::stoul(buttons, nullptr, 16));
    int lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
    in >> lx >> ly >> rx >> ry >> lt >> rt;
    entry.thumb_lx = static_cast<int16_t>(std::clamp(lx, -32768, 32767));
    entry.thumb_ly = static_cast<int16_t>(std::clamp(ly, -32768, 32767));
    entry.thumb_rx = static_cast<int16_t>(std::clamp(rx, -32768, 32767));
    entry.thumb_ry = static_cast<int16_t>(std::clamp(ry, -32768, 32767));
    entry.left_trigger = static_cast<uint8_t>(std::clamp(lt, 0, 255));
    entry.right_trigger = static_cast<uint8_t>(std::clamp(rt, 0, 255));
    entries_.push_back(entry);
  }
  std::stable_sort(entries_.begin(), entries_.end(),
                   [](const Entry& a, const Entry& b) { return a.time < b.time; });
  return true;
}

const ScriptInputDriver::Entry* ScriptInputDriver::EntryAt(double seconds) const {
  const Entry* current = nullptr;
  for (const auto& entry : entries_) {
    if (entry.time > seconds) {
      break;
    }
    current = &entry;
  }
  return current;
}

void ScriptInputDriver::EnumerateDevices(std::vector<DeviceInfo>& out) {
  DeviceInfo info;
  info.id = kScriptDevice;
  info.name = "Scripted";
  info.synthetic = true;
  out.push_back(info);
}

X_RESULT ScriptInputDriver::GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                                  X_INPUT_CAPABILITIES* out_caps) {
  if (id != kScriptDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (out_caps) {
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
    out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    out_caps->gamepad.buttons = 0xFFFF;
    out_caps->gamepad.left_trigger = 0xFF;
    out_caps->gamepad.right_trigger = 0xFF;
    out_caps->gamepad.thumb_lx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ly = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_rx = static_cast<int16_t>(0x7FFF);
    out_caps->gamepad.thumb_ry = static_cast<int16_t>(0x7FFF);
    out_caps->vibration.left_motor_speed = 0xFFFF;
    out_caps->vibration.right_motor_speed = 0xFFFF;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ScriptInputDriver::GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) {
  if (id != kScriptDevice) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  std::lock_guard lock(mutex_);
  auto now = std::chrono::steady_clock::now();
  double seconds = std::chrono::duration<double>(now - start_).count();
  {
    std::lock_guard<std::mutex> anchor_lock(g_anchor_mutex);
    if (g_anchor_enabled) {
      // Advance from the current base (last screen seen), but hold just below
      // the next anchor's time so presses meant for a later screen wait for it.
      seconds = g_base_time + std::chrono::duration<double>(now - g_base_point).count();
      double cap = NextAnchorTimeAbove(g_base_time) - 0.001;
      if (seconds > cap) seconds = cap;
    }
  }
  Entry current;
  if (const Entry* entry = EntryAt(seconds)) {
    current = *entry;
  }
  if (std::memcmp(&current, &last_reported_, sizeof(Entry)) != 0) {
    ++packet_number_;
    REXLOG_DEBUG("Script input: t={:.1f}s buttons=0x{:04X} lx={} ly={}", seconds, current.buttons,
                 current.thumb_lx, current.thumb_ly);
    last_reported_ = current;
  }
  if (out_state) {
    std::memset(out_state, 0, sizeof(*out_state));
    out_state->packet_number = packet_number_;
    out_state->gamepad.buttons = current.buttons;
    out_state->gamepad.left_trigger = current.left_trigger;
    out_state->gamepad.right_trigger = current.right_trigger;
    out_state->gamepad.thumb_lx = current.thumb_lx;
    out_state->gamepad.thumb_ly = current.thumb_ly;
    out_state->gamepad.thumb_rx = current.thumb_rx;
    out_state->gamepad.thumb_ry = current.thumb_ry;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT ScriptInputDriver::SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* /*vibration*/) {
  return id == kScriptDevice ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT ScriptInputDriver::GetDeviceKeystroke(DeviceId id, uint32_t /*flags*/,
                                               X_INPUT_KEYSTROKE* /*out_keystroke*/) {
  return id == kScriptDevice ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
}

}  // namespace rex::input::script
