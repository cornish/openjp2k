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

# Structural equivalence test for the packed MQ state table.
# Verifies index ranges, switch_flag correctness, and the J2K Annex C
# even/odd mirror invariant against opj_mqc_states_packed[].
add_executable(test_mqc_packed_equivalence test_mqc_packed_equivalence.c)
target_link_libraries(test_mqc_packed_equivalence openjp2)
target_include_directories(test_mqc_packed_equivalence PRIVATE
    ${CMAKE_SOURCE_DIR}/src/lib/openjp2)
add_test(NAME mqc_packed_equivalence COMMAND test_mqc_packed_equivalence)

# Tiny diff-test subset: 10 files, runs in <30s. Larger sweeps are
# invoked via scripts/run_diff_test.sh outside ctest.
# Bash-only: skip on Windows (the wrapper is a POSIX shell script).
if(NOT WIN32)
    set(MQC_DIFF_FAST_CORPUS
        "${OPJ_DATA_ROOT}/input/conformance/p1_07.j2k"
        "${OPJ_DATA_ROOT}/input/conformance/p1_01.j2k"
        "${OPJ_DATA_ROOT}/input/conformance/p0_01.j2k"
        "${OPJ_DATA_ROOT}/input/conformance/p0_05.j2k"
        "${OPJ_DATA_ROOT}/input/conformance/p0_09.j2k"
        "${OPJ_DATA_ROOT}/input/conformance/p0_13.j2k"
        "${OPJ_DATA_ROOT}/input/conformance/p1_03.j2k"
        "${OPJ_DATA_ROOT}/input/nonregression/Bretagne2.j2k"
        "${OPJ_DATA_ROOT}/input/nonregression/buxI.j2k"
        "${OPJ_DATA_ROOT}/input/nonregression/buxR.j2k"
    )
    add_test(NAME mqc_diff_fast
             COMMAND ${CMAKE_SOURCE_DIR}/scripts/run_diff_test.sh ${MQC_DIFF_FAST_CORPUS}
             WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    set_tests_properties(mqc_diff_fast PROPERTIES
        LABELS "diff-test;fast"
        SKIP_RETURN_CODE 77)
    # Skip if OPJ_DATA_ROOT not set.
    if(NOT OPJ_DATA_ROOT)
        set_tests_properties(mqc_diff_fast PROPERTIES
            DISABLED TRUE)
    endif()
endif()
