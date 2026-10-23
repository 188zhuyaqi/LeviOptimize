// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Adapted from Endstone 874c1fa31: per-sender pair slots with row/column retirement.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lo::locator_opt::detail {
template <class Invalidate>
class InvalidateOnException {
    int        mExceptions = std::uncaught_exceptions();
    Invalidate mInvalidate;

public:
    explicit InvalidateOnException(Invalidate invalidate) : mInvalidate(std::move(invalidate)) {
        static_assert(std::is_nothrow_invocable_v<Invalidate&>);
    }
    ~InvalidateOnException() noexcept {
        if (std::uncaught_exceptions() > mExceptions) mInvalidate();
    }
    InvalidateOnException(InvalidateOnException const&)            = delete;
    InvalidateOnException& operator=(InvalidateOnException const&) = delete;
};

enum class PairAction { Other, Check, Hide };
inline PairAction classify(std::string_view name) noexcept {
    if (name.find("checkPlayerPairsAndMaybeSendPackets") != std::string_view::npos) return PairAction::Check;
    if (name.find("sendPacketsHidingAllPlayers") != std::string_view::npos) return PairAction::Hide;
    return PairAction::Other;
}
inline bool textureMatches(float distance, int lower, std::optional<int> upper) noexcept {
    return (!upper || static_cast<float>(*upper) > distance) && distance >= static_cast<float>(lower);
}
inline bool iconChanged(float x, float y, float oldX, float oldY) noexcept {
    constexpr auto epsilon = std::numeric_limits<float>::epsilon();
    return !(epsilon >= std::fabs(x - oldX)) || !(epsilon >= std::fabs(y - oldY));
}
// Swap both vectors as one transaction. Native callbacks see only the window;
// exceptions restore the real map. Reentrant native work can suspend it briefly.
template <class Keys, class Values>
class VectorWindow {
    Keys&       mKeys;
    Values&     mValues;
    Keys        mRealKeys, mWindowKeys;
    Values      mRealValues, mWindowValues;
    std::size_t mSuspendDepth{};
    bool        mAbandoned{};

public:
    VectorWindow(Keys& keys, Values& values, Keys seedKeys = {}, Values seedValues = {})
    : mKeys(keys),
      mValues(values),
      mWindowKeys(std::move(seedKeys)),
      mWindowValues(std::move(seedValues)) {
        mKeys.swap(mRealKeys);
        mValues.swap(mRealValues);
        mKeys.swap(mWindowKeys);
        mValues.swap(mWindowValues);
    }
    VectorWindow(VectorWindow const&)            = delete;
    VectorWindow& operator=(VectorWindow const&) = delete;
    void          suspend() noexcept {
        if (mAbandoned || mSuspendDepth++ != 0) return;
        mKeys.swap(mWindowKeys);
        mValues.swap(mWindowValues);
        mKeys.swap(mRealKeys);
        mValues.swap(mRealValues);
    }
    void resume() noexcept {
        if (mAbandoned || mSuspendDepth == 0 || --mSuspendDepth != 0) return;
        mKeys.swap(mRealKeys);
        mValues.swap(mRealValues);
        mKeys.swap(mWindowKeys);
        mValues.swap(mWindowValues);
    }
    void clearReal() noexcept {
        mRealKeys.clear();
        mRealValues.clear();
    }
    void abandon() noexcept { mAbandoned = true; }
    ~VectorWindow() { suspend(); }
};

enum class SentState : std::uint8_t { Absent, Nullopt, Position };
template <class Position>
struct SentPosition {
    Position  position{};
    SentState state = SentState::Absent;
};

template <class Position>
class SentPositions {
    std::unordered_map<std::int64_t, std::size_t> mSlots;
    std::vector<std::size_t>                      mFree;
    std::vector<SentPosition<Position>>           mEntries;
    std::size_t                                   mCapacity{};
    std::size_t                                   mUsed{};

    void grow(std::size_t capacity) {
        if (capacity == 0 || capacity > mEntries.max_size() / capacity)
            throw std::length_error("locator pair capacity");
        std::vector<SentPosition<Position>> replacement(capacity * capacity);
        for (std::size_t i = 0; i < mCapacity; ++i) {
            std::copy_n(mEntries.begin() + i * mCapacity, mCapacity, replacement.begin() + i * capacity);
        }
        mEntries.swap(replacement);
        mCapacity = capacity;
    }

public:
    void clear() noexcept {
        mSlots.clear();
        mFree.clear();
        mEntries.clear();
        mCapacity = 0;
        mUsed     = 0;
    }
    std::size_t slot(std::int64_t id) {
        if (auto it = mSlots.find(id); it != mSlots.end()) return it->second;
        if (!mFree.empty()) {
            auto index = mFree.back();
            mSlots.emplace(id, index);
            mFree.pop_back();
            return index;
        }
        if (mUsed == mCapacity) {
            if (mCapacity > std::numeric_limits<std::size_t>::max() / 2)
                throw std::length_error("locator pair capacity");
            grow(mCapacity == 0 ? 128 : mCapacity * 2);
        }
        mSlots.emplace(id, mUsed);
        return mUsed++;
    }
    SentPosition<Position>& at(std::size_t viewer, std::size_t observed) {
        return mEntries[viewer * mCapacity + observed];
    }
    void remove(std::int64_t id) {
        auto it = mSlots.find(id);
        if (it == mSlots.end()) return;
        auto index = it->second;
        mFree.push_back(index);
        for (std::size_t i = 0; i < mCapacity; ++i) {
            at(index, i) = {};
            at(i, index) = {};
        }
        mSlots.erase(it);
    }
};
} // namespace lo::locator_opt::detail
