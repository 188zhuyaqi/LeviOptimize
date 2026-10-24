// Ported from Endstone bb0475b6:
// https://github.com/EndstoneMC/endstone/blob/bb0475b64744106b328238b3f51d77576ddc6569/src/endstone/runtime/bedrock_hooks/dimension.cpp#L33-L146

#include "lo/features/PlayerSnapshotOpt.h"
#include <mc/deps/ecs/WeakEntityRef.h>
#include <mc/deps/ecs/gamerefs_entity/EntityRegistry.h>
#include <mc/deps/ecs/gamerefs_entity/StackResultStorageEntity.h>
#include <mc/entity/components/ActorOwnerComponent.h>
#include <mc/platform/brstd/function_ref.h>
#include <mc/world/actor/player/Player.h>
#include <mc/world/level/Level.h>
#include <mc/world/level/dimension/Dimension.h>
#include <limits>
#include <span>
#include <thread>
#include <utility>

namespace lo::features {

std::shared_ptr<PlayerSnapshotOpt::Snapshot> PlayerSnapshotOpt::snapshotFor(Level const& level) {
    if (mRetiring.contains(&level)) return {};

    auto& stored = mSnapshots[&level];
    if (!stored) stored = std::make_shared<Snapshot>();

    auto snapshot = stored;

    // The snapshot belongs to the thread that built it; another thread falls
    // back to native traversal.
    if (snapshot->owner != std::this_thread::get_id()) return {};

    auto& users = level.getActiveUsers();
    auto        bytes = std::as_bytes(std::span{users});
    if (snapshot->state.matches(bytes)) return snapshot;
    if (!snapshot->state.canRebuild()) return {};

    std::vector<ActiveUser> replacement{};
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

Player* PlayerSnapshotOpt::resolve(ActiveUser& user, bool includeRemoved) {
    auto registry = user.registry.lock();
    if (!registry || !user.entity || !registry->mRegistry->valid(user.entity->mEntity)) return nullptr;

    // A live registry/entity can temporarily outlive its ActorOwner component;
    // verify the owner before reusing a pointer cached on an earlier call.
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

void PlayerSnapshotOpt::retire(Level const* level) {
    mRetiring.insert(level);
    if (auto it = mSnapshots.find(level); it != mSnapshots.end()) {
        it->second->state.retire();
        mSnapshots.erase(it);
    }
}

void PlayerSnapshotOpt::unretire(Level const* level) { mRetiring.erase(level); }

void PlayerSnapshotOpt::clearSnapshots() {
    for (auto& [level, snapshot] : mSnapshots) snapshot->state.retire();
    mSnapshots.clear();
    mRetiring.clear();
}

LL_TYPE_INSTANCE_HOOK(
    PlayerSnapshotOpt::ForEachPlayerHook,
    HookPriority::Normal,
    Dimension,
    &Dimension::$forEachPlayer,
    void,
    brstd::function_ref<bool(Player&)> callback
) {
    auto self = get();
    if (!self) return origin(callback);

    auto& level    = static_cast<Level&>(mLevel);
    auto  snapshot = self->snapshotFor(level);
    if (!snapshot) return origin(callback);

    auto dimension = getDimensionId();

    detail::SnapshotState::Visit visit(snapshot->state);
    for (auto& user : snapshot->entries) {
        if (snapshot->state.retired()) break;
        if (auto* player = resolve(user, false);
            player && player->getDimensionId() == dimension && !callback(*player)) {
            break;
        }
    }
}

LL_TYPE_INSTANCE_HOOK(
    PlayerSnapshotOpt::ActivePlayerCountHook,
    HookPriority::Normal,
    Level,
    &Level::$getActivePlayerCount,
    int
) {
    auto self = get();
    if (!self) return origin();

    auto snapshot = self->snapshotFor(*this);
    if (!snapshot || snapshot->entries.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return origin();
    }

    detail::SnapshotState::Visit visit(snapshot->state);
    auto                         count = int{};
    for (auto& user : snapshot->entries) {
        if (snapshot->state.retired()) return origin();
        if (resolve(user, true)) ++count;
    }
    return count;
}

LL_TYPE_INSTANCE_HOOK(
    PlayerSnapshotOpt::LevelDtorHook,
    HookPriority::Normal,
    Level,
    &Level::$dtor,
    void
) {
    auto self = get();
    if (!self) return origin();

    self->retire(this);
    origin();
    self->unretire(this);
}

PlayerSnapshotOpt::PlayerSnapshotOpt()  = default;
PlayerSnapshotOpt::~PlayerSnapshotOpt() = default;

ll::Expected<> PlayerSnapshotOpt::onEnable() {
    if (!mHooks) mHooks.emplace();
    return {};
}

void PlayerSnapshotOpt::onDisable() {
    mHooks.reset();
    clearSnapshots();
}

LO_REGISTER_FEATURE(PlayerSnapshotOpt);

} // namespace lo::features
