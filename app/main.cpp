// m68ktool - command line front end for the MC68000 tools.
//
// Version 0.1 is the project skeleton: only `help` and `version` do something. The planned
// sub-commands (asm, dis, monitor, serve) are listed in docs/roadmap.md.

#include <iostream>
#include <string>

#include "m68k/version.hpp"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;  // bad command line / I/O problems

void printUsage(std::ostream& out) {
    out << "m68ktool " << m68k::kVersion << " - Motorola MC68000 tools\n"
        << "\n"
        << "Usage:\n"
        << "  m68ktool help | version\n"
        << "\n"
        << "Planned (not implemented yet, see docs/roadmap.md):\n"
        << "  m68ktool asm <source.s> [options]\n"
        << "  m68ktool dis <image>    [options]\n"
        << "  m68ktool monitor [image]\n"
        << "  m68ktool serve\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(std::cerr);
        return kExitUsage;
    }
    const std::string command = argv[1];
    if (command == "help" || command == "--help" || command == "-h") {
        printUsage(std::cout);
        return kExitOk;
    }
    if (command == "version" || command == "--version") {
        std::cout << "m68ktool " << m68k::kVersion << "\n";
        return kExitOk;
    }
    if (command == "asm" || command == "dis" || command == "monitor" || command == "serve") {
        std::cerr << "m68ktool: '" << command << "' is not implemented yet (see docs/roadmap.md)\n";
        return kExitUsage;
    }
    std::cerr << "m68ktool: unknown command '" << command << "'\n\n";
    printUsage(std::cerr);
    return kExitUsage;
}
