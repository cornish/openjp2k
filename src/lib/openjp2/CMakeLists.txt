include_regular_expression("^.*$")

#
install( FILES  ${CMAKE_CURRENT_BINARY_DIR}/opj_config.h
 DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${OPENJPEG_INSTALL_SUBDIR} COMPONENT Headers)

include_directories(
  ${${OPENJPEG_NAMESPACE}_BINARY_DIR}/src/lib/openjp2 # opj_config.h and opj_config_private.h
)
# Defines the source code for the library
set(OPENJPEG_SRCS
  ${CMAKE_CURRENT_SOURCE_DIR}/thread.c
  ${CMAKE_CURRENT_SOURCE_DIR}/thread.h
  ${CMAKE_CURRENT_SOURCE_DIR}/bio.c
  ${CMAKE_CURRENT_SOURCE_DIR}/bio.h
  ${CMAKE_CURRENT_SOURCE_DIR}/cio.c
  ${CMAKE_CURRENT_SOURCE_DIR}/cio.h
  ${CMAKE_CURRENT_SOURCE_DIR}/dwt.c
  ${CMAKE_CURRENT_SOURCE_DIR}/dwt.h
  ${CMAKE_CURRENT_SOURCE_DIR}/event.c
  ${CMAKE_CURRENT_SOURCE_DIR}/event.h
  ${CMAKE_CURRENT_SOURCE_DIR}/ht_dec.c
  ${CMAKE_CURRENT_SOURCE_DIR}/image.c
  ${CMAKE_CURRENT_SOURCE_DIR}/image.h
  ${CMAKE_CURRENT_SOURCE_DIR}/invert.c
  ${CMAKE_CURRENT_SOURCE_DIR}/invert.h
  ${CMAKE_CURRENT_SOURCE_DIR}/j2k.c
  ${CMAKE_CURRENT_SOURCE_DIR}/j2k.h
  ${CMAKE_CURRENT_SOURCE_DIR}/jp2.c
  ${CMAKE_CURRENT_SOURCE_DIR}/jp2.h
  ${CMAKE_CURRENT_SOURCE_DIR}/mct.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mct.h
  ${CMAKE_CURRENT_SOURCE_DIR}/mqc.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mqc.h
  ${CMAKE_CURRENT_SOURCE_DIR}/mqc_inl.h
  ${CMAKE_CURRENT_SOURCE_DIR}/openjpeg.c
  ${CMAKE_CURRENT_SOURCE_DIR}/openjpeg.h
  ${CMAKE_CURRENT_SOURCE_DIR}/opj_clock.c
  ${CMAKE_CURRENT_SOURCE_DIR}/opj_clock.h
  ${CMAKE_CURRENT_SOURCE_DIR}/pi.c
  ${CMAKE_CURRENT_SOURCE_DIR}/pi.h
  ${CMAKE_CURRENT_SOURCE_DIR}/t1.c
  ${CMAKE_CURRENT_SOURCE_DIR}/t1.h
  ${CMAKE_CURRENT_SOURCE_DIR}/t1_inl.h
  ${CMAKE_CURRENT_SOURCE_DIR}/t1_fast.c
  ${CMAKE_CURRENT_SOURCE_DIR}/t1_fast.h
  ${CMAKE_CURRENT_SOURCE_DIR}/mqc_fast.c
  ${CMAKE_CURRENT_SOURCE_DIR}/mqc_fast.h
  ${CMAKE_CURRENT_SOURCE_DIR}/t2.c
  ${CMAKE_CURRENT_SOURCE_DIR}/t2.h
  ${CMAKE_CURRENT_SOURCE_DIR}/tcd.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tcd.h
  ${CMAKE_CURRENT_SOURCE_DIR}/tgt.c
  ${CMAKE_CURRENT_SOURCE_DIR}/tgt.h
  ${CMAKE_CURRENT_SOURCE_DIR}/function_list.c
  ${CMAKE_CURRENT_SOURCE_DIR}/function_list.h
  ${CMAKE_CURRENT_SOURCE_DIR}/opj_codec.h
  ${CMAKE_CURRENT_SOURCE_DIR}/opj_includes.h
  ${CMAKE_CURRENT_SOURCE_DIR}/opj_intmath.h
  ${CMAKE_CURRENT_SOURCE_DIR}/opj_malloc.c
  ${CMAKE_CURRENT_SOURCE_DIR}/opj_malloc.h
  ${CMAKE_CURRENT_SOURCE_DIR}/sparse_array.c
  ${CMAKE_CURRENT_SOURCE_DIR}/sparse_array.h
)
if(BUILD_JPIP)
  add_definitions(-DUSE_JPIP)
  set(OPENJPEG_SRCS
    ${OPENJPEG_SRCS}
    ${CMAKE_CURRENT_SOURCE_DIR}/cidx_manager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cidx_manager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/phix_manager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ppix_manager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/thix_manager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/tpix_manager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/indexbox_manager.h
  )
