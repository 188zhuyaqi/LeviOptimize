#define LL_MEMORY_OPERATORS
#include "ll/api/memory/MemoryOperators.h" // IWYU pragma: keep

#include "LeviOptimize.h"
#include "Config.h"
#include "features/network/NetworkRuntime.h"
#include "features/parked_chunks/ParkedChunksRuntime.h"
#include "features/timing/FullFrameTiming.h"

#include "ll/api/Config.h"
#include "ll/api/mod/RegisterHelper.h"
#include "ll/api/utils/ErrorUtils.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/command/ServerCommandRegisterEvent.h"


namespace lo {
namespace command {
extern void registerTimingCommand();
extern void registerPerformanceCommand();
} // namespace command

LeviOptimize& LeviOptimize::getInstance() {
    static LeviOptimize instance;
    return instance;
}

bool LeviOptimize::load() {
    bool res = loadConfig();
    if (getConfig().commands.timingCommand) {
        using namespace ll::event;
        EventBus::getInstance().emplaceListener<ServerCommandRegisterEvent>([](ServerCommandRegisterEvent&) {
            command::registerTimingCommand();
        });
    }
    if (getConfig().commands.performanceCommand) {
        ll::event::EventBus::getInstance().emplaceListener<ll::event::command::ServerCommandRegisterEvent>(
            [](ll::event::command::ServerCommandRegisterEvent&) { command::registerPerformanceCommand(); }
        );
    }
    return res;
}

bool LeviOptimize::unload() {
    // Each check may retire native resources; evaluate all even if one fails.
    auto const networkReady = network_opt::canUnload();
    auto const parkedReady  = parked_chunks_opt::canUnload();
    auto const timingReady  = full_frame_timing::canUnload();
    if (!networkReady || !parkedReady || !timingReady) {
        getLogger().warn(
            "Unload deferred: {} network peer control blocks remain; chunk retirement ready: {}; "
            "frame timing retirement ready: {}. "
            "Allow outstanding native callbacks/tasks and connections to drain, then retry.",
            network_opt::liveWrapperCount(),
            parkedReady,
            timingReady
        );
        return false;
    }
    return true;
}

bool LeviOptimize::enable() { // NOLINT
    if (!mConfig && !loadConfig()) {
        return false;
    }
    return true;
}

bool LeviOptimize::disable() { // NOLINT
    saveConfig();
    mConfig.reset();
    return true;
}

ll::io::Logger& LeviOptimize::getLogger() const { return getSelf().getLogger(); }

ll::io::Logger& getLogger() { return LeviOptimize::getInstance().getLogger(); }

std::string const& LeviOptimize::getName() const { return getSelf().getManifest().name; }

ll::mod::NativeMod& LeviOptimize::getSelf() const { return mSelf; }

std::filesystem::path LeviOptimize::getConfigPath() const { return getSelf().getConfigDir() / u8"config.json"; }

Config& LeviOptimize::getConfig() { return mConfig.value(); }

bool LeviOptimize::loadConfig() {
    bool res{};
    mConfig.emplace();
    try {
        res = ll::config::loadConfig(*mConfig, getConfigPath());
    } catch (...) {
        ll::error_utils::printCurrentException(getLogger());
        res = false;
    }
    if (!res) {
        res = ll::config::saveConfig(*mConfig, getConfigPath());
    }
    return res;
}

bool LeviOptimize::saveConfig() { return ll::config::saveConfig(mConfig.value(), getConfigPath()); }

bool LeviOptimize::isEnabled() const { return getSelf().isEnabled(); }
} // namespace lo

LL_REGISTER_MOD(lo::LeviOptimize, lo::LeviOptimize::getInstance());
