#include "m68k/amiga_os.hpp"

#include <algorithm>
#include <cstring>

namespace m68k {

namespace {

constexpr Long kMemfFast = 0x4;        // exec/memory.h: MEMF_FAST (1L<<2)
constexpr Long kMemfClear = 0x10000;
constexpr Long kFileOutput = 0x1000;  // the BPTR returned by Output()
constexpr Long kFileInput = 0x1004;   // ... and by Input()
constexpr Long kErrorObjectNotFound = 205;
constexpr std::size_t kMaxLog = 4096;

struct Func {
    int offset;
    const char* name;
    unsigned version = 33;   // first library version that has the function (33 = Kickstart 1.2, 36 = Kickstart 2.0): exec_lib.fd / dos_lib.fd of the NDK
};

// exec.library vectors (name known; only some are emulated)
const Func kExec[] = {
    {-120, "Disable"}, {-126, "Enable"}, {-132, "Forbid"}, {-138, "Permit"}, {-198, "AllocMem"}, {-210, "FreeMem"}, {-216, "AvailMem"},
    {-234, "Insert"}, {-240, "AddHead"}, {-246, "AddTail"}, {-252, "Remove"}, {-258, "RemHead"}, {-264, "RemTail"}, {-270, "Enqueue"},
    {-276, "FindName"}, {-624, "CopyMem"}, {-630, "CopyMemQuick"},
    {-294, "FindTask"}, {-318, "Wait"}, {-324, "Signal"}, {-354, "AddPort"}, {-360, "RemPort"}, {-366, "PutMsg"},
    {-372, "GetMsg"}, {-378, "ReplyMsg"}, {-384, "WaitPort"}, {-408, "OldOpenLibrary"}, {-414, "CloseLibrary"},
    {-444, "OpenDevice"}, {-450, "CloseDevice"}, {-456, "DoIO"}, {-462, "SendIO"}, {-552, "OpenLibrary"},
    {-684, "AllocVec", 36}, {-690, "FreeVec", 36},   // NOT in the Kickstart 1.2/1.3 of an original Amiga 500
};
// dos.library vectors
const Func kDos[] = {
    {-30, "Open"}, {-36, "Close"}, {-42, "Read"}, {-48, "Write"}, {-54, "Input"}, {-60, "Output"}, {-66, "Seek"},
    {-72, "DeleteFile"}, {-84, "Lock"}, {-90, "UnLock"}, {-132, "IoErr"}, {-144, "Exit"}, {-150, "LoadSeg"},
    {-156, "UnLoadSeg"}, {-198, "Delay"},
};

const Func* findFunc(const std::string& library, int offset) {
    const Func* table = library == "exec.library" ? kExec : library == "dos.library" ? kDos : nullptr;
    const std::size_t n = library == "exec.library" ? sizeof kExec / sizeof kExec[0] : library == "dos.library" ? sizeof kDos / sizeof kDos[0] : 0;
    for (std::size_t i = 0; i < n; ++i)
        if (table[i].offset == offset) return &table[i];
    return nullptr;
}

}  // namespace

std::string AmigaOs::functionName(const std::string& library, int offset) {
    const Func* f = findFunc(library, offset);
    return f ? f->name : "";
}

unsigned AmigaOs::functionVersion(const std::string& library, int offset) {
    const Func* f = findFunc(library, offset);
    return f ? f->version : 0;
}

AmigaOs::AmigaOs(Memory& memory, Amiga500& amiga) : mem_(memory), amiga_(amiga) {
    const Address chipTop = amiga.config().chipKb * 1024U;
    osBase_ = chipTop - 0x6000;
    heapBase_ = chipTop / 2;
    heapEnd_ = osBase_ - 0x4000;
}

Long AmigaOs::rd32(Address a) const {
    return (Long{amiga_.readByte(a)} << 24) | (Long{amiga_.readByte(a + 1)} << 16) | (Long{amiga_.readByte(a + 2)} << 8) | amiga_.readByte(a + 3);
}
void AmigaOs::wr32(Address a, Long v) {
    for (int i = 0; i < 4; ++i) amiga_.writeByte(a + static_cast<Address>(i), static_cast<Byte>(v >> (24 - 8 * i)));
}
void AmigaOs::wr16(Address a, Word v) {
    amiga_.writeByte(a, static_cast<Byte>(v >> 8));
    amiga_.writeByte(a + 1, static_cast<Byte>(v));
}
std::string AmigaOs::readString(Address a, std::size_t max) const {
    std::string s;
    for (std::size_t i = 0; i < max; ++i) {
        const Byte b = amiga_.readByte(a + static_cast<Address>(i));
        if (b == 0) break;
        s += static_cast<char>(b);
    }
    return s;
}

void AmigaOs::install(Cpu& cpu) {
    cpu.executeHook = [this](Cpu& c) { return onExecute(c); };
    cpu.hookLow = kStubBase;
    cpu.hookHigh = kExitStub + 2;
}

void AmigaOs::uninstall(Cpu& cpu) {
    cpu.executeHook = nullptr;
    cpu.hookLow = cpu.hookHigh = 0;
}

void AmigaOs::setState(const State& s) { state_ = s; }

// Layout of the OS area (osBase_ ...): strings, a task, then one region per library of 0x900 bytes:
// 0x600 bytes of jump table below the base and 0x300 bytes above it. Library 0 is exec.library.
void AmigaOs::buildLibrary(unsigned index, const std::string& name) {
    const Address base = osBase_ + 0x800 + index * 0x900 + 0x600;
    // jump table: vector n lives at base - 6n and is JMP stub(index, n)
    for (unsigned n = 1; n <= 0x100; ++n) {
        const Address at = base - 6 * n;
        wr16(at, 0x4EF9);
        wr32(at + 2, kStubBase + index * 0x400 + n * 2);
    }
    // the library node: name string, type, version, sizes
    const Address nameAt = osBase_ + 0x10 + index * 0x20;
    for (std::size_t i = 0; i < 31; ++i) amiga_.writeByte(nameAt + static_cast<Address>(i), i < name.size() ? static_cast<Byte>(name[i]) : Byte{0});
    for (Address a = base; a < base + 0x300; ++a) amiga_.writeByte(a, 0);
    amiga_.writeByte(base + 8, 9);        // ln_Type = NT_LIBRARY
    wr32(base + 10, nameAt);              // ln_Name
    wr16(base + 16, 0x600);               // lib_NegSize
    wr16(base + 18, 0x300);               // lib_PosSize
    wr16(base + 20, 34);                  // lib_Version
}

void AmigaOs::reset() {
    state_ = State{};
    log_.clear();
    for (Address a = osBase_; a < osBase_ + 0x6000; ++a) amiga_.writeByte(a, 0);
    buildLibrary(0, "exec.library");
    state_.libraries.emplace_back("exec.library", execBase());
    wr32(4, execBase());                                   // absolute address 4 holds the ExecBase pointer
    // a minimal task (a process) at osBase+0x400, referenced by ExecBase.ThisTask ($114)
    const Address task = osBase_ + 0x400;
    amiga_.writeByte(task + 8, 13);                        // ln_Type = NT_PROCESS
    const Address taskName = osBase_ + 0x10 + 0x1E0;
    const char* tn = "main";
    for (std::size_t i = 0; i < 5; ++i) amiga_.writeByte(taskName + static_cast<Address>(i), static_cast<Byte>(tn[i]));
    wr32(task + 10, taskName);
    wr32(execBase() + 0x114, task);                        // ThisTask
    wr16(execBase() + 0x128, 0);                           // AttnFlags: plain 68000
}

void AmigaOs::prepareProgram(Cpu& cpu) {
    Registers& r = cpu.registers();
    r.d[0] = 0;
    r.a[0] = 0;
    r.a[7] = stackTop();
    r.a[7] -= 4;
    wr32(r.a[7], kExitStub);  // the program's final RTS lands in the exit stub
}

Address AmigaOs::allocate(Long size, Long) {
    if (size == 0 || size > heapEnd_ - heapBase_) return 0;
    const Address need = (size + 7U) & ~7U;
    std::vector<std::pair<Address, Address>> blocks = state_.blocks;
    std::sort(blocks.begin(), blocks.end());
    Address at = heapBase_;
    for (const auto& [a, n] : blocks) {
        if (a >= at + need) break;
        at = std::max(at, static_cast<Address>(a + ((n + 7U) & ~7U)));
    }
    if (at + need > heapEnd_) return 0;
    state_.blocks.emplace_back(at, size);
    return at;
}

void AmigaOs::release(Address block) {
    state_.blocks.erase(std::remove_if(state_.blocks.begin(), state_.blocks.end(), [&](const auto& b) { return b.first == block; }),
                        state_.blocks.end());
}

Address AmigaOs::openLibrary(const std::string& name) {
    for (const auto& [n, base] : state_.libraries)
        if (n == name) return base;
    if (state_.libraries.size() >= kMaxLibraries) return 0;
    const unsigned index = static_cast<unsigned>(state_.libraries.size());
    buildLibrary(index, name);
    const Address base = osBase_ + 0x800 + index * 0x900 + 0x600;
    state_.libraries.emplace_back(name, base);
    return base;
}

bool AmigaOs::onExecute(Cpu& cpu) {
    Registers& r = cpu.registers();
    const Address pc = r.pc & kAddressMask;
    if (pc == kExitStub) {
        cpu.requestStop("the program returned to the system (D0 = " + std::to_string(static_cast<std::int32_t>(r.d[0])) + ")");
        return true;
    }
    const unsigned index = (pc - kStubBase) / 0x400;
    const unsigned n = ((pc - kStubBase) % 0x400) / 2;
    if (index >= state_.libraries.size() || n == 0) return false;
    const std::string lib = state_.libraries[index].first;
    LibCall call;
    call.library = lib;
    call.offset = -6 * static_cast<int>(n);
    call.function = functionName(lib, call.offset);
    call.minVersion = functionVersion(lib, call.offset);
    call.caller = rd32(r.a[7]);
    call.d0 = r.d[0]; call.d1 = r.d[1]; call.d2 = r.d[2]; call.d3 = r.d[3];
    call.a0 = r.a[0]; call.a1 = r.a[1];
    dispatch(cpu, call, lib);
    call.result = r.d[0];
    log_.push_back(call);
    while (log_.size() > kMaxLog) log_.pop_front();
    if (!(call.function == "Exit" && call.emulated)) cpu.returnFromSubroutine();
    return true;
}

void AmigaOs::dispatch(Cpu& cpu, LibCall& c, const std::string& lib) {
    Registers& r = cpu.registers();
    const std::string& f = c.function;
    auto done = [&](Long d0) {
        r.d[0] = d0;
        c.emulated = true;
    };
    if (lib == "exec.library") {
        // exec autodoc AllocMem: "If MEMF_FAST is set, AllocMem() will fail on machines that only have chip memory!"
        // The heap of this emulation is chip RAM in any case, so MEMF_FAST is satisfied only if the machine has fast RAM at all (and then it is a chip address).
        const bool fastWithoutFastRam = (r.d[1] & kMemfFast) != 0 && amiga_.config().fastKb == 0;
        if (f == "AllocMem") {
            if (fastWithoutFastRam) {
                c.note = "MEMF_FAST, but the machine has no fast memory";
                return done(0);
            }
            const Address a = allocate(r.d[0], r.d[1]);
            if (a && (r.d[1] & kMemfClear))
                for (Long i = 0; i < r.d[0]; ++i) amiga_.writeByte(a + i, 0);
            c.note = a ? "" : "out of memory";
            return done(a);
        }
        if (f == "FreeMem") {
            const bool known = std::any_of(state_.blocks.begin(), state_.blocks.end(), [&](const auto& b) { return b.first == r.a[1]; });
            if (!known) c.note = "not an allocated block";
            release(r.a[1]);
            return done(r.d[0]);
        }
        if (f == "AllocVec") {   // V36 (Kickstart 2.0)
            if (fastWithoutFastRam) {
                c.note = "MEMF_FAST, but the machine has no fast memory";
                return done(0);
            }
            const Address a = allocate(r.d[0] + 8, r.d[1]);
            if (a) {
                wr32(a, r.d[0] + 8);
                if (r.d[1] & kMemfClear)
                    for (Long i = 0; i < r.d[0]; ++i) amiga_.writeByte(a + 8 + i, 0);
            }
            return done(a ? a + 8 : 0);
        }
        if (f == "FreeVec") {
            if (r.a[1]) release(r.a[1] - 8);
            return done(r.d[0]);
        }
        if (f == "OpenLibrary" || f == "OldOpenLibrary") {
            const std::string name = readString(r.a[1]);
            c.note = name;
            return done(name.empty() ? 0 : openLibrary(name));
        }
        if (f == "CloseLibrary") {
            c.note = r.a[1] ? "" : "null library";
            return done(r.d[0]);
        }
        if (f == "FindTask") {
            c.note = r.a[1] ? readString(r.a[1]) : std::string("this task");
            return done(rd32(execBase() + 0x114));
        }
        if (f == "Forbid" || f == "Permit" || f == "Disable" || f == "Enable") return done(r.d[0]);
        if (f == "AvailMem") {
            // free bytes of the heap (MEMF_LARGEST, $20000: the largest free block); the flags for chip/fast do not matter, there is only one heap
            std::vector<std::pair<Address, Address>> blocks = state_.blocks;
            std::sort(blocks.begin(), blocks.end());
            Address at = heapBase_, total = 0, largest = 0;
            for (const auto& [a, n] : blocks) {
                if (a > at) total += a - at, largest = std::max(largest, a - at);
                at = std::max(at, static_cast<Address>(a + ((n + 7U) & ~7U)));
            }
            if (heapEnd_ > at) total += heapEnd_ - at, largest = std::max(largest, heapEnd_ - at);
            if (fastWithoutFastRam) return done(0);   // no fast memory at all
            return done((r.d[1] & 0x20000U) ? largest : total);
        }
        if (f == "CopyMem" || f == "CopyMemQuick") {   // A0 source, A1 destination, D0 size in bytes; overlapping blocks are copied correctly
            const Long n = std::min<Long>(r.d[0], 0x1000000);
            std::vector<Byte> tmp;
            tmp.reserve(n);
            for (Long i = 0; i < n; ++i) tmp.push_back(amiga_.readByte(r.a[0] + i));
            for (Long i = 0; i < n; ++i) amiga_.writeByte(r.a[1] + i, tmp[i]);
            return done(r.d[0]);
        }
        // exec lists: struct List { lh_Head 0, lh_Tail 4, lh_TailPred 8, lh_Type 12 }, struct Node { ln_Succ 0, ln_Pred 4, ln_Type 8, ln_Pri 9, ln_Name 10 }
        if (f == "AddHead" || f == "AddTail") {
            const Address list = r.a[0], node = r.a[1];
            if (f == "AddHead") {
                const Address first = rd32(list);
                wr32(node, first);
                wr32(node + 4, list);
                wr32(first + 4, node);
                wr32(list, node);
            } else {
                const Address last = rd32(list + 8);
                wr32(node, list + 4);
                wr32(node + 4, last);
                wr32(last, node);
                wr32(list + 8, node);
            }
            return done(r.d[0]);
        }
        if (f == "Remove") {
            const Address node = r.a[1], next = rd32(node), prev = rd32(node + 4);
            wr32(prev, next);
            wr32(next + 4, prev);
            return done(r.d[0]);   // void in the autodocs: D0 stays as it was
        }
        if (f == "RemHead" || f == "RemTail") {
            const Address list = r.a[0];
            const Address node = f == "RemHead" ? rd32(list) : rd32(list + 8);
            if (rd32(node) == 0 || node == list + 4 || node == list) {   // the list is empty: the first node is the tail marker
                c.note = "empty list";
                return done(0);
            }
            const Address next = rd32(node), prev = rd32(node + 4);
            wr32(prev, next);
            wr32(next + 4, prev);
            return done(node);
        }
        if (f == "Insert") {   // A0 list, A1 node, A2 the node to insert behind (0: at the head)
            const Address list = r.a[0], node = r.a[1], pred = r.a[2] ? r.a[2] : list;
            const Address next = rd32(pred);
            wr32(node, next);
            wr32(node + 4, pred);
            wr32(next + 4, node);
            wr32(pred, node);
            return done(r.d[0]);
        }
        if (f == "Enqueue") {   // by priority (signed ln_Pri): behind the nodes of the same priority, before the first node of a lower one
            const Address list = r.a[0], node = r.a[1];
            const std::int8_t pri = static_cast<std::int8_t>(amiga_.readByte(node + 9));
            Address pred = list, cur = rd32(list);
            for (unsigned guard = 0; guard < 100000 && rd32(cur) != 0; ++guard) {
                if (static_cast<std::int8_t>(amiga_.readByte(cur + 9)) < pri) break;
                pred = cur;
                cur = rd32(cur);
            }
            const Address next = rd32(pred);
            wr32(node, next);
            wr32(node + 4, pred);
            wr32(next + 4, node);
            wr32(pred, node);
            return done(r.d[0]);
        }
        if (f == "FindName") {   // A0 list (or a node: the search starts behind it), A1 name; case sensitive
            const std::string wanted = readString(r.a[1], 200);
            Address cur = rd32(r.a[0]);
            for (unsigned guard = 0; guard < 100000 && rd32(cur) != 0; ++guard) {
                const Address name = rd32(cur + 10);
                if (name && readString(name, 200) == wanted) {
                    c.note = wanted;
                    return done(cur);
                }
                cur = rd32(cur);
            }
            c.note = wanted + " (not found)";
            return done(0);
        }
    } else if (lib == "dos.library") {
        if (f == "Output") return done(kFileOutput);
        if (f == "Input") return done(kFileInput);
        if (f == "Write") {
            if (r.d[1] != kFileOutput) {
                c.note = "not the standard output";
                state_.ioErr = 0;
                return done(0xFFFFFFFFU);
            }
            std::string text;
            for (Long i = 0; i < r.d[3] && i < 4096; ++i) text += static_cast<char>(amiga_.readByte(r.d[2] + i));
            state_.console += text;
            c.note = text;
            return done(r.d[3]);
        }
        if (f == "Read") return done(0);  // end of file
        if (f == "Close" || f == "Delay" || f == "UnLock" || f == "UnLoadSeg") return done(r.d[0]);
        if (f == "Open" || f == "Lock" || f == "LoadSeg") {
            c.note = "no files: " + readString(r.d[1]);
            state_.ioErr = kErrorObjectNotFound;
            return done(0);
        }
        if (f == "IoErr") return done(state_.ioErr);
        if (f == "Exit") {
            cpu.requestStop("the program called dos Exit(" + std::to_string(static_cast<std::int32_t>(r.d[1])) + ")");
            r.d[0] = r.d[1];
            c.emulated = true;
            return;
        }
    }
    // everything else: logged, D0 = 0
    r.d[0] = 0;
    c.emulated = false;
}

}  // namespace m68k
