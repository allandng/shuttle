cmake_minimum_required(VERSION 3.25)
project(shuttle LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

# Sanitizer config: ASan and TSan are mutually exclusive (cannot link both),
# hence separate build trees selected via this cache variable.
set(SHUTTLE_SAN "off" CACHE STRING "Sanitizer config: off | asan | tsan")

if(SHUTTLE_SAN STREQUAL "asan")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
elseif(SHUTTLE_SAN STREQUAL "tsan")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
elseif(NOT SHUTTLE_SAN STREQUAL "off")
  message(FATAL_ERROR "SHUTTLE_SAN must be one of: off, asan, tsan (got '${SHUTTLE_SAN}')")
endif()

add_compile_options(-Wall -Wextra)

# Core library: must stay free of any test/bench dependency.
add_library(shuttle_core STATIC src/shuttle.cpp)
target_include_directories(shuttle_core PUBLIC include)

add_executable(shuttle_tests tests/test_main.cpp)
target_link_libraries(shuttle_tests PRIVATE shuttle_core)

add_executable(shuttle_bench bench/bench_main.cpp)
target_link_libraries(shuttle_bench PRIVATE shuttle_core)

add_executable(shuttle_shm_smoke tests/shm_smoke.cpp)
target_link_libraries(shuttle_shm_smoke PRIVATE shuttle_core)

find_package(Threads REQUIRED)
add_executable(shuttle_pshared_smoke tests/pshared_smoke.cpp)
target_link_libraries(shuttle_pshared_smoke PRIVATE shuttle_core Threads::Threads)

add_executable(shuttle_lifecycle_test tests/lifecycle_test.cpp)
target_link_libraries(shuttle_lifecycle_test PRIVATE shuttle_core Threads::Threads)

add_executable(shuttle_inspect tools/inspect.cpp)
target_link_libraries(shuttle_inspect PRIVATE shuttle_core)

enable_testing()
add_test(NAME shuttle_tests COMMAND shuttle_tests)
add_test(NAME shuttle_shm_smoke COMMAND shuttle_shm_smoke)
add_test(NAME shuttle_pshared_smoke COMMAND shuttle_pshared_smoke)
add_test(NAME shuttle_lifecycle_test COMMAND shuttle_lifecycle_test)
set_tests_properties(shuttle_tests shuttle_shm_smoke shuttle_pshared_smoke
                     shuttle_lifecycle_test
                     PROPERTIES TIMEOUT 60)
