/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <limits>
#include <atomic>
#include <mutex>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/math.h>

REXCVAR_DEFINE_BOOL(clock_no_scaling, false, "Clock",
                    "Disable clock scaling (inverted: false = scaling enabled)");

REXCVAR_DEFINE_BOOL(clock_source_raw, false, "Clock", "Use raw clock source without scaling");

namespace rex::chrono {

// Time scalar applied to all time operations.
double guest_time_scalar_ = 1.0;
// Tick frequency of guest.
uint64_t guest_tick_frequency_ = Clock::host_tick_frequency_platform();
// Base FILETIME of the guest system from app start.
uint64_t guest_system_time_base_ = Clock::QueryHostSystemTime();
// Guest time is host time re-based whenever the scalar or the frequency
// changes: guest = base_guest + (host - base_host) * ratio. The base is
// written under tick_mutex_ and published through a sequence number, so
// readers never take a lock. Several threads poll this clock continuously (a
// game's frame-pacing loop, the vblank timer, the GPU's waits); when the
// update was serialised through the mutex the readers stalled on each other,
// the vblank timer drifted and frames doubled during play.
struct ClockBase {
  std::atomic<uint64_t> host_ticks{0};
  std::atomic<uint64_t> guest_ticks{0};
  std::atomic<uint64_t> ratio_num{1};
  std::atomic<uint64_t> ratio_den{1};
};
ClockBase clock_base_;
std::atomic<uint32_t> clock_base_sequence_{0};
// Serialises writers of clock_base_.
std::mutex tick_mutex_;

struct ClockBaseSnapshot {
  uint64_t host_ticks;
  uint64_t guest_ticks;
  uint64_t ratio_num;
  uint64_t ratio_den;
};

ClockBaseSnapshot ReadClockBase() {
  for (;;) {
    const uint32_t before = clock_base_sequence_.load(std::memory_order_acquire);
    if (before & 1) {
      continue;  // a writer is mid-update
    }
    ClockBaseSnapshot snapshot{clock_base_.host_ticks.load(std::memory_order_relaxed),
                              clock_base_.guest_ticks.load(std::memory_order_relaxed),
                              clock_base_.ratio_num.load(std::memory_order_relaxed),
                              clock_base_.ratio_den.load(std::memory_order_relaxed)};
    std::atomic_thread_fence(std::memory_order_acquire);
    if (clock_base_sequence_.load(std::memory_order_relaxed) == before) {
      return snapshot;
    }
  }
}

uint64_t GuestTicksFromHost(const ClockBaseSnapshot& base, uint64_t host_ticks) {
  const uint64_t delta = host_ticks > base.host_ticks ? host_ticks - base.host_ticks : 0;
  return base.guest_ticks + delta * base.ratio_num / base.ratio_den;
}

void InitializeClockBase() {
  static const bool once = [] {
    clock_base_.host_ticks.store(Clock::QueryHostTickCount(), std::memory_order_relaxed);
    return true;
  }();
  (void)once;
}

void RecomputeGuestTickScalar() {
  // Create a rational number with numerator (first) and denominator (second)
  auto frac = std::make_pair(guest_tick_frequency_, Clock::QueryHostTickFrequency());
  // Doing it this way ensures we don't mess up our frequency scaling and
  // precisely controls the precision the guest_time_scalar_ can have.
  if (guest_time_scalar_ > 1.0) {
    frac.first *= static_cast<uint64_t>(guest_time_scalar_ * 10.0);
    frac.second *= 10;
  } else {
    frac.first *= 10;
    frac.second *= static_cast<uint64_t>(10.0 / guest_time_scalar_);
  }
  // Keep this a rational calculation and reduce the fraction
  reduce_fraction(frac);

  InitializeClockBase();
  std::lock_guard<std::mutex> lock(tick_mutex_);
  // Re-base so guest time continues from its current value with the new ratio.
  const uint64_t host_ticks = Clock::QueryHostTickCount();
  const uint64_t guest_ticks = GuestTicksFromHost(ReadClockBase(), host_ticks);
  clock_base_sequence_.fetch_add(1, std::memory_order_release);
  std::atomic_thread_fence(std::memory_order_release);
  clock_base_.host_ticks.store(host_ticks, std::memory_order_relaxed);
  clock_base_.guest_ticks.store(guest_ticks, std::memory_order_relaxed);
  clock_base_.ratio_num.store(frac.first, std::memory_order_relaxed);
  clock_base_.ratio_den.store(frac.second, std::memory_order_relaxed);
  clock_base_sequence_.fetch_add(1, std::memory_order_release);
}

// Guest tick count for the current host time. Lock-free; see ClockBase.
uint64_t UpdateGuestClock() {
  InitializeClockBase();
  const uint64_t host_tick_count = Clock::QueryHostTickCount();
  const ClockBaseSnapshot base = ReadClockBase();
  if (REXCVAR_GET(clock_no_scaling)) {
    // Nothing to re-base, calculate on the fly
    return host_tick_count * base.ratio_num / base.ratio_den;
  }
  return GuestTicksFromHost(base, host_tick_count);
}

// Offset of the current guest system file time relative to the guest base time.
inline uint64_t QueryGuestSystemTimeOffset() {
  if (REXCVAR_GET(clock_no_scaling)) {
    return Clock::QueryHostSystemTime() - guest_system_time_base_;
  }

  auto guest_tick_count = UpdateGuestClock();

  uint64_t numerator = 10000000;  // 100ns/10MHz resolution
  uint64_t denominator = guest_tick_frequency_;
  reduce_fraction(numerator, denominator);

  return guest_tick_count * numerator / denominator;
}

uint64_t Clock::QueryHostTickFrequency() {
#if REX_CLOCK_RAW_AVAILABLE
  if (REXCVAR_GET(clock_source_raw)) {
    return host_tick_frequency_raw();
  }
#endif
  return host_tick_frequency_platform();
}
uint64_t Clock::QueryHostTickCount() {
#if REX_CLOCK_RAW_AVAILABLE
  if (REXCVAR_GET(clock_source_raw)) {
    return host_tick_count_raw();
  }
#endif
  return host_tick_count_platform();
}

double Clock::guest_time_scalar() {
  return guest_time_scalar_;
}

void Clock::set_guest_time_scalar(double scalar) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return;
  }

  guest_time_scalar_ = scalar;
  RecomputeGuestTickScalar();
}

