// Ported from Endstone bb0475b6:
// https://github.com/EndstoneMC/endstone/blob/bb0475b64744106b328238b3f51d77576ddc6569/src/endstone/runtime/bedrock_hooks/player_tick_manager.cpp#L23-L51

#include "lo/features/PlayerNetworkingOpt.h"
#include "lo/features/detail/InfluenceBatch.h"
#include <mc/world/level/PlayerTickManager.h>
#include <mc/world/level/dimension/ChunkBuildOrderPolicy.h>

namespace lo::features {

using InfluenceBatch = detail::InfluenceBatch<ChunkBuildOrderPolicy>;

LL_TYPE_INSTANCE_HOOK(
    PlayerNetworkingOpt::PlayerNetworkingHook,
    HookPriority::Normal,
    PlayerTickManager,
    &PlayerTickManager::processPlayerNetworking,
    void,
    Tick const& currentTick
) {
    InfluenceBatch batch;
    origin(currentTick);
    batch.finish([](ChunkBuildOrderPolicy* policy) { policy->updateInfluences(); });
}

LL_TYPE_INSTANCE_HOOK(
    PlayerNetworkingOpt::UpdateInfluencesHook,
    HookPriority::Normal,
    ChunkBuildOrderPolicy,
    &ChunkBuildOrderPolicy::$updateInfluences,
    void
) {
    // Calls outside the player networking pass keep their original timing.
    if (!InfluenceBatch::defer(this)) origin();
}

PlayerNetworkingOpt::PlayerNetworkingOpt()  = default;
PlayerNetworkingOpt::~PlayerNetworkingOpt() = default;

ll::Expected<> PlayerNetworkingOpt::onEnable() {
    if (!mHooks) mHooks.emplace();
    return {};
}

void PlayerNetworkingOpt::onDisable() { mHooks.reset(); }

LO_REGISTER_FEATURE(PlayerNetworkingOpt);

} // namespace lo::features
