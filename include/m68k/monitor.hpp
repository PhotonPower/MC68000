#pragma once

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "m68k/amiga.hpp"
#include "m68k/amiga_os.hpp"
#include "m68k/cpu.hpp"
#include "m68k/memory.hpp"
#include "m68k/snapshot.hpp"

namespace m68k {

struct MonitorOptions {
    bool allowFiles = true;                       ///< load/save/asm of host files
    std::uint64_t maxInstructions = 10'000'000;   ///< per run command (`limit` changes it)
};

/// Interactive machine-code monitor: memory, disassembler, assembler and the emulator's debugger
/// functions behind a classic one-line command interface (docs/monitor.md).
///
/// `execute` takes one input line and returns the text to print; it never throws and has no I/O of
/// its own, so the same class serves the terminal front end (`m68ktool monitor`) and tests.
/// Numbers in arguments are **hexadecimal** (`1000`, `FF`); other bases and expressions are available
/// through prefixes (`$FF`, `0x1000`, `%1010`), a trailing dot (`100.` = decimal), symbols (`start+3`)
/// and registers (`@A0`, `@PC`). Lines typed in assemble mode use the assembler's syntax (decimal!).
class Monitor {
public:
    explicit Monitor(MonitorOptions options = {});
    Monitor(const Monitor&) = delete;
    Monitor& operator=(const Monitor&) = delete;
    ~Monitor();

    /// Executes one command line. Returns the output (empty or ending in a newline).
    std::string execute(std::string_view line);

    /// Prompt to show before reading the next line ("-" normally, "001000:" in assemble mode).
    [[nodiscard]] std::string prompt() const;
    [[nodiscard]] bool finished() const { return finished_; }
    [[nodiscard]] static std::string banner();

    [[nodiscard]] Cpu& cpu() { return *cpu_; }
    [[nodiscard]] Memory& memory() { return *memory_; }

private:
    struct Impl;
    friend struct Impl;
    struct Watch {
        Address start = 0, length = 1;
        bool read = false, write = true;
    };

    MonitorOptions options_;
    std::unique_ptr<Memory> memory_;
    std::unique_ptr<Cpu> cpu_;
    std::unique_ptr<Amiga500> amiga_;  // null = generic machine
    std::unique_ptr<AmigaOs> os_;      // null = no emulated library calls
    std::map<std::string, std::int64_t> symbols_;
    std::map<std::string, Snapshot> checkpoints_;
    std::map<std::string, Amiga500::State> amigaCheckpoints_;
    std::map<std::string, AmigaOs::State> osCheckpoints_;
    std::vector<Watch> watches_;
    Address nextDisassembly_ = 0;
    Address nextDump_ = 0;
    std::optional<Address> assembleAt_;
    std::uint64_t limit_;
    bool finished_ = false;
};

}  // namespace m68k
