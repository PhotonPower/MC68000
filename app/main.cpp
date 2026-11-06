// m68ktool - command line front end for the MC68000 tools.
//
//   m68ktool asm <source.s> [options]     assembler
//   m68ktool dis <image>    [options]     disassembler
//   m68ktool help | version
//
// The monitor and the emulator server are planned (docs/roadmap.md).

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "m68k/assembler.hpp"
#include "m68k/disassembler.hpp"
#include "m68k/expression.hpp"
#include "m68k/format.hpp"
#include "m68k/image_io.hpp"
#include "m68k/version.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;  // assembly errors in the source
constexpr int kExitUsage = 2;   // bad command line / I/O problems

struct UsageError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void printUsage(std::ostream& out) {
    out << "m68ktool " << m68k::kVersion << " - Motorola MC68000 tools\n"
        << "\n"
        << "Usage:\n"
        << "  m68ktool asm <source.s> [options]\n"
        << "  m68ktool dis <image>    [options]\n"
        << "  m68ktool help | version\n"
        << "\n"
        << "asm options:\n"
        << "  -o <file>        write the assembled code as a raw binary\n"
        << "  --srec <file>    write Motorola S-records\n"
        << "  -l <file|->      write a listing ('-' = standard output)\n"
        << "  -s <file|->      write the symbol table\n"
        << "  --org <addr>     start address before the first ORG (default 0)\n"
        << "  (without -o/--srec the result is written to <source>.bin)\n"
        << "\n"
        << "dis options:\n"
        << "  --org <addr>     load address of a raw binary (default 0); S-record files carry their own\n"
        << "  --start <addr>   first address to disassemble (default: start of the image)\n"
        << "  --end <addr>     address after the last byte (default: end of the image)\n"
        << "  -e <addr>        additional entry point (repeatable); the default entry is the start\n"
        << "  -c <file>        comment file (address,\"text\"); commented addresses are entry points\n"
        << "  --linear         decode sequentially instead of following the program flow\n"
        << "  --asm            write re-assemblable source instead of a listing\n"
        << "  --no-bytes       listing without the instruction words\n"
        << "  --style <s>      number style: dollar (default), c, h\n"
        << "  -o <file>        write to a file instead of standard output\n"
        << "\n"
        << "Numbers: 123, $1F, 0x1F, %1010. Exit codes: 0 ok, 1 errors in the source, 2 usage/I-O error.\n";
}

m68k::Address parseAddress(const std::string& text, const std::string& what) {
    const m68k::ExpressionEvaluator ev(nullptr, 0);
    const m68k::ExprResult r = ev.evaluate(text);
    if (!r.ok || r.undefined || r.value < 0 || r.value > static_cast<std::int64_t>(m68k::kAddressMask)) {
        throw UsageError("bad " + what + " '" + text + "'");
    }
    return static_cast<m68k::Address>(r.value);
}

class Args {
public:
    Args(int argc, char** argv) {
        for (int i = 2; i < argc; ++i) items_.push_back(argv[i]);
    }
    bool done() const { return pos_ >= items_.size(); }
    std::string next() { return items_[pos_++]; }
    std::string value(const std::string& option) {
        if (done()) throw UsageError("option " + option + " needs a value");
        return next();
    }

private:
    std::vector<std::string> items_;
    std::size_t pos_ = 0;
};

void writeOut(const std::string& target, const std::string& text) {
    if (target == "-") std::cout << text;
    else m68k::writeTextFile(target, text);
}

int cmdAsm(Args args) {
    std::string source, outBin, outSrec, outList, outSyms;
    m68k::AssemblerOptions opt;
    while (!args.done()) {
        const std::string a = args.next();
        if (a == "-o") outBin = args.value(a);
        else if (a == "--srec") outSrec = args.value(a);
        else if (a == "-l") outList = args.value(a);
        else if (a == "-s") outSyms = args.value(a);
        else if (a == "--org") opt.origin = parseAddress(args.value(a), "address");
        else if (!a.empty() && a[0] == '-' && a != "-") throw UsageError("unknown option " + a);
        else if (source.empty()) source = a;
        else throw UsageError("more than one source file");
    }
    if (source.empty()) throw UsageError("asm needs a source file");

    const m68k::AssemblyResult result = m68k::assemble(m68k::readTextFile(source), opt);
    for (const m68k::Diagnostic& d : result.errors) {
        std::cerr << source << ":" << d.line << ": error: " << d.message << "\n";
    }
    if (!outList.empty()) writeOut(outList, result.formatListing());
    if (!outSyms.empty()) writeOut(outSyms, result.formatSymbols());
    if (!result.ok()) {
        std::cerr << source << ": " << result.errors.size() << " error(s), no output written\n";
        return kExitFailed;
    }
    if (outBin.empty() && outSrec.empty()) outBin = source + ".bin";
    std::size_t total = 0;
    for (const m68k::Segment& s : result.segments) total += s.bytes.size();
    if (!outBin.empty()) m68k::writeBinaryFile(outBin, result.flatten());
    if (!outSrec.empty()) m68k::writeTextFile(outSrec, m68k::formatSRecords(result.segments, result.lowest()));
    std::cerr << source << ": " << total << " bytes";
    if (total) {
        std::cerr << ", " << m68k::formatHex(result.lowest(), 6, m68k::NumberStyle::Dollar);
    }
    std::cerr << ", 0 error(s)\n";
    return kExitOk;
}