std::pair<uint64_t, uint64_t> Clock::guest_tick_ratio() {
  const ClockBaseSnapshot base = ReadClockBase();
  return std::make_pair(base.ratio_num, base.ratio_den);
}

uint64_t Clock::guest_tick_frequency() {
  return guest_tick_frequency_;
}

void Clock::set_guest_tick_frequency(uint64_t frequency) {
  guest_tick_frequency_ = frequency;
  RecomputeGuestTickScalar();
}

uint64_t Clock::guest_system_time_base() {
  return guest_system_time_base_;
}

void Clock::set_guest_system_time_base(uint64_t time_base) {
  guest_system_time_base_ = time_base;
}

uint64_t Clock::QueryGuestTickCount() {
  auto guest_tick_count = UpdateGuestClock();
  return guest_tick_count;
}

uint64_t Clock::QueryGuestSystemTime() {
  if (REXCVAR_GET(clock_no_scaling)) {
    return Clock::QueryHostSystemTime();
  }

  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  return guest_system_time_base_ + guest_system_time_offset;
}

uint32_t Clock::QueryGuestUptimeMillis() {
  return static_cast<uint32_t>(std::min<uint64_t>(QueryGuestSystemTimeOffset() / 10000,
                                                  std::numeric_limits<uint32_t>::max()));
}

void Clock::SetGuestSystemTime(uint64_t system_time) {
  if (REXCVAR_GET(clock_no_scaling)) {
    // Time is fixed to host time.
    return;
  }

  // Query the filetime offset to calculate a new base time.
  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  guest_system_time_base_ = system_time - guest_system_time_offset;
}

uint32_t Clock::ScaleGuestDurationMillis(uint32_t guest_ms) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return guest_ms;
  }

  constexpr uint64_t max = std::numeric_limits<uint32_t>::max();

  if (guest_ms >= max) {
    return max;
  } else if (!guest_ms) {
    return 0;
  }
  uint64_t scaled_ms =
      static_cast<uint64_t>((static_cast<uint64_t>(guest_ms) * guest_time_scalar_));
  return static_cast<uint32_t>(std::min(scaled_ms, max));
}

int64_t Clock::ScaleGuestDurationFileTime(int64_t guest_file_time) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return static_cast<uint64_t>(guest_file_time);
  }

  if (!guest_file_time) {
    return 0;
  } else if (guest_file_time > 0) {
    // Absolute time.
    uint64_t guest_time = Clock::QueryGuestSystemTime();
    int64_t relative_time = guest_file_time - static_cast<int64_t>(guest_time);
    int64_t scaled_time = static_cast<int64_t>(relative_time * guest_time_scalar_);
    return static_cast<int64_t>(guest_time) + scaled_time;
  } else {
    // Relative time.
    uint64_t scaled_file_time =
        static_cast<uint64_t>((static_cast<uint64_t>(guest_file_time) * guest_time_scalar_));
    // TODO(benvanik): check for overflow?
    return scaled_file_time;
  }
}

void Clock::ScaleGuestDurationTimeval(int32_t* tv_sec, int32_t* tv_usec) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return;
  }

  uint64_t scaled_sec = static_cast<uint64_t>(static_cast<uint64_t>(*tv_sec) * guest_time_scalar_);
  uint64_t scaled_usec =
      static_cast<uint64_t>(static_cast<uint64_t>(*tv_usec) * guest_time_scalar_);
  if (scaled_usec > std::numeric_limits<uint32_t>::max()) {
    uint64_t overflow_sec = scaled_usec / 1000000;
    scaled_usec -= overflow_sec * 1000000;
    scaled_sec += overflow_sec;
  }
  *tv_sec = int32_t(scaled_sec);
  *tv_usec = int32_t(scaled_usec);
}

}  // namespace rex::chrono
