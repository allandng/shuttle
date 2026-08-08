cmake_minimum_required(VERSION 3.25)
project(shuttle VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Consumers that pull shuttle in via add_subdirectory()/FetchContent want the
# two libraries and nothing else: no test binaries, no bench, no tools, and no
# add_test() entries polluting their ctest run. Both default to ON only when
# shuttle is the top-level project.
option(SHUTTLE_BUILD_TESTS "Build shuttle tests, bench, and tools" ${PROJECT_IS_TOP_LEVEL})
option(SHUTTLE_INSTALL "Generate shuttle install and packaging rules" ${PROJECT_IS_TOP_LEVEL})

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# Sanitizer config: ASan and TSan are mutually exclusive (cannot link both),
# hence separate build trees selected via this cache variable. The flags
# live on an INTERFACE target so individual targets can opt out.
set(SHUTTLE_SAN "off" CACHE STRING "Sanitizer config: off | asan | tsan")

add_library(shuttle_san INTERFACE)
if(SHUTTLE_SAN STREQUAL "asan")
  target_compile_options(shuttle_san INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(shuttle_san INTERFACE -fsanitize=address,undefined)
elseif(SHUTTLE_SAN STREQUAL "tsan")
  target_compile_options(shuttle_san INTERFACE -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(shuttle_san INTERFACE -fsanitize=thread)
elseif(NOT SHUTTLE_SAN STREQUAL "off")
  message(FATAL_ERROR "SHUTTLE_SAN must be one of: off, asan, tsan (got '${SHUTTLE_SAN}')")
endif()

add_compile_options(-Wall -Wextra -g)

find_package(Threads REQUIRED)

# C ABI shared library (Phase 6): deliberately UNsanitized — it is dlopen'd
# by foreign runtimes (python3, future Rust test bins) that lack the
# sanitizer runtime. The header-only core logic it embeds is sanitizer-
# verified through every other target in this file. Defined BEFORE the
# link_libraries(shuttle_san) directive below, which sanitizes everything
# that follows.
add_library(shuttle_c SHARED src/shuttle_c.cpp src/shuttle.cpp)
add_library(shuttle::c ALIAS shuttle_c)
target_include_directories(shuttle_c PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_link_libraries(shuttle_c PRIVATE Threads::Threads)
# ABI v1 is frozen (SHUTTLE_ABI_VERSION 1), so the soname stays libshuttle_c.so.1
# across 1.x releases; only the full version tracks PROJECT_VERSION.
set_target_properties(shuttle_c PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    EXPORT_NAME c)  # so find_package() and add_subdirectory() agree on shuttle::c

# The bench and the two -O2 perf tests below stay in this pre-sanitizer region
# (see the shuttle_c note above): they must NOT be instrumented, so they are
# guarded by SHUTTLE_BUILD_TESTS *here* rather than moved down.
if(SHUTTLE_BUILD_TESTS)
  # Benchmark binary (Phase 7): also UNsanitized — instrumentation overhead
  # would invalidate the transport-ratio comparison (NFR-P1). Compiled -O2
  # regardless of the default Debug build type, for the same reason.
  add_executable(shuttle_bench bench/bench_main.cpp src/shuttle.cpp)
  target_include_directories(shuttle_bench PRIVATE include)
  target_compile_options(shuttle_bench PRIVATE -O2)
  target_link_libraries(shuttle_bench PRIVATE Threads::Threads)

  # G7.2 CPU-accounting proof: also unsanitized + -O2 (instrumentation would
  # distort the copy-CPU comparison).
  add_executable(shuttle_nocopy_cpu_test tests/nocopy_cpu_test.cpp src/shuttle.cpp)
  target_include_directories(shuttle_nocopy_cpu_test PRIVATE include)
  target_compile_options(shuttle_nocopy_cpu_test PRIVATE -O2)
  target_link_libraries(shuttle_nocopy_cpu_test PRIVATE Threads::Threads)

  # G7.3 wake-under-load latency: unsanitized + -O2 (latency measurement).
  add_executable(shuttle_wake_under_load_test tests/wake_under_load_test.cpp src/shuttle.cpp)
  target_include_directories(shuttle_wake_under_load_test PRIVATE include)
  target_compile_options(shuttle_wake_under_load_test PRIVATE -O2)
  target_link_libraries(shuttle_wake_under_load_test PRIVATE Threads::Threads)
endif()

link_libraries(shuttle_san)  # every target below is sanitizer-instrumented

# Core library: must stay free of any test/bench dependency.
add_library(shuttle_core STATIC src/shuttle.cpp)
add_library(shuttle::core ALIAS shuttle_core)
target_include_directories(shuttle_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
# The core uses pthreads (robust mutexes, futex-backed parking) directly, so the
# exported static target carries its own Threads dependency: linking shuttle::core
# alone must be enough to get -pthread.
target_link_libraries(shuttle_core PUBLIC Threads::Threads)
set_target_properties(shuttle_core PROPERTIES EXPORT_NAME core)

# Everything from here to the matching endif() is test/tool-only. The body is
# left unindented so this guard stays a pure wrapper in the diff.
if(SHUTTLE_BUILD_TESTS)

add_executable(shuttle_tests tests/test_main.cpp)
target_link_libraries(shuttle_tests PRIVATE shuttle_core)

add_executable(shuttle_shm_smoke tests/shm_smoke.cpp)
target_link_libraries(shuttle_shm_smoke PRIVATE shuttle_core)

add_executable(shuttle_pshared_smoke tests/pshared_smoke.cpp)
target_link_libraries(shuttle_pshared_smoke PRIVATE shuttle_core Threads::Threads)

add_executable(shuttle_lifecycle_test tests/lifecycle_test.cpp)
target_link_libraries(shuttle_lifecycle_test PRIVATE shuttle_core Threads::Threads)

add_executable(shuttle_capacity_test tests/capacity_test.cpp)
target_link_libraries(shuttle_capacity_test PRIVATE shuttle_core Threads::Threads)

add_executable(shuttle_leak_test tests/leak_test.cpp)
target_link_libraries(shuttle_leak_test PRIVATE shuttle_core Threads::Threads)

add_executable(shuttle_bipbuffer_test tests/bipbuffer_test.cpp)
target_link_libraries(shuttle_bipbuffer_test PRIVATE shuttle_core)

add_executable(shuttle_bipbuffer_edge_test tests/bipbuffer_edge_test.cpp)
target_link_libraries(shuttle_bipbuffer_edge_test PRIVATE shuttle_core)

add_executable(shuttle_bipbuffer_oversize_test tests/bipbuffer_oversize_test.cpp)
target_link_libraries(shuttle_bipbuffer_oversize_test PRIVATE shuttle_core)

add_executable(shuttle_spsc_stress_test tests/spsc_stress_test.cpp)
target_link_libraries(shuttle_spsc_stress_test PRIVATE shuttle_core Threads::Threads)

add_executable(shuttle_spsc_asym_test tests/spsc_asym_test.cpp)
target_link_libraries(shuttle_spsc_asym_test PRIVATE shuttle_core Threads::Threads)

add_executable(shuttle_spsc_wrap_test tests/spsc_wrap_test.cpp)
target_link_libraries(shuttle_spsc_wrap_test PRIVATE shuttle_core Threads::Threads)

add_executable(shuttle_park_idle_test tests/park_idle_test.cpp)
target_link_libraries(shuttle_park_idle_test PRIVATE shuttle_core Threads::Threads)

add_executable(shuttle_trickle_test tests/trickle_test.cpp)
target_link_libraries(shuttle_trickle_test PRIVATE shuttle_core Threads::Threads)

add_executable(shuttle_park_latency_test tests/park_latency_test.cpp)
target_link_libraries(shuttle_park_latency_test PRIVATE shuttle_core Threads::Threads)

add_executable(shuttle_crash_heartbeat_test tests/crash_heartbeat_test.cpp)
target_link_libraries(shuttle_crash_heartbeat_test PRIVATE shuttle_core Threads::Threads)

add_executable(shuttle_robust_mutex_test tests/robust_mutex_test.cpp)
target_link_libraries(shuttle_robust_mutex_test PRIVATE shuttle_core Threads::Threads)

add_executable(shuttle_crash_leak_test tests/crash_leak_test.cpp)
target_link_libraries(shuttle_crash_leak_test PRIVATE shuttle_core Threads::Threads)

add_executable(shuttle_crash_mutex_test tests/crash_mutex_test.cpp)
target_link_libraries(shuttle_crash_mutex_test PRIVATE shuttle_core Threads::Threads)

add_executable(shuttle_cabi_python_test tests/cabi_python_test.cpp)
target_link_libraries(shuttle_cabi_python_test PRIVATE shuttle_c shuttle_core Threads::Threads)
target_compile_definitions(shuttle_cabi_python_test PRIVATE
    SHUTTLE_C_LIB="$<TARGET_FILE:shuttle_c>"
    PY_CONSUMER="${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi/py_consumer.py")

add_executable(shuttle_cabi_rust_test tests/cabi_rust_test.cpp)
target_link_libraries(shuttle_cabi_rust_test PRIVATE shuttle_c shuttle_core Threads::Threads)
target_compile_definitions(shuttle_cabi_rust_test PRIVATE
    SHUTTLE_C_LIBDIR="$<TARGET_FILE_DIR:shuttle_c>"
    RUST_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi/rust"
    RUST_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")

add_executable(shuttle_cabi_errors_test tests/cabi_errors_test.cpp)
target_link_libraries(shuttle_cabi_errors_test PRIVATE shuttle_c shuttle_core Threads::Threads)
target_compile_definitions(shuttle_cabi_errors_test PRIVATE
    SHUTTLE_C_LIB="$<TARGET_FILE:shuttle_c>"
    SHUTTLE_C_LIBDIR="$<TARGET_FILE_DIR:shuttle_c>"
    PY_ERR_PROBE="${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi/err_probe.py"
    RUST_SRC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/ffi/rust"
    RUST_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")

add_executable(shuttle_hugepage_test tests/hugepage_test.cpp)
target_link_libraries(shuttle_hugepage_test PRIVATE shuttle_c shuttle_core Threads::Threads)

add_executable(shuttle_hugetlb_test tests/hugetlb_test.cpp)
target_link_libraries(shuttle_hugetlb_test PRIVATE shuttle_c shuttle_core Threads::Threads)

add_executable(shuttle_stats_test tests/stats_test.cpp)
target_link_libraries(shuttle_stats_test PRIVATE shuttle_c shuttle_core Threads::Threads)

add_executable(shuttle_drop_policy_test tests/drop_policy_test.cpp)
target_link_libraries(shuttle_drop_policy_test PRIVATE shuttle_c shuttle_core Threads::Threads)

add_executable(shuttle_inspect tools/inspect.cpp)
target_link_libraries(shuttle_inspect PRIVATE shuttle_core)

enable_testing()
add_test(NAME shuttle_tests COMMAND shuttle_tests)
add_test(NAME shuttle_shm_smoke COMMAND shuttle_shm_smoke)
add_test(NAME shuttle_pshared_smoke COMMAND shuttle_pshared_smoke)
add_test(NAME shuttle_lifecycle_test COMMAND shuttle_lifecycle_test)
add_test(NAME shuttle_capacity_test COMMAND shuttle_capacity_test)
add_test(NAME shuttle_leak_test COMMAND shuttle_leak_test)
add_test(NAME shuttle_bipbuffer_test COMMAND shuttle_bipbuffer_test)
add_test(NAME shuttle_bipbuffer_edge_test COMMAND shuttle_bipbuffer_edge_test)
add_test(NAME shuttle_bipbuffer_oversize_test COMMAND shuttle_bipbuffer_oversize_test)
add_test(NAME shuttle_spsc_stress_test COMMAND shuttle_spsc_stress_test)
add_test(NAME shuttle_spsc_threads_test COMMAND shuttle_spsc_stress_test threads)
set_tests_properties(shuttle_tests shuttle_shm_smoke shuttle_pshared_smoke
                     shuttle_lifecycle_test shuttle_capacity_test
                     shuttle_leak_test shuttle_bipbuffer_test
                     shuttle_bipbuffer_edge_test shuttle_bipbuffer_oversize_test
                     PROPERTIES TIMEOUT 60)
add_test(NAME shuttle_spsc_asym_test COMMAND shuttle_spsc_asym_test)
add_test(NAME shuttle_spsc_wrap_test COMMAND shuttle_spsc_wrap_test)
add_test(NAME shuttle_spsc_wrap_threads_test COMMAND shuttle_spsc_wrap_test threads)
add_test(NAME shuttle_park_idle_test COMMAND shuttle_park_idle_test)
add_test(NAME shuttle_trickle_test COMMAND shuttle_trickle_test)
add_test(NAME shuttle_park_latency_test COMMAND shuttle_park_latency_test)
add_test(NAME shuttle_park_hotpath_test COMMAND shuttle_park_latency_test hotpath)
add_test(NAME shuttle_crash_heartbeat_test COMMAND shuttle_crash_heartbeat_test)
add_test(NAME shuttle_robust_mutex_test COMMAND shuttle_robust_mutex_test)
add_test(NAME shuttle_crash_leak_test COMMAND shuttle_crash_leak_test)
add_test(NAME shuttle_crash_mutex_test COMMAND shuttle_crash_mutex_test)
add_test(NAME shuttle_cabi_python_test COMMAND shuttle_cabi_python_test)
add_test(NAME shuttle_cabi_rust_test COMMAND shuttle_cabi_rust_test)
add_test(NAME shuttle_cabi_errors_test COMMAND shuttle_cabi_errors_test)
add_test(NAME shuttle_hugepage_test COMMAND shuttle_hugepage_test)
add_test(NAME shuttle_hugetlb_test COMMAND shuttle_hugetlb_test)
add_test(NAME shuttle_stats_test COMMAND shuttle_stats_test)
add_test(NAME shuttle_drop_policy_test COMMAND shuttle_drop_policy_test)
add_test(NAME shuttle_bench_g71 COMMAND shuttle_bench)
add_test(NAME shuttle_nocopy_cpu_test COMMAND shuttle_nocopy_cpu_test)
add_test(NAME shuttle_wake_under_load_test COMMAND shuttle_wake_under_load_test)
set_tests_properties(shuttle_spsc_stress_test shuttle_spsc_threads_test
                     shuttle_spsc_asym_test shuttle_spsc_wrap_test
                     shuttle_spsc_wrap_threads_test shuttle_park_idle_test
                     shuttle_trickle_test shuttle_park_latency_test
                     shuttle_park_hotpath_test shuttle_crash_heartbeat_test
                     shuttle_robust_mutex_test shuttle_crash_leak_test
                     shuttle_crash_mutex_test shuttle_cabi_python_test
                     shuttle_cabi_rust_test shuttle_cabi_errors_test
                     shuttle_hugepage_test shuttle_hugetlb_test
                     shuttle_stats_test shuttle_drop_policy_test
                     shuttle_bench_g71 shuttle_nocopy_cpu_test
                     shuttle_wake_under_load_test
                     PROPERTIES TIMEOUT 300)

endif()  # SHUTTLE_BUILD_TESTS

# --- install / packaging -----------------------------------------------------
# Consumed three ways: find_package(shuttle CONFIG) for CMake projects,
# pkg-config for everyone else, and add_subdirectory/FetchContent (which skips
# this block entirely unless SHUTTLE_INSTALL is forced on).
if(SHUTTLE_INSTALL)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)

  # shuttle_san is the INTERFACE flags carrier that link_libraries() above
  # attaches to every target below it, shuttle_core included. CMake refuses to
  # export a target whose link interface names a target outside the export set,
  # so it rides along. In a non-sanitized build (the only kind worth installing)
  # it carries no flags at all.
  set_target_properties(shuttle_san PROPERTIES EXPORT_NAME san)
  install(TARGETS shuttle_san EXPORT shuttleTargets)

  # No INCLUDES DESTINATION here: the $<INSTALL_INTERFACE:include> on both
  # targets already supplies the exported include dir, and naming it twice
  # duplicates the entry in INTERFACE_INCLUDE_DIRECTORIES.
  install(TARGETS shuttle_core shuttle_c
          EXPORT shuttleTargets
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
          ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

  install(DIRECTORY include/shuttle
          DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  install(EXPORT shuttleTargets
          NAMESPACE shuttle::
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/shuttle)

  configure_package_config_file(
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/shuttleConfig.cmake.in
      ${CMAKE_CURRENT_BINARY_DIR}/shuttleConfig.cmake
      INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/shuttle)

  # SameMajorVersion: the C ABI is frozen at v1, so any 1.x satisfies a
  # find_package(shuttle 1.x) request.
  write_basic_package_version_file(
      ${CMAKE_CURRENT_BINARY_DIR}/shuttleConfigVersion.cmake
      VERSION ${PROJECT_VERSION}
      COMPATIBILITY SameMajorVersion)

  install(FILES
          ${CMAKE_CURRENT_BINARY_DIR}/shuttleConfig.cmake
          ${CMAKE_CURRENT_BINARY_DIR}/shuttleConfigVersion.cmake
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/shuttle)

  # pkg-config: keep libdir/includedir prefix-relative unless GNUInstallDirs
  # handed back absolute paths.
  if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
    set(SHUTTLE_PC_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
  else()
    set(SHUTTLE_PC_LIBDIR "\${exec_prefix}/${CMAKE_INSTALL_LIBDIR}")
  endif()
  if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
    set(SHUTTLE_PC_INCLUDEDIR "${CMAKE_INSTALL_INCLUDEDIR}")
  else()
    set(SHUTTLE_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
  endif()
  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/shuttle.pc.in
                 ${CMAKE_CURRENT_BINARY_DIR}/shuttle.pc @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/shuttle.pc
          DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
endif()

# --- libFuzzer harnesses (opt-in, clang-only) --------------------------------
# Deliberately the last block in the file and self-contained: the fuzz targets
# share no target with the test/bench/install machinery above, and nothing
# above changes when this is OFF (the default). They do inherit the file-scope
# link_libraries(shuttle_san) — harmless, because the guard below forces
# SHUTTLE_SAN=off, which makes shuttle_san an INTERFACE target carrying no
# flags at all.
option(SHUTTLE_FUZZ "Build libFuzzer harnesses (requires clang)" OFF)

if(SHUTTLE_FUZZ)
  # libFuzzer ships with clang's compiler-rt; gcc has no equivalent, so this is
  # a hard requirement rather than a silent skip. Configure with
  # `CC=clang CXX=clang++ cmake -B <dir> -DSHUTTLE_FUZZ=ON`.
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
      "SHUTTLE_FUZZ=ON requires clang (libFuzzer); this build uses "
      "'${CMAKE_CXX_COMPILER_ID}'. Re-run in a fresh build dir with "
      "CC=clang CXX=clang++.")
  endif()

  # The fuzz targets carry their OWN sanitizer flags (-fsanitize=fuzzer,
  # address,undefined) and must not be crossed with the SHUTTLE_SAN machinery:
  # SHUTTLE_SAN=tsan would try to link ThreadSanitizer and AddressSanitizer
  # into one binary, which cannot work, and SHUTTLE_SAN=asan would leave two
  # sources of truth for the same flags. An explicit error beats a documented
  # constraint here because the failure mode of the tsan case is a linker
  # error whose text points nowhere near this file. Fuzzing and the sanitizer
  # test legs use separate build trees anyway.
  if(NOT SHUTTLE_SAN STREQUAL "off")
    message(FATAL_ERROR
      "SHUTTLE_FUZZ=ON is incompatible with SHUTTLE_SAN=${SHUTTLE_SAN}: the "
      "fuzz targets already build with -fsanitize=fuzzer,address,undefined. "
      "Use a separate build directory for the sanitizer test legs.")
  endif()

  # -O1: libFuzzer needs a usable exec/s to get anywhere, and the project's
  # default Debug build type otherwise leaves these unoptimized. -g and the
  # frame pointer keep crash reports readable.
  set(SHUTTLE_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined
                         -fno-omit-frame-pointer -O1 -g)

  # bipbuffer.hpp is header-only, so this harness needs nothing else linked in.
  add_executable(shuttle_bipbuffer_fuzz fuzz/bipbuffer_fuzz.cpp)
  target_include_directories(shuttle_bipbuffer_fuzz PRIVATE include)
  target_compile_options(shuttle_bipbuffer_fuzz PRIVATE ${SHUTTLE_FUZZ_FLAGS})
  target_link_options(shuttle_bipbuffer_fuzz PRIVATE ${SHUTTLE_FUZZ_FLAGS})

  # validate_header lives in src/shuttle.cpp, which is compiled INTO this
  # target rather than linked from shuttle_core on purpose: shuttle_core is
  # built without coverage instrumentation, so linking it would leave the
  # fuzzer blind inside the very function under test.
  add_executable(shuttle_header_fuzz fuzz/header_fuzz.cpp src/shuttle.cpp)
  target_include_directories(shuttle_header_fuzz PRIVATE include)
  target_compile_options(shuttle_header_fuzz PRIVATE ${SHUTTLE_FUZZ_FLAGS})
  target_link_options(shuttle_header_fuzz PRIVATE ${SHUTTLE_FUZZ_FLAGS})
  target_link_libraries(shuttle_header_fuzz PRIVATE Threads::Threads)
endif()
