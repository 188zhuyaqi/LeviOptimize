// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Adapted from Endstone fa7b3d4eb. Adds per-level retirement and registry lifetime guards.
#include "features/features.h"
#include "PlayerSnapshotState.h"
#include "LeviOptimize.h"

#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "ll/api/memory/Hook.h"
#include "ll/api/memory/Symbol.h"
#include "mc/deps/ecs/WeakEntityRef.h"
#include "mc/deps/ecs/gamerefs_entity/EntityContext.h"
#include "mc/deps/ecs/gamerefs_entity/EntityRegistry.h"
#include "mc/entity/components/ActorOwnerComponent.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/dimension/Dimension.h"

namespace lo::player_snapshot_opt {
namespace {
struct ActiveUser {
    std::optional<EntityContext>  entity;
    std::weak_ptr<EntityRegistry> registry;
    Player*                       player{};
};
struct Snapshot {
    detail::SnapshotState   state;
    std::vector<ActiveUser> entries;
    std::thread::id         owner = std::this_thread::get_id();
};
std::mutex                                                  snapshotsMutex;
std::unordered_map<Level const*, std::shared_ptr<Snapshot>> snapshots;
std::unordered_set<Level const*>                            retiringLevels;

std::shared_ptr<Snapshot> snapshotFor(Level const& level) {
    std::shared_ptr<Snapshot> snapshot;
    {
        std::lock_guard lock(snapshotsMutex);
        if (retiringLevels.contains(&level)) return {};
        auto& stored = snapshots[&level];
        if (!stored) stored = std::make_shared<Snapshot>();
        snapshot = stored;
    }
    // Native world ownership is unchanged. A different thread uses native traversal.
    if (snapshot->owner != std::this_thread::get_id()) return {};
    auto const& users = level.getActiveUsers();
    auto        bytes = std::as_bytes(std::span{users});
    if (snapshot->state.matches(bytes)) return snapshot;
    if (!snapshot->state.canRebuild()) return {};

    std::vector<ActiveUser> replacement;
    replacement.reserve(users.size());
    for (auto const& user : users) {
        auto& entry    = replacement.emplace_back();
        auto  registry = user.mRegistry.lock();
        if (!registry || !registry->mRegistry->valid(user.mEntity)) continue;
        StackResultStorageEntity entity(user);
        if (!entity) continue;
        entry.entity.emplace(*entity);
        entry.registry = registry;
        entry.player   = Player::tryGetFromEntity(*entry.entity, true);
    }
    snapshot->state.replace(bytes);
    snapshot->entries.swap(replacement);
    return snapshot;
}

Player* resolve(ActiveUser& user, bool includeRemoved) {
    auto registry = user.registry.lock();
    if (!registry || !user.entity || !registry->mRegistry->valid(user.entity->mEntity)) return nullptr;
    // A live registry/entity can temporarily outlive its ActorOwner component.
    // Verify the owner before dereferencing a pointer cached on an earlier call.
    auto owner = user.entity->tryGetComponent<ActorOwnerComponent>();
    if (!owner || !owner->mActor) {
        user.player = nullptr;
        return nullptr;
    }
    if (!user.player || owner->mActor.get() != user.player) {
        user.player = Player::tryGetFromEntity(*user.entity, includeRemoved);
    }
    return user.player && (includeRemoved || !user.player->mRemoved) ? user.player : nullptr;
}

void retire(Level const* level) {
    std::lock_guard lock(snapshotsMutex);
    retiringLevels.insert(level);
    if (auto it = snapshots.find(level); it != snapshots.end()) {
        it->second->state.retire();
        snapshots.erase(it);
    }
}
void clearSnapshots() {
    std::lock_guard lock(snapshotsMutex);
    for (auto& [level, snapshot] : snapshots) snapshot->state.retire();
    snapshots.clear();
}
} // namespace

LL_TYPE_INSTANCE_HOOK(
    ForEachPlayerHook,
    ll::memory::HookPriority::Normal,
    Dimension,
    &Dimension::$forEachPlayer,
    void,
    brstd::function_ref<bool(Player&)> callback
) {
    auto const& level    = static_cast<Level const&>(mLevel);
    auto        snapshot = snapshotFor(level);
    if (!snapshot) {
        origin(callback);
        return;
    }
    auto                         dimension = static_cast<int>(getDimensionId());
    detail::SnapshotState::Visit visit(snapshot->state);
    for (auto& user : snapshot->entries) {
        if (snapshot->state.retired()) break;
        if (auto* player = resolve(user, false);
            player && static_cast<int>(player->getDimensionId()) == dimension && !callback(*player))
            break;
    }
}

LL_TYPE_INSTANCE_HOOK(
    ActivePlayerCountHook,
    ll::memory::HookPriority::Normal,
    Level,
    &Level::$getActivePlayerCount,
    int
) {
    auto snapshot = snapshotFor(*this);
    if (!snapshot || snapshot->entries.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return origin();
    detail::SnapshotState::Visit visit(snapshot->state);
    int                          count{};
    for (auto& user : snapshot->entries) {
        if (snapshot->state.retired()) return origin();
        if (resolve(user, true)) ++count;
    }
    return count;
}

LL_TYPE_INSTANCE_HOOK(LevelDtorHook, ll::memory::HookPriority::Normal, Level, &Level::$dtor, void) {
    retire(this);
    struct Leave {
        Level const* level;
        ~Leave() {
            std::lock_guard lock(snapshotsMutex);
            retiringLevels.erase(level);
        }
    } leave{this};
    origin();
}

struct PlayerSnapshotOpt::Impl {
    std::optional<ll::memory::HookRegistrar<ForEachPlayerHook, ActivePlayerCountHook, LevelDtorHook>> hooks;
    Impl() {
        constexpr std::string_view symbols[]{
            "?forEachPlayer@Dimension@@UEBAXV?$function_ref@$$A6A_NAEAVPlayer@@@Z$$A6A_N0@Z@brstd@@@Z",
            "?getActivePlayerCount@Level@@UEBAHXZ",
            "??1Level@@UEAA@XZ"
        };
        for (auto symbol : symbols) {
            if (!ll::memory::SymbolView(symbol).resolve(true)) {
                getLogger().warn("optPlayerSnapshot: missing {}; keeping native player traversal.", symbol);
                return;
            }
        }
        hooks.emplace();
    }
    ~Impl() {
        hooks.reset();
        clearSnapshots();
    }
};

void PlayerSnapshotOpt::call(bool enable) {
    if (enable) {
        if (!impl) impl = std::make_unique<Impl>();
    } else impl.reset();
}
PlayerSnapshotOpt::PlayerSnapshotOpt()                                        = default;
PlayerSnapshotOpt::~PlayerSnapshotOpt()                                       = default;
PlayerSnapshotOpt::PlayerSnapshotOpt(PlayerSnapshotOpt&&) noexcept            = default;
PlayerSnapshotOpt& PlayerSnapshotOpt::operator=(PlayerSnapshotOpt&&) noexcept = default;
} // namespace lo::player_snapshot_opt
