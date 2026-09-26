/**
 * ReXGlue runtime - GPU progress signal
 *
 * The command processor bumps a sequence number and wakes waiters whenever
 * it writes guest memory (fences, scratch writebacks) or raises a GPU
 * interrupt. Guest code that polls memory for GPU progress can block on it
 * instead of spinning a core.
 */

#pragma once

#include <cstdint>

namespace rex::system {

// Current sequence number; read it BEFORE checking the condition you wait for.
uint32_t GpuWriteSequence();
// Called by the GPU after it wrote guest memory or dispatched an interrupt.
void NotifyGpuMemoryWrite();
// Blocks until the sequence differs from `seen` or `timeout_us` passes.
void WaitForGpuMemoryWrite(uint32_t seen, uint32_t timeout_us);

}  // namespace rex::system
