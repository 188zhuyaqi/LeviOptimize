// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Adapted from 6e7417c001190d497aef2486925c700f3ede9a00 and
// fdea21001193a4aa91fd9725bdcc526f85530474; isolates unresolved seed symbols,
// retains the native synchronous callback scope, and reads the seed without changing it.

#include "features/features.h"
#include "LeviOptimize.h"
#include "RandomTickAdapter.h"
#include "RandomTickSamples.h"

#include <array>
#include <functional>
#include <optional>
#include <utility>
#include <xmmintrin.h>

#include "ll/api/memory/Hook.h"
#include "mc/world/level/ILevel.h"
#include "mc/world/level/block/Block.h"
#include "mc/world/level/block/BlockType.h"
#include "mc/world/level/block/registry/BlockTypeRegistry.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/chunk/SubChunk.h"
#include "mc/world/level/storage/GameRule.h"
#include "mc/world/level/storage/GameRules.h"

namespace lo::random_tick_prefetch_opt {
namespace {
std::uint32_t const* liveSeed{};

void warmRandomTicks(LevelChunk const& chunk) {
    if (!liveSeed) return;
    auto const& rules     = chunk.mLevel.getGameRules().mGameRules.get();
    auto        ruleIndex = static_cast<size_t>(GameRules::GameRulesIndex::RandomTickSpeed);
    if (ruleIndex >= rules.size()) return;
    auto* speed    = std::get_if<int>(&rules[ruleIndex].mValue.get());
    auto* registry = chunk.mLevel.getBlockTypeRegistry().get().get();
    if (!speed || *speed <= 0 || !registry || !registry->mDirectAccessBlocks) return;
    auto* air = registry->mDirectAccessBlocks->mAirBlock.mDefaultState;
    if (!air) return;
    auto const& subChunks = chunk.mSubChunks.get();
    int         top       = -1;
    for (auto i = static_cast<int>(subChunks.size()) - 1; i >= 0; --i) {
        auto* standard = subChunks[static_cast<size_t>(i)].mBlocksReadPtr[0];
        if (!standard) return;
        auto* extra = subChunks[static_cast<size_t>(i)].mBlocksReadPtr[1];
        if (!standard->isPaletteUniform(*air) || (extra && !extra->isPaletteUniform(*air))) {
            top = i;
            break;
        }
    }
    // Every stage only reads: neither the live seed nor the game's Random is advanced.
    auto                                                  selected = detail::samples(*liveSeed, top, *speed);
    std::array<SubChunk const*, detail::MaxWarmedSamples> records{};
    std::array<SubChunkStorage<Block> const*, detail::MaxWarmedSamples> storages{};
    std::array<Block const*, detail::MaxWarmedSamples>                  blocks{};
    for (size_t i = 0; i < selected.size(); ++i) {
        if (selected[i].subChunk >= subChunks.size()) continue;
        records[i] = &subChunks[selected[i].subChunk];
        _mm_prefetch(reinterpret_cast<char const*>(&records[i]->mBlocksReadPtr[0]), _MM_HINT_T0);
    }
    for (size_t i = 0; i < selected.size(); ++i) {
        storages[i] = records[i] ? records[i]->mBlocksReadPtr[0] : nullptr;
        if (storages[i]) _mm_prefetch(reinterpret_cast<char const*>(storages[i]), _MM_HINT_T0);
    }
    for (size_t i = 0; i < selected.size(); ++i) {
        blocks[i] = storages[i] ? &storages[i]->getElement(selected[i].index) : nullptr;
    }
    for (size_t i = 0; i < selected.size(); ++i) {
        if (blocks[i]) {
            _mm_prefetch(reinterpret_cast<char const*>(&blocks[i]->getBlockType().mEventManager.get()), _MM_HINT_T0);
        }
    }
}
} // namespace

LL_TYPE_INSTANCE_HOOK(
    RandomTickPrefetchHook,
    ll::memory::HookPriority::Normal,
    LevelChunk,
    &LevelChunk::tickImpl,
    void,
    BlockSource&          tickRegion,
    Tick const&           tick,
    std::function<void()> spawnerCallback
) {
    // tickImpl invokes this callback synchronously. Keep Endstone's small capture
    // rather than copying a std::function into a heap-allocated wrapper per chunk.
    origin(tickRegion, tick, [this, &spawnerCallback] {
        if (spawnerCallback) spawnerCallback();
        warmRandomTicks(*this);
    });
}

struct RandomTickPrefetchOpt::Impl {
    std::optional<ll::memory::HookRegistrar<RandomTickPrefetchHook>> hook;
    Impl() {
        auto* seed = adapter::resolveSeed();
        if (!seed || !adapter::hasTickSymbol()) {
            getLogger().warn(
                "optRandomTickPrefetch: tick/seed symbol missing; see random_tick/RandomTickAdapter.h; skipped."
            );
            return;
        }
        liveSeed = seed;
        hook.emplace();
    }
    ~Impl() {
        hook.reset();
        liveSeed = nullptr;
    }
};

void RandomTickPrefetchOpt::call(bool enable) {
    if (enable) {
        if (!impl) impl = std::make_unique<Impl>();
    } else {
        impl.reset();
    }
}
RandomTickPrefetchOpt::RandomTickPrefetchOpt()                                            = default;
RandomTickPrefetchOpt::~RandomTickPrefetchOpt()                                           = default;
RandomTickPrefetchOpt::RandomTickPrefetchOpt(RandomTickPrefetchOpt&&) noexcept            = default;
RandomTickPrefetchOpt& RandomTickPrefetchOpt::operator=(RandomTickPrefetchOpt&&) noexcept = default;
} // namespace lo::random_tick_prefetch_opt
