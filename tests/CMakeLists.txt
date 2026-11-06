# The orchestrator tests need Python 3 (standard library only) and the m68ktool binary.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_test(NAME agent
           COMMAND ${CMAKE_COMMAND} -E env "M68KTOOL=$<TARGET_FILE:m68ktool>" ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/agent)
endif()

# Each suite is one executable without an external test framework (see test_util.hpp).
foreach(name format memory expression decoder assembler roundtrip image_io hunk hunk_write disassembler cpu session reverse monitor os)
  add_executable(test_${name} test_${name}.cpp)
  target_link_libraries(test_${name} PRIVATE m68kcore)
  m68k_set_warnings(test_${name})
  add_test(NAME ${name} COMMAND test_${name})
endforeach()

# Smoke test of the command line tool.
add_test(NAME cli_version COMMAND m68ktool version)
set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "m68ktool [0-9]+\\.[0-9]+\\.[0-9]+")

# The example program assembles, and what the disassembler writes assembles again to the same bytes.
add_test(NAME cli_asm_example
         COMMAND m68ktool asm ${CMAKE_CURRENT_SOURCE_DIR}/../examples/hello.s -o ${CMAKE_CURRENT_BINARY_DIR}/hello.bin)
add_test(NAME cli_dis_example
         COMMAND m68ktool dis ${CMAKE_CURRENT_BINARY_DIR}/hello.bin --org 0x1000 --asm
                 -o ${CMAKE_CURRENT_BINARY_DIR}/hello_re.s)
set_tests_properties(cli_dis_example PROPERTIES DEPENDS cli_asm_example)
add_test(NAME cli_reasm_example
         COMMAND m68ktool asm ${CMAKE_CURRENT_BINARY_DIR}/hello_re.s -o ${CMAKE_CURRENT_BINARY_DIR}/hello_re.bin)
set_tests_properties(cli_reasm_example PROPERTIES DEPENDS cli_dis_example)
add_test(NAME cli_example_identical
         COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_CURRENT_BINARY_DIR}/hello.bin ${CMAKE_CURRENT_BINARY_DIR}/hello_re.bin)
set_tests_properties(cli_example_identical PROPERTIES DEPENDS cli_reasm_example)

# A program with sections becomes a hunk file, which the disassembler reads again (relocated, with its symbols).
add_test(NAME cli_hunk_example
         COMMAND m68ktool asm ${CMAKE_CURRENT_SOURCE_DIR}/../examples/sections.s --hunk ${CMAKE_CURRENT_BINARY_DIR}/sections.hunk --hunk-symbols)
add_test(NAME cli_hunk_dis
         COMMAND m68ktool dis ${CMAKE_CURRENT_BINARY_DIR}/sections.hunk --org 0x20000)
set_tests_properties(cli_hunk_dis PROPERTIES DEPENDS cli_hunk_example PASS_REGULAR_EXPRESSION "MOVEA.L +#.00020020,A0")
# ... and a section program refuses raw output
add_test(NAME cli_hunk_refuses_raw
         COMMAND m68ktool asm ${CMAKE_CURRENT_SOURCE_DIR}/../examples/sections.s -o ${CMAKE_CURRENT_BINARY_DIR}/nope.bin)
set_tests_properties(cli_hunk_refuses_raw PROPERTIES WILL_FAIL TRUE)
