// Phase 0 empty test: proves the toolchain + sanitizer wiring end-to-end on
// both legs (G0.1 ASan, G0.2 TSan). Real tests arrive with Phase 1.
#include <cstdio>

#include "shuttle/platform.hpp"

int main() {
    std::printf("shuttle_tests: empty test ok (platform=%s)\n",
                shuttle::platform_name());
    return 0;
}