int cmdDis(Args args) {
    std::string image, commentFile, outFile;
    m68k::DisassemblerOptions dopt;
    bool linear = false, haveOrg = false, haveStart = false, haveEnd = false;
    m68k::Address org = 0, start = 0, end = 0;
    std::vector<m68k::Address> entries;
    while (!args.done()) {
        const std::string a = args.next();
        if (a == "--org") org = parseAddress(args.value(a), "address"), haveOrg = true;
        else if (a == "--start") start = parseAddress(args.value(a), "address"), haveStart = true;
        else if (a == "--end") end = parseAddress(args.value(a), "address"), haveEnd = true;
        else if (a == "-e") entries.push_back(parseAddress(args.value(a), "address"));
        else if (a == "-c") commentFile = args.value(a);
        else if (a == "-o") outFile = args.value(a);
        else if (a == "--linear") linear = true;
        else if (a == "--asm") dopt.asmSource = true;
        else if (a == "--no-bytes") dopt.showBytes = false;
        else if (a == "--style") {
            const std::string s = args.value(a);
            if (!m68k::parseNumberStyle(s, dopt.style)) throw UsageError("unknown number style '" + s + "'");
        } else if (!a.empty() && a[0] == '-') throw UsageError("unknown option " + a);
        else if (image.empty()) image = a;
        else throw UsageError("more than one image file");
    }
    if (image.empty()) throw UsageError("dis needs an image file");
    (void)haveOrg;

    m68k::Memory memory;
    m68k::Address lo = 0, hi = 0;
    std::optional<m68k::Address> imageEntry;
    if (m68k::looksLikeSRecordFile(image)) {
        const m68k::Image img = m68k::parseSRecords(m68k::readTextFile(image));
        m68k::loadSegments(memory, img.segments);
        imageEntry = img.entry;
        bool first = true;
        for (const m68k::Segment& s : img.segments) {
            if (s.bytes.empty()) continue;
            const m68k::Address e = s.address + static_cast<m68k::Address>(s.bytes.size());
            lo = first ? s.address : std::min(lo, s.address);
            hi = first ? e : std::max(hi, e);
            first = false;
        }
    } else {
        const std::vector<m68k::Byte> bytes = m68k::readBinaryFile(image);
        memory.load(org, bytes);
        lo = org;
        hi = org + static_cast<m68k::Address>(bytes.size());
    }
    if (!haveStart) start = lo;
    if (!haveEnd) end = hi;

    m68k::Disassembler dis(memory, start, end);
    for (const m68k::Address e : entries) dis.addEntry(e);
    bool commented = false;
    if (!commentFile.empty()) {
        for (const m68k::Diagnostic& d : m68k::loadCommentFile(m68k::readTextFile(commentFile), dis)) {
            std::cerr << commentFile << ":" << d.line << ": " << d.message << "\n";
        }
        commented = true;
    }
    if (linear) {
        dis.analyzeLinear();
    } else {
        if (entries.empty() && !commented) dis.addEntry(imageEntry && *imageEntry >= start && *imageEntry < end ? *imageEntry : start);
        else if (entries.empty()) dis.addEntry(start);
        dis.analyzeFlow();
    }
    const std::string text = dis.render(dopt);
    if (outFile.empty()) std::cout << text;
    else m68k::writeTextFile(outFile, text);
    return kExitOk;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(std::cerr);
        return kExitUsage;
    }
    const std::string command = argv[1];
    try {
        if (command == "help" || command == "--help" || command == "-h") {
            printUsage(std::cout);
            return kExitOk;
        }
        if (command == "version" || command == "--version") {
            std::cout << "m68ktool " << m68k::kVersion << "\n";
            return kExitOk;
        }
        if (command == "asm") return cmdAsm(Args(argc, argv));
        if (command == "dis") return cmdDis(Args(argc, argv));
        if (command == "monitor" || command == "serve") {
            std::cerr << "m68ktool: '" << command << "' is not implemented yet (see docs/roadmap.md)\n";
            return kExitUsage;
        }
        std::cerr << "m68ktool: unknown command '" << command << "'\n\n";
        printUsage(std::cerr);
        return kExitUsage;
    } catch (const UsageError& e) {
        std::cerr << "m68ktool: " << e.what() << "\n";
        return kExitUsage;
    } catch (const m68k::IoError& e) {
        std::cerr << "m68ktool: " << e.what() << "\n";
        return kExitUsage;
    }
}
