#include "m68k/amiga.hpp"

#include <algorithm>
#include <cstdio>

namespace m68k {

namespace {
constexpr Address kFastBase = 0x200000, kCiaBase = 0xBF0000, kCiaEnd = 0xC00000, kSlowBase = 0xC00000,
                  kCustomBase = 0xDFF000, kCustomEnd = 0xE00000, kRomBase = 0xF80000, kRomEnd = 0x1000000;
constexpr Address kOverlayWindow = 0x80000;  // the ROM is visible at address 0 up to 512 KiB
}  // namespace

Amiga500::Amiga500(Memory& memory, AmigaConfig config)
    : mem_(memory), config_(config), custom_(0x1000, 0), cia_(0x10000, 0) {}

void Amiga500::install(Cpu& cpu) {
    cpu_ = &cpu;
    cpu.readOverride = [this](Address a, Size s, Long& v) { return read(a, s, v); };
    cpu.writeOverride = [this](Address a, Size s, Long v) { return write(a, s, v); };
}

void Amiga500::uninstall(Cpu& cpu) {
    cpu.readOverride = nullptr;
    cpu.writeOverride = nullptr;
    cpu_ = nullptr;
}

void Amiga500::loadRom(std::vector<Byte> rom) {
    rom_ = std::move(rom);
    overlay_ = true;
}

void Amiga500::reset(Cpu& cpu) {
    std::fill(custom_.begin(), custom_.end(), Byte{0});
    std::fill(cia_.begin(), cia_.end(), Byte{0});
    overlay_ = true;
    beamH_ = beamV_ = 0;
    lof_ = false;
    Registers& r = cpu.registers();
    r.sr = 0x2700;
    r.inactiveSp = 0;
    if (hasRom()) {
        r.a[7] = (Long{readByte(0)} << 24) | (Long{readByte(1)} << 16) | (Long{readByte(2)} << 8) | readByte(3);
        r.pc = ((Long{readByte(4)} << 24) | (Long{readByte(5)} << 16) | (Long{readByte(6)} << 8) | readByte(7)) & kAddressMask;
    }
}

Amiga500::Region Amiga500::classify(Address a) const {
    a &= kAddressMask;
    if (a < config_.chipKb * 1024U) return Region::ChipRam;
    if (config_.fastKb && a >= kFastBase && a < kFastBase + config_.fastKb * 1024U) return Region::FastRam;
    if (a >= kCiaBase && a < kCiaEnd) return Region::Cia;
    if (config_.slowKb && a >= kSlowBase && a < kSlowBase + config_.slowKb * 1024U) return Region::SlowRam;
    if (a >= kCustomBase && a < kCustomEnd) return Region::Custom;
    if (a >= kRomBase && a < kRomEnd) return Region::Rom;
    return Region::Unmapped;
}

Byte Amiga500::readByte(Address a) const {
    a &= kAddressMask;
    if (overlay() && a < kOverlayWindow) return rom_[a % rom_.size()];
    switch (classify(a)) {
        case Region::ChipRam:
        case Region::FastRam:
        case Region::SlowRam: return mem_.read8(a);
        case Region::Cia: return cia_[a - kCiaBase];
        case Region::Custom: return custom_[a - kCustomBase];
        case Region::Rom: return rom_.empty() ? Byte{0} : rom_[(a - kRomBase) % rom_.size()];
        default: return 0;
    }
}

void Amiga500::updateOverlay() {
    const Byte praByte = cia_[0xBFE001 - kCiaBase];
    const Byte ddra = cia_[0xBFE201 - kCiaBase];
    overlay_ = !((ddra & 1U) && !(praByte & 1U));
}

bool Amiga500::writeByte(Address a, Byte v) {
    a &= kAddressMask;
    switch (classify(a)) {
        case Region::ChipRam:
        case Region::FastRam:
        case Region::SlowRam: mem_.write8(a, v); return true;
        case Region::Cia:
            cia_[a - kCiaBase] = v;
            if (a == 0xBFE001 || a == 0xBFE201) updateOverlay();
            return true;
        case Region::Custom: custom_[a - kCustomBase] = v; return true;
        default: return false;
    }
}

void Amiga500::setState(const State& s) {
    overlay_ = s.overlay;
    if (s.custom.size() == custom_.size()) custom_ = s.custom;
    if (s.cia.size() == cia_.size()) cia_ = s.cia;
    beamH_ = s.beamH;
    beamV_ = s.beamV;
    lof_ = s.longFrame;
}

bool Amiga500::read(Address a, Size s, Long& value) {
    a &= kAddressMask;
    const Region first = classify(a);
    const bool plain = (first == Region::ChipRam || first == Region::FastRam || first == Region::SlowRam) &&
                       !(overlay() && a < kOverlayWindow);
    if (plain) return false;  // ordinary memory
    Long v = 0;
    if (first == Region::Custom && a >= kCustomBase + 4 && a < kCustomBase + 8) {   // VPOSR ($DFF004) and VHPOSR ($DFF006): the beam position
        const Word vposr = static_cast<Word>((lof_ ? 0x8000U : 0U) | ((beamV_ >> 8) & 1U));
        const Word vhposr = static_cast<Word>(((beamV_ & 0xFFU) << 8) | (beamH_ & 0xFFU));
        const Long both = (static_cast<Long>(vposr) << 16) | vhposr;
        for (unsigned i = 0; i < static_cast<unsigned>(s); ++i) {
            const Address at = a + i;
            const unsigned off = at - (kCustomBase + 4);
            v = (v << 8) | (off < 4 ? ((both >> (24 - 8 * off)) & 0xFFU) : readByte(at));
        }
        if (a <= kCustomBase + 6 && a + static_cast<unsigned>(s) > kCustomBase + 6) advanceBeam();   // reading VHPOSR moves the beam on
        value = v;
        return true;
    }
    for (unsigned i = 0; i < static_cast<unsigned>(s); ++i) v = (v << 8) | readByte(a + i);
    value = v;
    return true;
}

void Amiga500::advanceBeam() {
    beamH_ += 32;
    if (beamH_ >= 228) {
        beamH_ -= 228;
        if (++beamV_ >= 312) {
            beamV_ = 0;
            lof_ = !lof_;
        }
    }
}

bool Amiga500::write(Address a, Size s, Long value) {
    a &= kAddressMask;
    const Region first = classify(a);
    if (first == Region::ChipRam || first == Region::FastRam || first == Region::SlowRam) return false;
    DeviceWrite w;
    w.kind = first == Region::Custom ? DeviceWrite::Kind::Custom : first == Region::Cia ? DeviceWrite::Kind::Cia
             : first == Region::Rom ? DeviceWrite::Kind::Rom : DeviceWrite::Kind::Unmapped;
    w.address = a;
    w.size = s;
    w.value = value;
    w.pc = cpu_ ? cpu_->instructionAddress() : 0;
    for (unsigned i = 0; i < static_cast<unsigned>(s); ++i)
        writeByte(a + i, static_cast<Byte>(value >> (8 * (static_cast<unsigned>(s) - 1 - i))));
    log_.push_back(w);
    while (log_.size() > maxLog_) log_.pop_front();
    return true;
}

std::string Amiga500::customRegisterName(Address a) {
    a &= kAddressMask;
    if (a < kCustomBase || a >= kCustomEnd) return "";
    const unsigned off = (a - kCustomBase) & ~1U;
    struct Reg {
        unsigned off;
        const char* name;
    };
    static const Reg regs[] = {
        {0x002, "DMACONR"}, {0x004, "VPOSR"},  {0x006, "VHPOSR"}, {0x01C, "INTENAR"}, {0x01E, "INTREQR"},
        {0x058, "BLTSIZE"}, {0x080, "COP1LCH"}, {0x082, "COP1LCL"}, {0x088, "COPJMP1"}, {0x08E, "DIWSTRT"},
        {0x090, "DIWSTOP"}, {0x092, "DDFSTRT"}, {0x094, "DDFSTOP"}, {0x096, "DMACON"},  {0x09A, "INTENA"},
        {0x09C, "INTREQ"},  {0x09E, "ADKCON"},  {0x100, "BPLCON0"}, {0x102, "BPLCON1"}, {0x104, "BPLCON2"},
    };
    for (const Reg& r : regs)
        if (r.off == off) return r.name;
    if (off >= 0x180 && off < 0x1C0) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "COLOR%02u", (off - 0x180) / 2);
        return buf;
    }
    return "";
}

}  // namespace m68k
