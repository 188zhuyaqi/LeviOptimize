// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Key equality/hash adapted from Endstone NetworkChunkPublisher's current header.
#pragma once

#include "mc/server/ChunkPositionAndDimension.h"

// TODO(SDK): remove this compatibility definition when the generated key gets
// its native equality/hash. Keep it before NetworkChunkPublisher is instantiated.
inline bool operator==(ChunkPositionAndDimension const& a, ChunkPositionAndDimension const& b) {
    return a.mPos->x == b.mPos->x && a.mPos->z == b.mPos->z && a.mType->mValue == b.mType->mValue;
}

template <>
struct std::hash<ChunkPositionAndDimension> {
    size_t operator()(ChunkPositionAndDimension const& key) const {
        size_t seed{};
        auto   part  = std::hash<int>{};
        seed        ^= part(key.mPos->x) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed        ^= part(key.mPos->z) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        seed        ^= part(key.mType->mValue) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};

#include <array>
#include <string_view>

#include "ll/api/memory/Symbol.h"
#include "mc/server/NetworkChunkPublisher.h"

namespace lo::parked_chunks_opt::adapter {
inline constexpr std::array<std::string_view, 6> RequiredSymbols{
    "?_updateChunkPublisherView@ServerPlayer@@UEAAXAEBVVec3@@M@Z",
    "?disconnect@ServerPlayer@@QEAAXXZ",
    "??1ServerPlayer@@UEAA@XZ",
    "?clearRegion@NetworkChunkPublisher@@QEAAXXZ",
    "?destroyRegion@NetworkChunkPublisher@@QEAAXXZ",
    "??1Level@@UEAA@XZ",
};

// TODO(target symbols): keep exact decorated signatures here in one place. An
// unpublished/missing symbol prevents installation of the entire hook group.
inline std::string_view firstMissingSymbol() {
    for (auto symbol : RequiredSymbols) {
        if (!ll::memory::SymbolView(symbol).resolve(true)) return symbol;
    }
    return {};
}

using Queue = std::unordered_map<ChunkPositionAndDimension, std::weak_ptr<LevelChunk>>;
static_assert(sizeof(ChunkPositionAndDimension) == 16);
static_assert(sizeof(Queue) == 64);
static_assert(sizeof(NetworkChunkPublisher) == 488);
} // namespace lo::parked_chunks_opt::adapter