endif()

option(OPJ_DISABLE_TPSOT_FIX "Disable TPsot==TNsot fix. See https://github.com/uclouvain/openjpeg/issues/254." OFF)
if(OPJ_DISABLE_TPSOT_FIX)
  add_definitions(-DOPJ_DISABLE_TPSOT_FIX)
endif()

option(OPJ_T1_LEGACY_ONLY "Force the legacy T1 decoder path (disable t1_fast variants). Used by CI diff-tests and as a correctness fallback." OFF)
if(OPJ_T1_LEGACY_ONLY)
  add_definitions(-DOPJ_T1_LEGACY_ONLY)
endif()

# SP-2.1a: opt-in Taskflow-based DAG decoder orchestration.  Default ON.
# When ON, task_graph.cpp is compiled as C++17 and the produced library
# links libstdc++.  When OFF, the library remains pure C with no C++
# runtime dependency.  The new module is gated by the OPJ_ENABLE_TASK_GRAPH
# preprocessor define and is opaque at the C ABI boundary.
option(OPJ_ENABLE_TASK_GRAPH
       "Build openjp2 with the Taskflow-based DAG decoder orchestration (requires C++17 + libstdc++)"
       ON)
if(OPJ_ENABLE_TASK_GRAPH)
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(OPENJPEG_SRCS
    ${OPENJPEG_SRCS}
    ${CMAKE_CURRENT_SOURCE_DIR}/task_graph.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/task_graph.h
  )
  set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/task_graph.cpp
    PROPERTIES LANGUAGE CXX
               # Suppress warnings on this file:
               # 1. C-only flags (-Wdeclaration-after-statement, -Wstrict-prototypes,
               #    -Wmissing-prototypes) from OPENJP2_COMPILE_OPTIONS produce cc1plus
               #    notices when applied to a C++ source.
               # 2. Taskflow is vendored third-party and trips -Wconversion warnings
               #    in observer.hpp / etc.
               # The wrapper itself is small (~90 LOC) and reviewed manually.
               COMPILE_FLAGS "-w")
  add_definitions(-DOPJ_ENABLE_TASK_GRAPH)
  include_directories(${OPENJPEG_SOURCE_DIR}/third_party)
endif()

# Special case for old i586-mingw32msvc-gcc cross compiler
if(NOT WIN32 AND CMAKE_COMPILER_IS_GNUCC AND CMAKE_C_COMPILER MATCHES ".*mingw32msvc.*" )
  set(WIN32 YES)
endif()

# Build the library
add_library(${OPENJPEG_LIBRARY_NAME} ${OPENJPEG_SRCS})
set(INSTALL_LIBS ${OPENJPEG_LIBRARY_NAME})
if(WIN32)
  if(BUILD_SHARED_LIBS)
    target_compile_definitions(${OPENJPEG_LIBRARY_NAME} PRIVATE OPJ_EXPORTS)
  else()
    target_compile_definitions(${OPENJPEG_LIBRARY_NAME} PUBLIC OPJ_STATIC)
  endif()
elseif(BUILD_SHARED_LIBS AND BUILD_STATIC_LIBS)
  # Builds both static and dynamic libs
  add_library(openjp2_static STATIC ${OPENJPEG_SRCS})
  set_target_properties(openjp2_static PROPERTIES OUTPUT_NAME ${OPENJPEG_LIBRARY_NAME})
  list(APPEND INSTALL_LIBS openjp2_static)
  target_include_directories(openjp2_static PUBLIC $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/${OPENJPEG_INSTALL_SUBDIR}>)
endif()

target_include_directories(${OPENJPEG_LIBRARY_NAME} PUBLIC $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/${OPENJPEG_INSTALL_SUBDIR}>)
if(UNIX)
  target_link_libraries(${OPENJPEG_LIBRARY_NAME} m)
endif()
set_target_properties(${OPENJPEG_LIBRARY_NAME} PROPERTIES ${OPENJPEG_LIBRARY_PROPERTIES})
target_compile_options(${OPENJPEG_LIBRARY_NAME} PRIVATE ${OPENJP2_COMPILE_OPTIONS})

# Install library
install(TARGETS ${INSTALL_LIBS}
  EXPORT OpenJPEGTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT Applications
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Libraries
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Libraries
)

# Install includes files
install(FILES openjpeg.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/${OPENJPEG_INSTALL_SUBDIR} COMPONENT Headers
)

if(BUILD_DOC)
# install man page of the library
install(
  FILES       ${OPENJPEG_SOURCE_DIR}/doc/man/man3/libopenjp2.3
  DESTINATION ${CMAKE_INSTALL_MANDIR}/man3)
