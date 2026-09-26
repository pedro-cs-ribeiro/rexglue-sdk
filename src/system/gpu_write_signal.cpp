/**
 * ReXGlue runtime - GPU progress signal (see gpu_write_signal.h)
 */

#include <rex/system/gpu_write_signal.h>

#include <atomic>
#include <chrono>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace rex::system {

namespace {
std::atomic<uint32_t> g_sequence{0};
// Waiters register themselves so the GPU skips the wake call (a syscall) when
// nobody is blocked, which is the common case.
std::atomic<uint32_t> g_waiters{0};
}  // namespace

uint32_t GpuWriteSequence() {
  return g_sequence.load(std::memory_order_acquire);
}

void NotifyGpuMemoryWrite() {
  g_sequence.fetch_add(1, std::memory_order_acq_rel);
  if (g_waiters.load(std::memory_order_acquire)) {
#if defined(_WIN32)
    WakeByAddressAll(&g_sequence);
#endif
  }
}

void WaitForGpuMemoryWrite(uint32_t seen, uint32_t timeout_us) {
  g_waiters.fetch_add(1, std::memory_order_acq_rel);
  if (g_sequence.load(std::memory_order_acquire) == seen) {
#if defined(_WIN32)
    // WaitOnAddress takes milliseconds; round up so a short wait still blocks.
    WaitOnAddress(&g_sequence, &seen, sizeof(seen), (timeout_us + 999) / 1000);
#else
    std::this_thread::sleep_for(std::chrono::microseconds(timeout_us));
#endif
  }
  g_waiters.fetch_sub(1, std::memory_order_acq_rel);
}

}  // namespace rex::system
