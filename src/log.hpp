#pragma once

// Co-op build: the log is always on (release too) -- every test run is read back from
// tlou2-coop.log next to the game exe.
#include <cstdio>
namespace to1log {
    void Init();
    void Printf(const char* fmt, ...);
}
#define TO1_LOG(...) ::to1log::Printf(__VA_ARGS__)