endif()

if(BUILD_LUTS_GENERATOR)
# internal utility to generate t1_luts.h and t1_ht_luts.h (part of the jp2 lib)
# no need to install:
  add_executable(t1_generate_luts t1_generate_luts.c t1_ht_generate_luts.c)
  if(UNIX)
    target_link_libraries(t1_generate_luts m)
  endif()
endif()

# Experimental option; let's how cppcheck performs
# Implementation details:
# I could not figure out how to easily upload a file to CDash. Instead simply
# pretend cppcheck is part of the Build step. Technically cppcheck can even
# output gcc formatted error/warning report
# Another implementation detail: I could not redirect error to the error
# catching mechanism something is busted in cmake 2.8.5, I had to use the
# warning regex to catch them.
if(OPENJPEG_CPPCHECK)
  find_package(CPPCHECK REQUIRED)
  foreach(f ${OPENJPEG_SRCS})
    # cppcheck complains about too many configuration, pretend to be WIN32:
    add_custom_command(TARGET ${OPENJPEG_LIBRARY_NAME}
      COMMAND ${CPPCHECK_EXECUTABLE} -DWIN32 ${f})
  endforeach()
endif()

if(OPJ_USE_DSYMUTIL)
  if(BUILD_SHARED_LIBS)
    add_custom_command(TARGET ${OPENJPEG_LIBRARY_NAME} POST_BUILD
    COMMAND "dsymutil" "$<TARGET_SONAME_FILE:${OPENJPEG_LIBRARY_NAME}>"
    COMMENT "dsymutil $<TARGET_SONAME_FILE:${OPENJPEG_LIBRARY_NAME}>"
    DEPENDS ${OPENJPEG_LIBRARY_NAME})
  endif()
endif()

#################################################################################
# threading configuration
#################################################################################
set(CMAKE_THREAD_PREFER_PTHREAD TRUE)

option(OPJ_USE_THREAD "Build with thread/mutex support " ON)
if(NOT OPJ_USE_THREAD)
   add_definitions( -DMUTEX_stub)
endif(NOT OPJ_USE_THREAD)

find_package(Threads QUIET)

if(OPJ_USE_THREAD AND WIN32 AND NOT Threads_FOUND )
    add_definitions( -DMUTEX_win32)
    set(Threads_FOUND YES)
endif()

if(OPJ_USE_THREAD AND Threads_FOUND AND CMAKE_USE_WIN32_THREADS_INIT )
   add_definitions( -DMUTEX_win32)
endif(OPJ_USE_THREAD AND Threads_FOUND AND CMAKE_USE_WIN32_THREADS_INIT )

if(OPJ_USE_THREAD AND Threads_FOUND AND CMAKE_USE_PTHREADS_INIT )
   add_definitions( -DMUTEX_pthread)
endif(OPJ_USE_THREAD AND Threads_FOUND AND CMAKE_USE_PTHREADS_INIT )

if(OPJ_USE_THREAD AND NOT Threads_FOUND)
  message(FATAL_ERROR "No thread library found and thread/mutex support is required by OPJ_USE_THREAD option")
endif(OPJ_USE_THREAD AND NOT Threads_FOUND)

if(OPJ_USE_THREAD AND Threads_FOUND AND CMAKE_USE_PTHREADS_INIT)
   TARGET_LINK_LIBRARIES(${OPENJPEG_LIBRARY_NAME} ${CMAKE_THREAD_LIBS_INIT})
endif(OPJ_USE_THREAD AND Threads_FOUND AND CMAKE_USE_PTHREADS_INIT)

if(BUILD_UNIT_TESTS AND UNIX)
    add_executable(bench_dwt bench_dwt.c)
    if(UNIX)
        target_link_libraries(bench_dwt m ${OPENJPEG_LIBRARY_NAME})
    endif()
    if(OPJ_USE_THREAD AND Threads_FOUND AND CMAKE_USE_PTHREADS_INIT)
        target_link_libraries(bench_dwt ${CMAKE_THREAD_LIBS_INIT})
    endif(OPJ_USE_THREAD AND Threads_FOUND AND CMAKE_USE_PTHREADS_INIT)

    add_executable(test_sparse_array test_sparse_array.c)
    if(UNIX)
        target_link_libraries(test_sparse_array m ${OPENJPEG_LIBRARY_NAME})
    endif()
    if(OPJ_USE_THREAD AND Threads_FOUND AND CMAKE_USE_PTHREADS_INIT)
        target_link_libraries(test_sparse_array ${CMAKE_THREAD_LIBS_INIT})
    endif(OPJ_USE_THREAD AND Threads_FOUND AND CMAKE_USE_PTHREADS_INIT)
endif(BUILD_UNIT_TESTS AND UNIX)
