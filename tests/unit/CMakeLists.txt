# UNIT TESTS

include_directories(
  ${OPENJPEG_BINARY_DIR}/src/lib/openjp2 # opj_config.h
  ${OPENJPEG_SOURCE_DIR}/src/lib/openjp2
)

set(unit_test
  testempty0
  testempty1
  testempty2
)
foreach(ut ${unit_test})
  add_executable(${ut} ${ut}.c)
  target_link_libraries(${ut} openjp2)
  add_test(NAME ${ut} COMMAND ${ut})
endforeach()

add_executable(testjp2 testjp2.c)
target_link_libraries(testjp2 openjp2)
add_test(NAME testjp2 COMMAND testjp2 ${OPJ_DATA_ROOT})

# Diff-test driver: decodes a file with OPJ_T1_FAST=0 and =1 and asserts
# byte-identical output.  Not registered via add_test() because it takes a
# file argument; invoked by the Task-3 shell wrapper instead.
add_executable(test_mqc_diff test_mqc_diff.c)
target_link_libraries(test_mqc_diff openjp2)

# Dump helper: decodes one file and writes a deterministic binary dump of
# pixel data to stdout.  Used by scripts/run_diff_test.sh to compare
# legacy vs fast paths in separate processes (one per OPJ_T1_FAST value).
# Not registered via add_test() — invoked by the shell wrapper.
add_executable(test_mqc_dump test_mqc_dump.c)
target_link_libraries(test_mqc_dump openjp2)
