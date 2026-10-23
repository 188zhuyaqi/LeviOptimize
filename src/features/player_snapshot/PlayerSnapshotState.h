// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Adapted from Endstone fa7b3d4eb: scoped active-user snapshot traversal.
#pragma once

#include <cstddef>
#include <atomic>
#include <cstring>
#include <span>
#include <vector>

namespace lo::player_snapshot_opt::detail {

class SnapshotState {
    std::vector<std::byte> mBytes;
    std::size_t            mDepth{};
    std::atomic_bool       mRetired{};

public:
    [[nodiscard]] bool matches(std::span<std::byte const> bytes) const noexcept {
        return !mRetired.load() && mBytes.size() == bytes.size()
            && (bytes.empty() || std::memcmp(mBytes.data(), bytes.data(), bytes.size()) == 0);
    }
    [[nodiscard]] bool canRebuild() const noexcept { return !mRetired.load() && mDepth == 0; }
    void               replace(std::span<std::byte const> bytes) { mBytes.assign(bytes.begin(), bytes.end()); }
    void               retire() noexcept { mRetired.store(true); }
    [[nodiscard]] bool retired() const noexcept { return mRetired.load(); }

    class Visit {
        SnapshotState& mState;

    public:
        explicit Visit(SnapshotState& state) noexcept : mState(state) { ++mState.mDepth; }
        ~Visit() { --mState.mDepth; }
        Visit(Visit const&)            = delete;
        Visit& operator=(Visit const&) = delete;
    };
};

} // namespace lo::player_snapshot_opt::detail
