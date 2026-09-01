#pragma once
#include <cstdint>

namespace block3d {

struct XorShift32 {
    uint32_t state;
    explicit XorShift32(uint32_t seed) : state(seed) {}

    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    float next_float() {
        return static_cast<float>(static_cast<int>(next()) % 200 - 100);
    }

    uint64_t rand_u64_mod(uint64_t max) {
        return static_cast<uint64_t>(next()) % max;
    }
};

} // namespace block3d
