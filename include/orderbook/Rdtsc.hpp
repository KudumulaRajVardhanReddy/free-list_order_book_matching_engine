// Rdtsc.hpp

#pragma once
#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
#include <x86intrin.h>
#endif

namespace orderbook {

[[nodiscard]] inline uint64_t rdtsc_start() noexcept {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    _mm_lfence();
    return __rdtsc();
#else return 0;
#endif
}

[[nodiscard]] inline uint64_t rdtsc_end() noexcept {
#if defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
    unsigned int aux;
    uint64_t tsc = __rdtscp(&aux);
    _mm_lfence();
    return tsc;
#else return 0;
#endif
}

}
