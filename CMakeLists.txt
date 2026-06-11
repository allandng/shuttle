cmake_minimum_required(VERSION 3.25)
project(shuttle LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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
target_include_directories(shuttle_c PUBLIC include)
target_link_libraries(shuttle_c PRIVATE Threads::Threads)

# Benchmark binary (Phase 7): also UNsanitized — instrumentation overhead
# would invalidate the transport-ratio comparison (NFR-P1). Compiled -O2
# regardless of the default Debug build type, for the same reason.
add_executable(shuttle_bench bench/bench_main.cpp src/shuttle.cpp)
target_include_directories(shuttle_bench PRIVATE include)
target_compile_options(shuttle_bench PRIVATE -O2)
target_link_libraries(shuttle_bench PRIVATE Threads::Threads)

link_libraries(shuttle_san)  # every target below is sanitizer-instrumented

# Core library: must stay free of any test/bench dependency.
add_library(shuttle_core STATIC src/shuttle.cpp)
target_include_directories(shuttle_core PUBLIC include)

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
add_test(NAME shuttle_bench_g71 COMMAND shuttle_bench)
set_tests_properties(shuttle_spsc_stress_test shuttle_spsc_threads_test
                     shuttle_spsc_asym_test shuttle_spsc_wrap_test
                     shuttle_spsc_wrap_threads_test shuttle_park_idle_test
                     shuttle_trickle_test shuttle_park_latency_test
                     shuttle_park_hotpath_test shuttle_crash_heartbeat_test
                     shuttle_robust_mutex_test shuttle_crash_leak_test
                     shuttle_crash_mutex_test shuttle_cabi_python_test
                     shuttle_cabi_rust_test shuttle_cabi_errors_test
                     shuttle_bench_g71
                     PROPERTIES TIMEOUT 300)
