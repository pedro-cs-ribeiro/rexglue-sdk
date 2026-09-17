#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include <rex/input/input_driver.h>

namespace rex::input::script {

// Replays controller state from a text file on a timeline, as a synthetic
// gamepad on user 0. Meant for automated bring-up tests where no window focus
// or physical pad is available.
//
// File format, one entry per line, sorted by time (comments start with '#'):
//   <seconds> <buttons_hex> [thumb_lx thumb_ly thumb_rx thumb_ry lt rt]
// The state at time t is the last entry whose time is <= t; sticks are in
// -32768..32767 and triggers in 0..255 (all default to 0). Time starts when
// the driver is set up, i.e. at runtime initialisation.
class ScriptInputDriver final : public InputDriver {
 public:
  ScriptInputDriver(rex::ui::Window* window, size_t window_z_order, std::string path);
  ~ScriptInputDriver() override;

  X_STATUS Setup() override;

  void EnumerateDevices(std::vector<DeviceInfo>& out) override;
  X_RESULT GetDeviceState(DeviceId id, X_INPUT_STATE* out_state) override;
  X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t flags,
                                 X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT SetDeviceVibration(DeviceId id, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t flags,
                              X_INPUT_KEYSTROKE* out_keystroke) override;

 private:
  struct Entry {
    double time = 0.0;
    uint16_t buttons = 0;
    int16_t thumb_lx = 0;
    int16_t thumb_ly = 0;
    int16_t thumb_rx = 0;
    int16_t thumb_ry = 0;
    uint8_t left_trigger = 0;
    uint8_t right_trigger = 0;
  };

  bool LoadFile();
  const Entry* EntryAt(double seconds) const;

  std::string path_;
  std::vector<Entry> entries_;
  std::chrono::steady_clock::time_point start_;
  std::mutex mutex_;
  uint32_t packet_number_ = 0;
  Entry last_reported_;
};

}  // namespace rex::input::script
