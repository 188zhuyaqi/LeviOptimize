// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Adapted from Endstone 874c1fa31. Adds RAII windows, recursive fallback and lifecycle cleanup.
#include "features/features.h"
#include "LeviOptimize.h"
#include "LocatorCompat.h"
#include "LocatorState.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "ll/api/memory/Hook.h"
#include "ll/api/memory/Symbol.h"
#include "mc/deps/ecs/WeakEntityRef.h"
#include "mc/entity/components/ActorUniqueIDComponent.h"
#include "mc/world/ServerWaypoint.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/world/level/PlayerListManager.h"

namespace lo::locator_opt {
namespace {
using Action  = std::function<void(UserEntityIdentifierComponent const&, Player const&, Player const&)>;
using Pair    = std::pair<ActorUniqueID, ActorUniqueID>;
using SentMap = std::remove_reference_t<decltype(std::declval<PlayerLocationSender>().mSentPlayerData.get())>;
using CurrentMap =
    std::remove_reference_t<decltype(std::declval<PlayerLocationSender>().mCurrentPlayerLocationData.get())>;
using Position = detail::SentPosition<Vec3>;
using detail::PairAction;
PairAction classify(Action const& action) {
    if (!action) return PairAction::Other;
    std::string_view name = action.target_type().raw_name();
    return detail::classify(name);
}
struct Cache {
    detail::SentPositions<Vec3>                    positions;
    Bedrock::NonOwnerPointer<PlayerLocationSender> sender;
    PlayerListManager*                             manager{};
    std::size_t                                    generation{};
    std::size_t                                    depth{};
    bool                                           dirty{};
    bool                                           retired{};
    std::thread::id                                thread = std::this_thread::get_id();
    void                                           invalidate() noexcept {
        ++generation;
        dirty = true;
    }
};
std::unordered_map<PlayerLocationSender*, std::shared_ptr<Cache>> caches;
std::recursive_mutex                                              cachesMutex;
std::unordered_set<PlayerListManager*>                            retiringManagers;
std::uintptr_t pairTarget{}, removeTarget{}, managerDtorTarget{}, waypointTarget{};
using ShouldSend = bool (*)(
    PlayerLocationSender const*,
    Vec3 const&,
    DimensionType const&,
    bool,
    std::optional<Vec3> const&,
    PlayerLocationSender::PlayerLocationData const&
);
ShouldSend shouldSend{};

class SentWindow {
    compat::FlatMap<SentMap>                                                          mMap;
    detail::VectorWindow<SentMap::key_container_type, SentMap::mapped_container_type> mWindow;
    static SentMap::key_container_type seedKeys(Pair pair, Position const* previous) {
        return previous && previous->state == detail::SentState::Position ? SentMap::key_container_type{pair}
                                                                          : SentMap::key_container_type{};
    }
    static SentMap::mapped_container_type seedValues(Position const* previous) {
        return previous && previous->state == detail::SentState::Position
                 ? SentMap::mapped_container_type{previous->position}
                 : SentMap::mapped_container_type{};
    }

public:
    SentWindow(SentMap& map, Pair pair, Position const* previous)
    : mMap(map),
      mWindow(mMap.keys(), mMap.values(), seedKeys(pair, previous), seedValues(previous)) {}
    void     suspend() noexcept { mWindow.suspend(); }
    void     resume() noexcept { mWindow.resume(); }
    void     clearReal() noexcept { mWindow.clearReal(); }
    void     abandon() noexcept { mWindow.abandon(); }
    Position result(Pair const& pair) const {
        for (std::size_t i = 0; i < mMap.keys().size(); ++i) {
            if (mMap.keys()[i] == pair) {
                if (auto const& pos = mMap.values()[i]) return {*pos, detail::SentState::Position};
                return {{}, detail::SentState::Nullopt};
            }
        }
        return {};
    }
};
thread_local SentWindow* activeWindow{};
thread_local Cache*      activeCache{};
thread_local std::size_t pairDepth{};
struct PairScope {
    PairScope() { ++pairDepth; }
    ~PairScope() { --pairDepth; }
};
struct CacheScope {
    std::shared_ptr<Cache> cache;
    explicit CacheScope(std::shared_ptr<Cache> value) : cache(std::move(value)) { ++cache->depth; }
    ~CacheScope() {
        if (--cache->depth == 0 && cache->dirty) {
            cache->positions.clear();
            cache->dirty = false;
        }
    }
};
struct WindowScope {
    SentWindow* previousWindow = activeWindow;
    Cache*      previousCache  = activeCache;
    WindowScope(SentWindow& window, Cache& cache) {
        activeWindow = &window;
        activeCache  = &cache;
    }
    ~WindowScope() {
        activeWindow = previousWindow;
        activeCache  = previousCache;
    }
};
struct SuspendWindow {
    SentWindow* window = activeWindow;
    SuspendWindow() {
        if (window) window->suspend();
    }
    ~SuspendWindow() {
        if (window) window->resume();
    }
};

void clearNative(Cache& cache) {
    if (auto* sender = cache.sender.get()) {
        compat::FlatMap view(sender->mSentPlayerData.get());
        if (view.valid()) view.clear();
    }
}
void retireManager(PlayerListManager* manager) {
    for (auto it = caches.begin(); it != caches.end();) {
        auto& cache = *it->second;
        if (cache.manager != manager) {
            ++it;
            continue;
        }
        cache.retired = true;
        cache.invalidate();
        if (activeCache == &cache && activeWindow) activeWindow->abandon();
        it = caches.erase(it);
    }
}
void clearCaches() {
    std::lock_guard lock(cachesMutex);
    for (auto& [sender, cache] : caches) {
        clearNative(*cache);
        cache->retired = true;
        cache->invalidate();
        if (activeCache == cache.get() && activeWindow) activeWindow->clearReal();
    }
    caches.clear();
}

void* resolveAny(std::initializer_list<std::string_view> names) {
    for (auto name : names)
        if (auto* result = ll::memory::SymbolView(name).resolve(true)) return result;
    return nullptr;
}
} // namespace

LL_TYPE_INSTANCE_HOOK(
    PairHook,
    ll::memory::HookPriority::Normal,
    PlayerLocationSender,
    pairTarget,
    void,
    std::vector<WeakEntityRef> const& users,
    Action                            action
) {
    std::lock_guard lock(cachesMutex);
    auto            kind = classify(action);
    if (pairDepth != 0) {
        // Native recursion must never observe the temporary one-pair map. Drop
        // previous positions and let native routing rebuild them conservatively.
        SuspendWindow suspend;
        if (activeCache) {
            activeCache->invalidate();
            clearNative(*activeCache);
        }
        origin(users, std::move(action));
        return;
    }
    if (kind == PairAction::Other) {
        // An unknown native action may alter the sent-data map. It owns that map
        // unchanged; a later known action must not reuse our older pair snapshot.
        for (auto& [sender, cache] : caches) {
            cache->positions.clear();
            cache->invalidate();
        }
        origin(users, std::move(action));
        return;
    }
    PairScope                 pairScope;
    std::shared_ptr<Cache>    cache;
    std::optional<CacheScope> cacheScope;
    // Destroy before cacheScope so an exceptional native action discards the
    // partial pair snapshot when the final enclosing visit unwinds.
    detail::InvalidateOnException invalidateOnException([&cache]() noexcept {
        if (!cache) return;
        cache->invalidate();
        if (cache->depth == 0) cache->positions.clear();
    });
    std::size_t                   generation{};
    PlayerLocationSender*         sender{};
    Player const*                 lastViewer{};
    ActorUniqueID                 viewerId{};
    std::size_t                   viewerSlot{};
    DimensionType                 viewerDimension{};
    struct Observed {
        ActorUniqueID                                   id{};
        std::size_t                                     slot{};
        PlayerLocationSender::PlayerLocationData const* data{};
    };
    std::unordered_map<Player const*, Observed> observedPlayers;

    auto bind = [&](Player const& viewer) -> bool {
        auto manager = viewer.getLevel().getPlayerListManager();
        if (retiringManagers.contains(manager.get())) return false;
        auto* currentSender = manager->mPlayerLocationSender->get();
        if (!currentSender) return false;
        if (sender) return sender == currentSender && cache && !cache->retired && cache->generation == generation;
        sender = currentSender;
        compat::FlatMap sent(sender->mSentPlayerData.get());
        compat::FlatMap current(sender->mCurrentPlayerLocationData.get());
        if (!sent.valid() || !current.valid()) return false;
        for (auto it = caches.begin(); it != caches.end();) {
            if (!it->second->sender || (it->second->manager == manager.get() && it->first != sender)) {
                it->second->retired = true;
                it->second->invalidate();
                it = caches.erase(it);
            } else ++it;
        }
        auto& stored = caches[sender];
        if (!stored || stored->sender.get() != sender || stored->manager != manager.get()) {
            stored          = std::make_shared<Cache>();
            stored->sender  = manager->mPlayerLocationSender.get();
            stored->manager = manager.get();
        }
        cache = stored;
        if (cache->thread != std::this_thread::get_id() || cache->retired) return false;
        generation = cache->generation;
        cacheScope.emplace(cache);
        return true;
    };

    auto run = [&](UserEntityIdentifierComponent const& user,
                   Player const&                        viewer,
                   Player const&                        observed,
                   Position const*                      previous,
                   Pair                                 pair) -> Position {
        SentWindow  window(sender->mSentPlayerData.get(), pair, previous);
        WindowScope scope(window, *cache);
        action(user, viewer, observed);
        if (cache->retired) return {};
        return window.result(pair);
    };
    auto check = [&](UserEntityIdentifierComponent const& user, Player const& viewer, Player const& observed) {
        if (!bind(viewer)) {
            action(user, viewer, observed);
            return;
        }
        if (kind == PairAction::Hide) {
            run(user, viewer, observed, nullptr, {});
            return;
        }
        if (&viewer != lastViewer) {
            lastViewer      = &viewer;
            viewerId        = viewer.getOrCreateUniqueID();
            viewerSlot      = cache->positions.slot(viewerId.rawID);
            viewerDimension = viewer.getDimensionId();
        }
        auto [it, inserted]  = observedPlayers.try_emplace(&observed);
        auto& observedPlayer = it->second;
        if (inserted) {
            observedPlayer.id   = observed.getOrCreateUniqueID();
            observedPlayer.slot = cache->positions.slot(observedPlayer.id.rawID);
            compat::FlatMap current(sender->mCurrentPlayerLocationData.get());
            auto const&     keys = current.keys();
            auto            key  = std::ranges::lower_bound(keys, observedPlayer.id.rawID, {}, &ActorUniqueID::rawID);
            if (key != keys.end() && *key == observedPlayer.id)
                observedPlayer.data = &current.values()[key - keys.begin()];
        }
        auto        previous = cache->positions.at(viewerSlot, observedPlayer.slot);
        auto const* data     = observedPlayer.data;
        if (data && previous.state != detail::SentState::Absent && !data->mIsSpectator) {
            if (previous.state == detail::SentState::Nullopt) {
                if (!shouldSend(sender, viewer.getPosition(), viewerDimension, false, std::nullopt, *data)) return;
            } else if (
                data->mPosition.get() && static_cast<int>(data->mDimension.get()) == static_cast<int>(viewerDimension)
                && !shouldSend(sender, viewer.getPosition(), viewerDimension, false, previous.position, *data)
            )
                return;
        }
        auto result = run(user, viewer, observed, &previous, Pair{viewerId, observedPlayer.id});
        if (!cache->retired && cache->generation == generation)
            cache->positions.at(viewerSlot, observedPlayer.slot) = result;
    };
    origin(users, check);
    if (kind == PairAction::Hide && cache) {
        cache->invalidate();
        clearNative(*cache);
    }
}

LL_TYPE_INSTANCE_HOOK(
    PlayerRemovedHook,
    ll::memory::HookPriority::Normal,
    PlayerListManager,
    removeTarget,
    void,
    EntityContext& entity
) {
    std::lock_guard lock(cachesMutex);
    // The native removal handler must erase pairs in the full sent-data map,
    // even if removal was triggered from inside a packet action.
    SuspendWindow suspend;
    if (auto id = entity.tryGetComponent<ActorUniqueIDComponent>()) {
        for (auto& [sender, cache] : caches) {
            if (cache->manager != this) continue;
            cache->positions.remove(id->mActorUniqueID->rawID);
            if (cache->depth != 0) cache->invalidate();
        }
    } else {
        // An already stripped entity cannot identify a row safely.
        for (auto& [sender, cache] : caches) {
            if (cache->manager == this) {
                cache->invalidate();
                if (!cache->depth) cache->positions.clear();
            }
        }
    }
    origin(entity);
}
LL_TYPE_INSTANCE_HOOK(ManagerDtorHook, ll::memory::HookPriority::Normal, PlayerListManager, managerDtorTarget, void) {
    std::lock_guard lock(cachesMutex);
    SuspendWindow   suspend;
    retiringManagers.insert(this);
    struct Leave {
        PlayerListManager* manager;
        ~Leave() { retiringManagers.erase(manager); }
    } leave{this};
    retireManager(this);
    origin();
}

static_assert(sizeof(ServerWaypoint) == 144);
LL_TYPE_INSTANCE_HOOK(
    WaypointUpdateHook,
    ll::memory::HookPriority::Normal,
    ServerWaypoint,
    waypointTarget,
    void,
    Player const& viewer
) {
    Texture const* selected{};
    auto const&    textures = mTextureSelector->mTextures.get();
    if (!textures.empty()) {
        auto        pos     = compat::position(mWorldPos.get());
        auto const& viewing = viewer.getPosition();
        auto        dx = pos.x - viewing.x, dy = pos.y - viewing.y, dz = pos.z - viewing.z;
        auto        distance = std::sqrt(dz * dz + (dy * dy + dx * dx));
        for (auto const& texture : textures) {
            if (detail::textureMatches(distance, texture.mLowerBound, texture.mUpperBound.get())) {
                selected = &texture;
                break;
            }
        }
    }
    if (!selected) {
        if (mTexturePath.get()) setTexturePath(std::nullopt);
    } else {
        if (mTexturePath.get() != selected->mTexturePath.get()) setTexturePath(selected->mTexturePath.get());
        auto const& size = selected->mIconSize.get();
        if (detail::iconChanged(size.x, size.y, mIconSize->x, mIconSize->y)) {
            mUpdateFlags    |= static_cast<std::uint32_t>(UpdateFlags::IconSize);
            mIconSize.get()  = size;
        }
    }
    auto visible = calculateIsVisible(viewer);
    if (mIsVisible != visible) {
        mUpdateFlags |= static_cast<std::uint32_t>(UpdateFlags::Visibility);
        mIsVisible    = visible;
    }
}

struct LocatorOpt::Impl {
    std::optional<ll::memory::HookRegistrar<PairHook, PlayerRemovedHook, ManagerDtorHook>> pairs;
    std::optional<ll::memory::HookRegistrar<WaypointUpdateHook>>                           waypoints;
    Impl() {
        pairTarget        = reinterpret_cast<std::uintptr_t>(resolveAny(
            {"?_forEachClientPlayerPair@PlayerLocationSender@@AEAAXAEBV?$vector@VWeakEntityRef@@V?$allocator@"
             "VWeakEntityRef@@@std@@@std@@V?$function@$$A6AXAEBVUserEntityIdentifierComponent@@AEBVPlayer@@1@Z@3@@Z",
             "?_forEachClientPlayerPair@PlayerLocationSender@@QEAAXAEBV?$vector@VWeakEntityRef@@V?$allocator@"
             "VWeakEntityRef@@@std@@@std@@V?$function@$$A6AXAEBVUserEntityIdentifierComponent@@AEBVPlayer@@1@Z@3@@Z"}
        ));
        shouldSend        = reinterpret_cast<ShouldSend>(resolveAny(
            {"?_shouldSendPositionPacket@PlayerLocationSender@@AEBA_NAEBVVec3@@AEBUDimensionType@@_NAEBV?$optional@"
             "VVec3@@@std@@AEBUPlayerLocationData@1@@Z",
             "?_shouldSendPositionPacket@PlayerLocationSender@@QEBA_NAEBVVec3@@AEBUDimensionType@@_NAEBV?$optional@"
             "VVec3@@@std@@AEBUPlayerLocationData@1@@Z"}
        ));
        removeTarget      = reinterpret_cast<std::uintptr_t>(resolveAny(
            {"?_onGameplayUserRemoved@PlayerListManager@@QEAAXAEAVEntityContext@@@Z",
             "?_onGameplayUserRemoved@PlayerListManager@@AEAAXAEAVEntityContext@@@Z"}
        ));
        managerDtorTarget = reinterpret_cast<std::uintptr_t>(resolveAny({"??1PlayerListManager@@QEAA@XZ"}));
        waypointTarget = reinterpret_cast<std::uintptr_t>(resolveAny({"?update@ServerWaypoint@@UEAAXAEBVPlayer@@@Z"}));
        if (pairTarget && shouldSend && removeTarget && managerDtorTarget) pairs.emplace();
        else getLogger().warn("optLocator: missing pair/lifecycle symbols; keeping native locator pair handling.");
        if (waypointTarget) waypoints.emplace();
        else getLogger().warn("optLocator: missing ServerWaypoint::update symbol; keeping native waypoint updates.");
    }
    ~Impl() {
        pairs.reset();
        waypoints.reset();
        clearCaches();
    }
};
void LocatorOpt::call(bool enable) {
    if (enable) {
        if (!impl) impl = std::make_unique<Impl>();
    } else impl.reset();
}
LocatorOpt::LocatorOpt()                                 = default;
LocatorOpt::~LocatorOpt()                                = default;
LocatorOpt::LocatorOpt(LocatorOpt&&) noexcept            = default;
LocatorOpt& LocatorOpt::operator=(LocatorOpt&&) noexcept = default;
} // namespace lo::locator_opt
