// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Adapted from Endstone 099d6f0287bb0ac5f7cfc7653b41afec8083d4ba.
// Modifications: isolate SDK hash/symbol gaps, own load subscriptions per level,
// synchronize event ingestion, and restore extracted nodes at lifetime boundaries.

#include "ParkedChunksAdapter.h"
#include "ParkedChunksRuntime.h"
#include "ParkedQueue.h"
#include "features/features.h"
#include "LeviOptimize.h"

#include <memory>
#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "ll/api/memory/Hook.h"
#include "ll/api/thread/GlobalThreadPauser.h"
#include "mc/server/ServerPlayer.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/Tick.h"
#include "mc/world/level/chunk/ChunkViewSource.h"
#include "mc/world/level/chunk/LevelChunk.h"
#include "mc/world/level/chunk/LevelChunkEventManager.h"

namespace lo::parked_chunks_opt {
namespace {
using Queue = adapter::Queue;
using State = detail::QueuedChunkState;

struct Events {
    std::mutex                      mutex;
    detail::LoadedJournal<ChunkPos> journal;
    size_t                          invalidations{};
};

struct LevelEvents {
    std::shared_ptr<Events>                        events = std::make_shared<Events>();
    std::unique_ptr<Bedrock::PubSub::Subscription> loaded, reloaded, discarded;

    void disconnect() {
        loaded.reset();
        reloaded.reset();
        discarded.reset();
    }

    explicit LevelEvents(Level& level) {
        std::weak_ptr<Events> weak    = events;
        auto                  manager = level.getLevelChunkEventManager();
        loaded.reset(new Bedrock::PubSub::Subscription(manager->getOnChunkLoadedConnector().connect(
            [weak](ChunkSource&, LevelChunk& chunk, int) {
                if (auto state = weak.lock()) {
                    std::lock_guard lock(state->mutex);
                    state->journal.append(chunk.mPosition.get());
                }
            },
            Bedrock::PubSub::ConnectPosition::AtBack,
            nullptr
        )));
        reloaded.reset(new Bedrock::PubSub::Subscription(manager->getOnChunkReloadedConnector().connect(
            [weak](ChunkSource&, LevelChunk& chunk) {
                if (auto state = weak.lock()) {
                    std::lock_guard lock(state->mutex);
                    state->journal.append(chunk.mPosition.get());
                    ++state->invalidations;
                }
            },
            Bedrock::PubSub::ConnectPosition::AtBack,
            nullptr
        )));
        discarded.reset(new Bedrock::PubSub::Subscription(manager->getOnChunkDiscardedConnector().connect(
            [weak](LevelChunk&) {
                if (auto state = weak.lock()) {
                    std::lock_guard lock(state->mutex);
                    ++state->invalidations;
                }
            },
            Bedrock::PubSub::ConnectPosition::AtBack,
            nullptr
        )));
    }
};

struct PlayerState : detail::TraversalState {
    std::weak_ptr<NetworkChunkPublisher> publisher;
    Level*                               level{};
    ChunkViewSource*                     source{};
    int                                  dimension{};
    detail::ParkedQueue<Queue>           queue;
    size_t                               loadedCursor{};
    size_t                               invalidations{};
};

struct Runtime {
    enum class HashCheck { Pending, Passed, Failed } hashCheck{};
    size_t                                                          checkedKeys{};
    bool                                                            retired{};
    size_t                                                          epoch{};
    size_t                                                          depth{};
    std::unordered_map<ServerPlayer*, std::shared_ptr<PlayerState>> players;
    std::unordered_map<Level*, std::shared_ptr<LevelEvents>>        levels;

