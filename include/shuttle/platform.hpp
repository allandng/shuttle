#pragma once

// The single platform seam. Nothing else in the codebase may #ifdef on
// platform; every macOS-vs-Linux divergence (robust mutexes, timedwait
// clocks, shm name limits, one-shot ftruncate, ...) gets an interface here
// with two implementations.

#if defined(__linux__)
  #define SHUTTLE_PLATFORM_LINUX 1
  #define SHUTTLE_PLATFORM_NAME "linux"
#elif defined(__APPLE__)
  #define SHUTTLE_PLATFORM_MACOS 1
  #define SHUTTLE_PLATFORM_NAME "macos"
#else
  #error "Shuttle supports Linux and macOS only (v1.0)"
#endif

namespace shuttle {

// Stub surface for Phase 0 toolchain verification; real seam functions
// (wait_blocking, recover_if_peer_dead, shm name validation, ...) arrive in
// Phases 1-5.
const char* platform_name() noexcept;

}  // namespace shuttle
