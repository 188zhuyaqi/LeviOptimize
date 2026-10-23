// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Adapted from 6e7417c001190d497aef2486925c700f3ede9a00 and
// fdea21001193a4aa91fd9725bdcc526f85530474. Uses a seed value, never a live RNG.
#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <limits>

namespace lo::random_tick_prefetch_opt::detail {

inline constexpr std::uint32_t MaxWarmedSamples = 256;

struct Sample {
    std::uint32_t subChunk;
    std::uint16_t index;
    bool          operator==(Sample const&) const = default;
};

enum class SeedPlatform { Windows32BitLong, Linux64BitLong };

struct SampleSet {
    std::array<Sample, MaxWarmedSamples> entries{};
    size_t                               count{};
    [[nodiscard]] size_t                 size() const { return count; }
    Sample const&                        operator[](size_t index) const { return entries[index]; }
};

inline SampleSet
samples(std::uint32_t seed, int top, int speed, SeedPlatform platform = SeedPlatform::Windows32BitLong) {
    if (top < 0 || speed <= 0 || top >= std::numeric_limits<int>::max() / 16) return {};
    auto      count  = static_cast<std::uint64_t>(static_cast<float>(top + 1) * 2.5F);
    auto      total  = std::min<std::uint64_t>(count * static_cast<unsigned>(speed), MaxWarmedSamples);
    auto      height = static_cast<std::uint32_t>((top + 1) * 16);
    SampleSet result;
    for (std::uint64_t i = 0; i < total; ++i) {
        seed = seed * 3 + 0x3C6EF35F;
        std::uint32_t value;
        if (platform == SeedPlatform::Windows32BitLong) {
            auto signedValue = std::bit_cast<std::int32_t>(seed) >> 2;
            value            = static_cast<std::uint32_t>(signedValue < 0 ? -signedValue : signedValue);
        } else {
            value = seed >> 2;
        }
        auto x                         = value & 15;
        auto z                         = (value >> 8) & 15;
        auto y                         = ((value >> 16) & 0xFFFF) % height;
        result.entries[result.count++] = {y >> 4, static_cast<std::uint16_t>((x << 8) | (z << 4) | (y & 15))};
    }
    return result;
}

} // namespace lo::random_tick_prefetch_opt::detail
