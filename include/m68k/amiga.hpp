#pragma once

#include <deque>
#include <string>
#include <vector>

#include "m68k/cpu.hpp"
#include "m68k/memory.hpp"

namespace m68k {

struct AmigaConfig {
    unsigned chipKb = 512;  ///< chip RAM at $000000 (A500: 512)
    unsigned slowKb = 0;    ///< "slow" RAM at $C00000 (A501 trapdoor expansion: 512)
    unsigned fastKb = 0;    ///< fast RAM at $200000
};

/// A write the program made to something that is not plain RAM.
struct DeviceWrite {
    enum class Kind { Custom, Cia, Rom, Unmapped } kind = Kind::Custom;
    Address address = 0;
    Size size = Size::W;
    Long value = 0;
    Address pc = 0;  ///< address of the writing instruction
};

/// The memory map of an Amiga 500, as far as it matters for looking at memory contents:
///
/// | range               | what                                                              |
/// |---------------------|-------------------------------------------------------------------|
/// | `$000000-`          | chip RAM (`chipKb`), the Kickstart ROM overlays it after a reset  |
/// | `$200000-`          | fast RAM (`fastKb`, optional)                                     |
/// | `$BF0000-$BFFFFF`   | CIA space: plain bytes (CIA-A at odd, CIA-B at even addresses)    |
/// | `$C00000-`          | "slow" RAM (`slowKb`, optional)                                   |
/// | `$DFF000-$DFFFFF`   | custom chip registers: plain storage, writes are logged           |
/// | `$F80000-$FFFFFF`   | Kickstart ROM (256 or 512 KiB, mirrored), read-only               |
///
/// Everything else is unmapped: reads give 0, writes are dropped and logged. **No device is
/// emulated** (no DMA, copper, blitter, sound, disk, video; no timing): custom registers and CIAs only
/// remember what was written. The one exception is the ROM overlay: it is on after `reset()` (if a ROM
/// is loaded) and turns off when the program writes CIA-A port A (`$BFE001`) bit 0 as 0 with the
/// corresponding direction bit (`$BFE201`) set - the Kickstart's way of switching RAM to address 0.
///
/// The addresses come from the Amiga Hardware Reference Manual as remembered; check them before relying on them.
class Amiga500 {
public:
    enum class Region { ChipRam, SlowRam, FastRam, Cia, Custom, Rom, Unmapped };

    Amiga500(Memory& memory, AmigaConfig config = {});

    /// Connects the memory map to the processor (readOverride/writeOverride).
    void install(Cpu& cpu);
    /// Detaches it again.
    void uninstall(Cpu& cpu);

    /// ROM image (256 or 512 KiB; any size > 0 is accepted and mirrored). An empty image removes the ROM.
    void loadRom(std::vector<Byte> rom);
    [[nodiscard]] bool hasRom() const { return !rom_.empty(); }

    /// Clears the custom/CIA registers and switches the overlay on. With a ROM, SSP and PC are taken from
    /// the ROM's first two long words (the overlay makes them appear at address 0).
    void reset(Cpu& cpu);

    [[nodiscard]] bool overlay() const { return overlay_ && hasRom(); }
    [[nodiscard]] Region classify(Address address) const;
    [[nodiscard]] const AmigaConfig& config() const { return config_; }

    /// Reads one byte as the processor would see it.
    [[nodiscard]] Byte readByte(Address address) const;
    /// Writes one byte like a program would (device registers are stored; ROM and unmapped are dropped).
    bool writeByte(Address address, Byte value);

    [[nodiscard]] const std::deque<DeviceWrite>& log() const { return log_; }
    void clearLog() { log_.clear(); }
    void setMaxLog(std::size_t n) { maxLog_ = n; }

    /// Name of a custom chip register at `address` (e.g. "DMACON"), or "" if not known.
    [[nodiscard]] static std::string customRegisterName(Address address);

    struct State {
        bool overlay = true;
        std::vector<Byte> custom, cia;
    };
    [[nodiscard]] State state() const { return {overlay_, custom_, cia_}; }
    void setState(const State& s);

    // for the processor hooks
    bool read(Address a, Size s, Long& value);
    bool write(Address a, Size s, Long value);

private:
    void updateOverlay();

    Memory& mem_;
    AmigaConfig config_;
    std::vector<Byte> rom_;
    std::vector<Byte> custom_;  // 4 KiB at $DFF000
    std::vector<Byte> cia_;     // 64 KiB at $BF0000
    bool overlay_ = true;
    Cpu* cpu_ = nullptr;
    std::deque<DeviceWrite> log_;
    std::size_t maxLog_ = 4096;
};

}  // namespace m68k
