# Each suite is one executable without an external test framework (see test_util.hpp).
foreach(name format memory expression decoder assembler roundtrip image_io hunk disassembler cpu session)
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
