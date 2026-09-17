#include <rex/input/script/script_input_driver.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include <rex/input/flags.h>
#include <rex/logging.h>

namespace rex::input::script {

namespace {

constexpr rex::input::DeviceId kScriptDevice = static_cast<rex::input::DeviceId>(0x53435200);

}  // namespace

ScriptInputDriver::ScriptInputDriver(rex::ui::Window* window, size_t window_z_order,
                                     std::string path)
    : InputDriver(window, window_z_order), path_(std::move(path)) {}

ScriptInputDriver::~ScriptInputDriver() = default;

X_STATUS ScriptInputDriver::Setup() {
  if (!LoadFile()) {
    return X_STATUS_UNSUCCESSFUL;
  }
  start_ = std::chrono::steady_clock::now();
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
  double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start_).count();
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
