// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Adapted from Endstone commits 03be20b637ff1b4c86e7d6d8fdbd97cbcc7803e6
// and 6790acf3c1cc52cd7a7451e1a590279666e64937.
// Modifications: explicit frame boundaries, initialized-sample averages and a
// deterministic clock interface for tests. No idle time is counted as tick work.

#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <optional>

namespace lo::full_frame_timing {

struct TimingSnapshot {
    std::size_t samples{};
    double      currentMspt{};
    double      averageMspt{};
    double      currentTps{};
    double      averageTps{};
};

class FullFrameTimingState {
public:
    using Clock                         = std::chrono::steady_clock;
    using TimePoint                     = Clock::time_point;
    static constexpr std::size_t Window = 20;

private:
    std::array<double, Window> mSamples{};
    std::size_t                mCount{};
    std::size_t                mNext{};
    double                     mCurrent{};
    std::optional<TimePoint>   mStart;
    bool                       mTickPending{};

    void endPending(TimePoint now) {
        if (!mStart || !mTickPending) return;
        if (now >= *mStart) {
            mCurrent        = std::chrono::duration<double, std::milli>(now - *mStart).count();
            mSamples[mNext] = mCurrent;
            mNext           = (mNext + 1) % Window;
            mCount          = std::min(mCount + 1, Window);
        }
        mStart       = now;
        mTickPending = false;
    }

public:
    // Resetting a missing/aborted frame never turns its idle gap into a sample.
    void beginFrame(TimePoint now) noexcept {
        mStart       = now;
        mTickPending = false;
    }

    // A catch-up frame may run multiple Level ticks. The next tick's start
    // finishes the previous sample; the first includes pre-Level network work.
    void beginTick(TimePoint now) { endPending(now); }
    void completeTick() noexcept { mTickPending = mStart.has_value(); }
    void endFrame(TimePoint now) {
        endPending(now);
        cancelFrame();
    }
    void cancelFrame() noexcept {
        mStart.reset();
        mTickPending = false;
    }
    void reset() noexcept { *this = {}; }

    [[nodiscard]] static constexpr double ticksPerSecond(double mspt) noexcept {
        return std::min(20.0, 1000.0 / std::max(1.0, mspt));
    }

    [[nodiscard]] TimingSnapshot snapshot() const noexcept {
        TimingSnapshot result{.samples = mCount};
        if (mCount == 0) return result;
        result.currentMspt = mCurrent;
        result.currentTps  = ticksPerSecond(mCurrent);
        for (std::size_t index = 0; index < mCount; ++index) {
            result.averageMspt += mSamples[index];
            result.averageTps  += ticksPerSecond(mSamples[index]);
        }
        result.averageMspt /= static_cast<double>(mCount);
        result.averageTps  /= static_cast<double>(mCount);
        return result;
    }
};

} // namespace lo::full_frame_timing