    void restore(PlayerState& state) {
        if (state.discard) {
            state.queue.parked.clear();
            state.queue.sendable.clear();
            return;
        }
        if (auto publisher = state.publisher.lock()) state.queue.restore(publisher->mQueuedChunks.get());
    }
    void finishPlayer(ServerPlayer* player, std::shared_ptr<PlayerState> const& state) {
        if (!state->readyToRestore()) return;
        restore(*state);
        auto it = players.find(player);
        if (it != players.end() && it->second == state) players.erase(it);
    }
    void restorePlayer(ServerPlayer* player) {
        auto it = players.find(player);
        if (it == players.end()) return;
        ++epoch;
        auto state = it->second;
        state->retire();
        finishPlayer(player, state);
    }
    void restorePublisher(NetworkChunkPublisher* publisher) {
        for (auto it = players.begin(); it != players.end();) {
            auto state = it->second;
            auto owner = state->publisher.lock();
            if (!owner || owner.get() == publisher) {
                ++epoch;
                state->retire(true);
                // Native clear/destroy will erase the live queue. An active
                // traversal cannot reinsert parked nodes after that clear.
                if (state->active == 0) {
                    restore(*state);
                    it = players.erase(it);
                } else ++it;
            } else {
                ++it;
            }
        }
    }
    void restoreLevel(Level* level) {
        for (auto it = players.begin(); it != players.end();) {
            auto state = it->second;
            if (state->level == level) {
                ++epoch;
                state->retire(true);
                if (state->active == 0) {
                    restore(*state);
                    it = players.erase(it);
                } else ++it;
            } else {
                ++it;
            }
        }
        if (auto it = levels.find(level); it != levels.end()) {
            // The active consumer can keep the journal alive, but subscription
            // destruction must finish while the level's connectors still live.
            it->second->disconnect();
            levels.erase(it);
        }
    }
    void restoreAll() {
        ++epoch;
        for (auto it = players.begin(); it != players.end();) {
            auto& state = *it->second;
            state.retire();
            if (state.active == 0) {
                restore(state);
                it = players.erase(it);
            } else ++it;
        }
        for (auto const& [level, events] : levels) events->disconnect();
        levels.clear();
    }

