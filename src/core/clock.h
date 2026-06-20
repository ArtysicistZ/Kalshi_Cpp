#pragma once

#include <cstdint>
#include <chrono>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <x86intrin.h>
#endif

namespace kalshi {

#if defined(__aarch64__) || defined(__arm64__)
// arm64 has no x86 TSC; CNTVCT_EL0 is the equivalent cheap monotonic tick
// source. It runs at a fixed frequency, not the CPU clock, but CycleClock
// calibrates ns-per-tick against steady_clock so the frequency is irrelevant.
inline uint64_t rdtsc() {
    uint64_t v;
    asm volatile("mrs %0, cntvct_el0" : "=r"(v));
    return v;
}
inline uint64_t rdtscp() {
    uint64_t v;
    asm volatile("isb;\n\tmrs %0, cntvct_el0" : "=r"(v) :: "memory");
    return v;
}
#else
inline uint64_t rdtsc() { return __rdtsc(); }
inline uint64_t rdtscp() {
    unsigned int aux;
    return __rdtscp(&aux);
}
#endif

class CycleClock {
    double ns_per_cycle_;

public:
    CycleClock() {
        auto start_time = std::chrono::steady_clock::now();
        uint64_t start_clock = rdtsc();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        auto end_time = std::chrono::steady_clock::now();
        uint64_t end_clock = rdtscp();

        auto elapsed = end_time - start_time;
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
        ns_per_cycle_ = static_cast<double>(ns) / (end_clock - start_clock);
    }
    ~CycleClock() = default;

    inline double to_ns(uint64_t cycles) const { return ns_per_cycle_ * cycles; }

};

}
