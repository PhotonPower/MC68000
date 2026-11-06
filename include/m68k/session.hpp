#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "m68k/amiga.hpp"
#include "m68k/cpu.hpp"
#include "m68k/json.hpp"
#include "m68k/memory.hpp"
#include "m68k/snapshot.hpp"

namespace m68k {

struct SessionOptions {
    /// Allow commands that read/write files on the host (load_file, save_state, load_rom ...).
    /// Off by default: an agent should not get file access unless the operator opts in.
    bool allowFiles = false;
    std::size_t maxIoLog = 4096;  ///< newest device writes kept for the "io" command
};

/// One emulated machine plus what an agent needs to work with it: symbols, named checkpoints and a
/// JSON command interface (docs/agent-protocol.md).
///
/// A request is a JSON object {"cmd": "...", ...}; the reply is a JSON object with "ok": true plus the
/// results, or "ok": false and "error". `handle` never throws.
class Session {
public:
    explicit Session(SessionOptions options = {});
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session();

    Json handle(const Json& request);
    /// Parses one line of JSON, handles it and returns the reply as a single line.
    std::string handleLine(std::string_view line);

    [[nodiscard]] Cpu& cpu() { return *cpu_; }
    [[nodiscard]] Memory& memory() { return *memory_; }

private:
    struct Impl;
    friend struct Impl;

    struct Checkpoint {
        Snapshot machine;
        std::optional<Amiga500::State> amiga;
    };
    struct Watch {
        Address start;
        Address length;
        bool read, write;
    };

    void resetMachine(bool clearMemory);
    void applyWatches();

    SessionOptions options_;
    std::unique_ptr<Memory> memory_;  // heap: a Session is large
    std::unique_ptr<Cpu> cpu_;
    std::unique_ptr<Amiga500> amiga_;  // null = generic machine
    std::map<std::string, std::int64_t> symbols_;
    std::map<std::string, Checkpoint> checkpoints_;
    std::vector<Watch> watches_;
    std::vector<DeviceWrite> noLog_;
};

}  // namespace m68k
