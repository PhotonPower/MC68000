cmake_minimum_required(VERSION 3.16)

project(M68kTools
        VERSION 0.5.0
        DESCRIPTION "Motorola MC68000 assembler, disassembler, emulator and machine code monitor"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(M68K_BUILD_TESTS "Build the unit tests" ON)
option(M68K_BUILD_TOOLS "Build developer tools (cross-check helpers)" OFF)
option(M68K_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(M68K_SANITIZE "Build with AddressSanitizer and UBSan (GCC/Clang)" OFF)

function(m68k_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
    if(M68K_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
    if(M68K_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
    if(M68K_SANITIZE)
      target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
endfunction()

# ---- core library ---------------------------------------------------------------------
# New components (assembler, decoder, cpu, ...) are added to this list as they are written.
add_library(m68kcore STATIC
  src/amiga.cpp
  src/amiga_os.cpp
  src/analysis.cpp
  src/annotations.cpp
  src/assembler.cpp
  src/cpu.cpp
  src/decoder.cpp
  src/disassembler.cpp
  src/encoder.cpp
  src/expression.cpp
  src/forms.cpp
  src/hunk.cpp
  src/adf.cpp
  src/format.cpp
  src/image_io.cpp
  src/listing.cpp
  src/monitor.cpp
  src/session.cpp
  src/session_analysis.cpp
  src/snapshot.cpp
  src/json.cpp)
target_include_directories(m68kcore PUBLIC include)
target_compile_features(m68kcore PUBLIC cxx_std_20)
m68k_set_warnings(m68kcore)

# ---- command line tool ----------------------------------------------------------------
add_executable(m68ktool app/main.cpp)
target_link_libraries(m68ktool PRIVATE m68kcore)
m68k_set_warnings(m68ktool)

install(TARGETS m68ktool RUNTIME DESTINATION bin)

# ---- developer tools ------------------------------------------------------------------
# Cross-check helpers (see docs/testing.md).
if(M68K_BUILD_TOOLS)
  add_executable(m68k_dump_forms tools/dump_forms.cpp)
  target_link_libraries(m68k_dump_forms PRIVATE m68kcore)
  m68k_set_warnings(m68k_dump_forms)
endif()

# ---- tests ----------------------------------------------------------------------------
if(M68K_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