    bool validate(Queue const& queue) {
        if (hashCheck == HashCheck::Failed) return false;
        if (hashCheck == HashCheck::Passed) return true;
        if (queue.size() < 64) return false;
        if (!detail::checkQueueHash(queue)) {
            hashCheck = HashCheck::Failed;
            restoreAll();
            getLogger().warn(
                "optParkedChunks: native chunk queue hash mismatch; restored queues and retained native processing."
            );
            return false;
        }
        checkedKeys += queue.size();
        if (checkedKeys < 256) return false;
        hashCheck = HashCheck::Passed;
        return true;
    }
};

std::atomic<std::shared_ptr<Runtime>> runtime;
std::recursive_mutex                  runtimeMutex;
std::vector<std::weak_ptr<Runtime>>   runtimes;
std::atomic_size_t                    activeCallbacks{};
thread_local Runtime*                 activeRuntime{};
struct Cancelled {};

struct CallbackGuard {
    CallbackGuard() { activeCallbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~CallbackGuard() { activeCallbacks.fetch_sub(1, std::memory_order_acq_rel); }
};

template <class Action>
void withRuntimes(Action&& action) {
    std::lock_guard lock(runtimeMutex);
    // A disable/re-enable inside native code can install a new runtime while
    // an enclosing call still owns parked nodes in its retired predecessor.
    // Keep predecessors visible to lifecycle hooks on every thread.
    for (auto it = runtimes.begin(); it != runtimes.end();) {
        if (auto state = it->lock()) {
            action(*state);
            ++it;
        } else {
            it = runtimes.erase(it);
        }
    }
}

State resolve(ChunkViewSource& source, ChunkPositionAndDimension const& key) {
    // A native callback can clear the queue containing key. Its input must not
    // remain a reference into that queue while getExistingChunk is running.
    auto position = key.mPos.get();
    auto chunk    = source.getExistingChunk(position);
    if (!chunk) return State::OutOfView;
    return chunk->mLoadState.get().load() == ChunkState::Loaded ? State::Sendable : State::NotLoaded;
}
} // namespace

LL_TYPE_INSTANCE_HOOK(
    ParkedChunksUpdateHook,
    ll::memory::HookPriority::Normal,
    ServerPlayer,
    &ServerPlayer::$_updateChunkPublisherView,
    void,
    Vec3 const& position,
    float       minDistance
) {
    CallbackGuard   callback;
    std::lock_guard runtimeLock(runtimeMutex);
    auto            current = runtime.load();
    auto            owner   = mChunkPublisherView;
    if (activeRuntime) {
        withRuntimes([&](Runtime& state) { state.restorePlayer(this); });
        origin(position, minDistance);
        return;
    }
    if (!current || !owner) {
        origin(position, minDistance);
        return;
    }
    if (current->retired || current->hashCheck == Runtime::HashCheck::Failed) {
        origin(position, minDistance);
        return;
    }
    if (current->depth != 0) {
        // Do not mutate a queue underneath an active iterator. Retirement is
        // completed by its outer call after the native callback has returned.
        current->restorePlayer(this);
        origin(position, minDistance);
        return;
    }
    struct Depth {
        std::shared_ptr<Runtime> state;
        Runtime*                 previous = activeRuntime;
        explicit Depth(std::shared_ptr<Runtime> value) : state(std::move(value)) {
            activeRuntime = state.get();
            ++state->depth;
        }
        ~Depth() {
            activeRuntime = previous;
            if (--state->depth == 0 && state->retired) {
                auto expected = state;
                runtime.compare_exchange_strong(expected, {});
            }
        }
    } depth(current);
    auto& level          = getLevel();
    auto  dimension      = getDimensionId().mValue;
    auto  previousCenter = ChunkPos(owner->mLastChunkUpdatePosition.get());
    auto  previousRadius = owner->mLastChunkUpdateRadius;
    if (auto it = current->players.find(this); it != current->players.end()) {
        auto const& state = *it->second;
        if (state.publisher.lock() != owner || state.source != owner->mSource.get() || state.dimension != dimension
            || previousRadius == 0)
            current->restorePlayer(this);
    }
    auto                         epoch = current->epoch;
    std::shared_ptr<PlayerState> originState;
    if (auto it = current->players.find(this); it != current->players.end()) originState = it->second;
    auto nativeUpdate = [&] {
        if (originState) {
            detail::ActiveTraversal active(*originState);
            origin(position, minDistance);
        } else {
            origin(position, minDistance);
        }
    };
    try {
        nativeUpdate();
    } catch (...) {
        if (originState) current->finishPlayer(this, originState);
        throw;
    }
    if (originState) current->finishPlayer(this, originState);
    // origin can synchronously disable/re-enable the feature, disconnect a
    // player, or retire this level. Do not access native owners after that.
    if (current->retired || runtime.load() != current || current->epoch != epoch) return;
    if (mChunkPublisherView != owner || !owner->mSource) {
        current->restorePlayer(this);
        return;
    }
    auto& queue = owner->mQueuedChunks.get();
    if (!current->validate(queue)) return;
    std::shared_ptr<PlayerState> playerState;
    try {
        std::shared_ptr<LevelEvents> eventOwner;
        if (auto it = current->levels.find(&level); it != current->levels.end()) eventOwner = it->second;
        else {
            // Connecting subscriptions invokes native code. Do not retain a map
            // reference across a callback which can clear this runtime.
            eventOwner = std::make_shared<LevelEvents>(level);
            if (current->retired || current->epoch != epoch || runtime.load() != current) return;
            current->levels.emplace(&level, eventOwner);
        }
        auto& stored = current->players[this];
        if (!stored) stored = std::make_shared<PlayerState>();
        playerState     = stored;
        auto& state     = *playerState;
        state.publisher = owner;
        state.level     = &level;
        {
            detail::ActiveTraversal active(state);
            auto                    ensureCurrent = [&] {
                if (state.retired || current->retired || current->epoch != epoch || runtime.load() != current) {
                    throw Cancelled{};
                }
            };
            auto tick = level.getCurrentServerTick().tickID;
            ensureCurrent();
            auto&                                  events = *eventOwner->events;
            detail::LoadedJournal<ChunkPos>::Slice loaded;
            size_t                                 invalidations;
            {
                std::lock_guard eventLock(events.mutex);
                events.journal.advance(tick);
                if (!state.source) state.loadedCursor = events.journal.end();
                loaded        = events.journal.since(state.loadedCursor);
                invalidations = events.invalidations;
            }
            auto* source  = owner->mSource.get();
            auto  recheck = [&](auto const& key) {
                auto result = resolve(*source, key);
                // A reentrant clear can invalidate both the native queue and
                // its iterator. Cancel before ParkedQueue touches either again.
                ensureCurrent();
                return result;
            };
            auto center = ChunkPos(owner->mLastChunkUpdatePosition.get());
            if (state.source != source || state.dimension != dimension || previousRadius == 0) {
                state.queue.restore(queue);
            } else if (center != previousCenter || owner->mLastChunkUpdateRadius != previousRadius) {
                state.queue.sendable.clear();
                state.queue.recheckAll(queue, recheck);
            }
            state.source    = source;
            state.dimension = dimension;
            if (loaded.missed || state.invalidations != invalidations) {
                state.queue.recheckAll(queue, recheck);
            } else {
                for (auto const& pos : loaded.positions) {
                    ChunkPositionAndDimension key;
                    key.mPos  = pos;
                    key.mType = DimensionType{dimension};
                    auto it   = state.queue.parked.find(key);
                    if (it != state.queue.parked.end() && it->second.state == State::NotLoaded) {
                        state.queue.recheck(queue, it, recheck);
                    }
                }
            }
            state.loadedCursor  = loaded.end;
            state.invalidations = invalidations;
            if ((tick + (reinterpret_cast<std::uintptr_t>(owner.get()) >> 4)) % 100 == 0) {
                state.queue.recheckAll(queue, recheck);
            }
            state.queue.parkUnsendable(queue, [&](auto const& key) { return key.mType->mValue == dimension; }, recheck);
        }
        current->finishPlayer(this, playerState);
    } catch (Cancelled const&) {
        // Active's destructor has released every iterator/reference before nodes
        // are returned, or discarded if native clear/destroy already ran.
        if (playerState) current->finishPlayer(this, playerState);
    } catch (...) {
        current->hashCheck = Runtime::HashCheck::Failed;
        current->restoreAll();
        getLogger().warn(
            "optParkedChunks: failed to maintain parked queues; restored all nodes and disabled the fast path."
        );
    }
}

LL_TYPE_INSTANCE_HOOK(
    ParkedPlayerDisconnectHook,
    ll::memory::HookPriority::Normal,
    ServerPlayer,
    &ServerPlayer::disconnect,
    void
) {
    CallbackGuard callback;
    withRuntimes([&](Runtime& state) { state.restorePlayer(this); });
    origin();
}
LL_TYPE_INSTANCE_HOOK(
    ParkedPlayerDtorHook,
    ll::memory::HookPriority::Normal,
    ServerPlayer,
    &ServerPlayer::$dtor,
    void
) {
    CallbackGuard callback;
    withRuntimes([&](Runtime& state) { state.restorePlayer(this); });
    origin();
}
LL_TYPE_INSTANCE_HOOK(
    ParkedClearRegionHook,
    ll::memory::HookPriority::Normal,
    NetworkChunkPublisher,
    &NetworkChunkPublisher::clearRegion,
    void
) {
    CallbackGuard callback;
    withRuntimes([&](Runtime& state) { state.restorePublisher(this); });
    origin();
}
LL_TYPE_INSTANCE_HOOK(
    ParkedDestroyRegionHook,
    ll::memory::HookPriority::Normal,
    NetworkChunkPublisher,
    &NetworkChunkPublisher::destroyRegion,
    void
) {
    CallbackGuard callback;
    withRuntimes([&](Runtime& state) { state.restorePublisher(this); });
    origin();
}
LL_TYPE_INSTANCE_HOOK(ParkedLevelDtorHook, ll::memory::HookPriority::Normal, Level, &Level::$dtor, void) {
    CallbackGuard callback;
    withRuntimes([&](Runtime& state) { state.restoreLevel(this); });
    origin();
}

namespace {
using Hooks = ll::memory::HookRegistrar<
    ParkedChunksUpdateHook,
    ParkedPlayerDisconnectHook,
    ParkedPlayerDtorHook,
    ParkedClearRegionHook,
    ParkedDestroyRegionHook,
    ParkedLevelDtorHook>;
std::optional<Hooks> residentHooks;
} // namespace

struct ParkedChunksOpt::Impl {
    std::shared_ptr<Runtime> state = std::make_shared<Runtime>();
    Impl() {
        std::lock_guard lock(runtimeMutex);
        auto            missing = adapter::firstMissingSymbol();
        if (!missing.empty()) {
            getLogger().warn(
                "optParkedChunks: required symbol {} missing; see parked_chunks/ParkedChunksAdapter.h; skipped.",
                missing
            );
            return;
        }
        runtimes.emplace_back(state);
        if (!residentHooks) residentHooks.emplace();
        runtime.store(state);
    }
    ~Impl() {
        std::lock_guard lock(runtimeMutex);
        state->retired = true;
        state->restoreAll();
        // Keep clear/destroy hooks alive until an active call has unwound its
        // iterators. Those hooks must still see a clear after reentrant disable.
        if (state->depth == 0) {
            auto expected = state;
            runtime.compare_exchange_strong(expected, {});
        }
    }
};

bool canUnload() noexcept {
    try {
        std::lock_guard lock(runtimeMutex);
        auto            current = runtime.load();
        if ((current && !current->retired) || activeCallbacks.load(std::memory_order_acquire)) return false;
        // Never release a trampoline from its own detour. Freeze new entries
        // and recheck immediately before removing the resident cleanup hooks.
        std::optional<ll::thread::GlobalThreadPauser> paused;
        if (ll::memory::shouldHookSuspendThreads()) paused.emplace();
        if (activeCallbacks.load(std::memory_order_acquire)) return false;
        residentHooks.reset();
        runtime.store({});
        runtimes.clear();
        return true;
    } catch (...) {
        return false;
    }
}

void ParkedChunksOpt::call(bool enable) {
    if (enable) {
        if (!impl) impl = std::make_unique<Impl>();
    } else {
        impl.reset();
    }
}
ParkedChunksOpt::ParkedChunksOpt()                                      = default;
ParkedChunksOpt::~ParkedChunksOpt()                                     = default;
ParkedChunksOpt::ParkedChunksOpt(ParkedChunksOpt&&) noexcept            = default;
ParkedChunksOpt& ParkedChunksOpt::operator=(ParkedChunksOpt&&) noexcept = default;
} // namespace lo::parked_chunks_opt
