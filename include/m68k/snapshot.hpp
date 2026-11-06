#pragma once

#include <vector>

#include "m68k/cpu.hpp"
#include "m68k/image_io.hpp"
#include "m68k/memory.hpp"

namespace m68k {

/// The complete machine state: processor and memory. Resuming from a snapshot is bit-identical
/// to continuing the original run.
struct Snapshot {
    CpuState cpu;
    Memory memory;
};

[[nodiscard]] Snapshot capture(const Cpu& cpu);
/// Replaces the processor state and the memory contents of `cpu`. Breakpoints, watchpoints and hooks are kept.
void restore(Cpu& cpu, const Snapshot& snapshot);

/// File format: "M68KSNP1", the registers and flags (big-endian), then the 64 KiB memory pages that
/// contain a non-zero byte (page number + contents). Throws IoError on malformed data.
[[nodiscard]] std::vector<Byte> serialize(const Snapshot& snapshot);
[[nodiscard]] Snapshot deserializeSnapshot(const std::vector<Byte>& bytes);

/// A run of changed bytes between two memories.
struct MemoryChange {
    Address address = 0;
    std::vector<Byte> before, after;
};
/// Differences between two memories, adjacent changes (gap <= `gap` bytes) merged into one range.
/// At most `maxRanges` ranges are returned; `total` (if given) receives the number of changed bytes.
[[nodiscard]] std::vector<MemoryChange> diffMemory(const Memory& before, const Memory& after, std::size_t gap = 8,
                                                   std::size_t maxRanges = 1000, std::size_t* total = nullptr);

}  // namespace m68k
