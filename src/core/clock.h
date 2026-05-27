#pragma once

#include <cstdint>
#include <x86intrin.h>
#include <chrono>
#include <thread>

namespace kalshi {

inline uint64_t rdtsc() { return __rdtsc(); }
inline uint64_t rdtscp() { 
    unsigned int aux;
    return __rdtscp(&aux); 
}

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
