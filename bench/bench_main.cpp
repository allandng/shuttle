// Phase 0 stub: real benchmark harness arrives in Phase 7.
#include <cstdio>

#include "shuttle/platform.hpp"

int main() {
    std::printf("shuttle_bench: stub (platform=%s)\n",
                shuttle::platform_name());
    return 0;
}
