#include "lo/manager/FeatureManager.h"
#include "lo/mod/LeviOptimize.h"
#include <ll/api/i18n/I18n.h>
#include <ll/api/mod/NativeMod.h>
#include <system_error>
#include <utility>
#include <vector>

namespace lo::manager {

using namespace fmt::literals;
using namespace ll::i18n_literals;

FeatureManager& FeatureManager::getInstance() {
    static FeatureManager instance;
    return instance;
}

FeatureManager::FeatureManager()  = default;
FeatureManager::~FeatureManager() = default;

// ---- registration ---------------------------------------------------------

bool FeatureManager::addFeature(
    std::string_view       name,
    features::FeatureSide  side,
    std::string_view       trigger,
    std::string_view       stopTrigger,
    features::FeatureFactory factory
) {
    return mFeatures.try_emplace_l(name, [](auto&) {}, FeatureSlot{side, trigger, stopTrigger, factory, nullptr});
}

bool FeatureManager::addTrigger(std::string_view name, triggers::TriggerFactory factory) {
    return mTriggers.try_emplace_l(name, [](auto&) {}, TriggerSlot{factory, nullptr});
}

// ---- configuration --------------------------------------------------------

std::filesystem::path FeatureManager::configPath() const {
    return mod::LeviOptimize::getInstance().getSelf().getConfigDir() / u8"config.json";
}

std::vector<FeatureManager::FeatureRef> FeatureManager::featureSnapshot() const {
    auto refs = std::vector<FeatureRef>{};

    mFeatures.for_each([&](auto const& pair) {
        if (!pair.second.instance) return;
        refs.push_back({pair.first, pair.second.trigger, pair.second.stopTrigger, pair.second.instance});
    });

    return refs;
}

bool FeatureManager::triggerActive(std::string_view trigger) const noexcept {
    auto active = false;
    mTriggers.if_contains(trigger, [&](auto const& pair) {
        active = pair.second.instance && pair.second.instance->active();
    });
    return active;
}

ll::Expected<> FeatureManager::load() {
    auto path = configPath();

    auto& logger = mod::LeviOptimize::getInstance().getSelf().getLogger();

    std::error_code ec;
    auto            missing = !std::filesystem::exists(path, ec);
    if (ec) {
        return ll::makeI18nStringError<"failed to inspect configuration file ({path}): {error}">(
            "path"_a  = path.lexically_normal(),
            "error"_a = ec.message()
        );
    }

    logger.debug(
        missing ? "configuration file ({path}) is missing, using defaults"_tr("path"_a = path.lexically_normal())
                : "loading configuration from ({path})"_tr("path"_a = path.lexically_normal())
    );

    auto root = utils::Json::object();
    if (!missing) {
        auto document = utils::readJson(path);
        if (!document) return ll::forwardError(document.error());

        root = std::move(*document);
        logger.debug("configuration file ({path}) read"_tr("path"_a = path.lexically_normal()));
    }

    if (auto it = root.find("version"); it != root.end() && it->is_number_integer()) {
        auto version = it->get<int>();
        if (version > kConfigVersion) {
            logger.warn(
                "configuration file ({path}) was written by a newer version ({version}); reading it anyway"_tr(
                    "path"_a    = path.lexically_normal(),
                    "version"_a = version
                )
            );
        }
    }
    root["version"] = kConfigVersion;

    createFeatures();
    if (!mBindingsChecked.load()) warnMissingTriggers();

    auto sections = utils::Json::object();
    if (auto it = root.find("features"); it != root.end() && it->is_object()) sections = *it;

    for (auto const& ref : featureSnapshot()) {
        auto section = utils::Json{};
        if (auto it = sections.find(std::string{ref.name}); it != sections.end()) section = *it;

        if (auto result = ref.instance->loadConfig(section); !result) {
            logger.error(
                "feature ({feature}) failed to load its configuration: {error}"_tr(
                    "feature"_a = ref.name,
                    "error"_a   = result.error().message()
                )
            );
            continue;
        }

        logger.debug(
            "feature ({feature}) configured, enable={enable}"_tr(
                "feature"_a = ref.name,
                "enable"_a  = ref.instance->enabled()
            )
        );
    }

    mDocument = std::move(root);
    mLoaded   = true;
    logger.debug("configuration applied to {count} feature(s)"_tr("count"_a = mFeatures.size()));

    if (!save()) {
        return ll::makeI18nStringError<"failed to write the completed configuration to ({path})">(
            "path"_a = path.lexically_normal()
        );
    }
    return {};
}

bool FeatureManager::save() const {
    auto  path   = configPath();
    auto& logger = mod::LeviOptimize::getInstance().getSelf().getLogger();
    logger.debug("saving configuration to ({path})"_tr("path"_a = path.lexically_normal()));

    auto document       = utils::Json::object();
    document["version"] = kConfigVersion;

    auto features = utils::Json::object();
    for (auto const& ref : featureSnapshot()) {
        auto current = ref.instance->saveConfig();
        if (!current) {
            logger.error(
                "feature ({feature}) failed to serialize its configuration: {error}"_tr(
                    "feature"_a = ref.name,
                    "error"_a   = current.error().message()
                )
            );
            continue;
        }
        features[std::string{ref.name}] = std::move(*current);
    }
    document["features"] = std::move(features);

    preserveUnknown(document);

    // Same content as the file that was loaded: leave it alone, so comments and
    // formatting in an untouched file survive.
    if (document == mDocument) {
        logger.debug("configuration is unchanged, not rewriting ({path})"_tr("path"_a = path.lexically_normal()));
        return true;
    }

    if (auto result = utils::writeJson(path, document); !result) {
        logger.error("failed to write configuration file: {error}"_tr("error"_a = result.error().message()));
        return false;
    }

    mDocument = document;
    logger.debug("configuration saved ({count} feature(s))"_tr("count"_a = mFeatures.size()));
    return true;
}

void FeatureManager::preserveUnknown(utils::Json& root) const {
    for (auto const& [key, value] : mDocument.items()) {
        if (key == "version" || key == "features") continue;
        if (root.find(key) == root.end()) root[key] = value;
    }

    auto previous = mDocument.find("features");
    if (previous == mDocument.end() || !previous->is_object()) return;

    auto current = root.find("features");
    if (current == root.end() || !current->is_object()) return;

    for (auto const& [name, value] : previous->items()) {
        if (current->find(name) != current->end()) continue;
        (*current)[name] = value;
    }
}

ll::Expected<> FeatureManager::reload() {
    mod::LeviOptimize::getInstance().getSelf().getLogger().debug("reloading configuration and triggers"_tr());

    auto wasEnabled = mEnabled.load();
    destroyTriggers();

    if (auto result = load(); !result) {
        if (wasEnabled) createTriggers();
        return ll::forwardError(result.error());
    }

    if (wasEnabled) createTriggers();
    refresh();
    return {};
}

ll::Expected<> FeatureManager::reload(std::string_view feature) {
    if (!mFeatures.if_contains(feature, [](auto const&) {})) {
        return ll::makeI18nStringError<"feature ({feature}) is not registered">("feature"_a = feature);
    }

    auto instance = findFeature(feature);
    if (!instance) {
        return ll::makeI18nStringError<"feature ({feature}) is not constructed">("feature"_a = feature);
    }

    mod::LeviOptimize::getInstance().getSelf().getLogger().debug(
        "reloading feature ({feature}) from config.json"_tr("feature"_a = feature)
    );

    auto document = utils::readJson(configPath());
    if (!document) return ll::forwardError(document.error());

    auto section = utils::Json{};
    if (auto features = document->find("features"); features != document->end() && features->is_object()) {
        if (auto entry = features->find(std::string{feature}); entry != features->end()) section = *entry;
    }

    if (auto result = instance->loadConfig(section); !result) {
        return ll::forwardError(result.error());
    }

    refresh();
    return {};
}

ll::Expected<> FeatureManager::setEnabled(std::string_view feature, bool enabled) {
    if (!mFeatures.if_contains(feature, [](auto const&) {})) {
        return ll::makeI18nStringError<"feature ({feature}) is not registered">("feature"_a = feature);
    }

    auto instance = findFeature(feature);
    if (!instance) {
        return ll::makeI18nStringError<"feature ({feature}) is not constructed">("feature"_a = feature);
    }

    auto local = instance->saveConfig();
    if (!local) return ll::forwardError(local.error());

    (*local)["enable"] = enabled;

    if (auto result = instance->loadConfig(*local); !result) {
        return ll::forwardError(result.error());
    }

    if (enabled) {
        // Whether the moment is right is the trigger's call: refresh() starts the
        // feature only when its trigger currently holds (or it has none).
        refresh();
    } else if (instance->running()) {
        instance->stop();
    }

    if (!save()) {
        return ll::makeI18nStringError<"failed to write configuration file ({path})">(
            "path"_a = configPath().lexically_normal()
        );
    }
    return {};
}

// ---- objects --------------------------------------------------------------

void FeatureManager::createFeatures() {
    auto& logger = mod::LeviOptimize::getInstance().getSelf().getLogger();

    auto pending = std::vector<std::string_view>{};
    mFeatures.for_each([&](auto const& pair) {
        if (!pair.second.instance) pending.push_back(pair.first);
    });

    for (auto name : pending) {
        mFeatures.modify_if(name, [&](auto& pair) {
            if (pair.second.instance) return;

            logger.debug("constructing feature ({feature})"_tr("feature"_a = name));

            pair.second.instance = pair.second.factory();
            if (!pair.second.instance) {
                logger.error("failed to construct feature ({feature})"_tr("feature"_a = name));
            }
        });
    }
}

void FeatureManager::createTriggers() {
    auto& logger = mod::LeviOptimize::getInstance().getSelf().getLogger();

    auto pending = std::vector<std::string_view>{};
    mTriggers.for_each([&](auto const& pair) {
        if (!pair.second.instance) pending.push_back(pair.first);
    });

    for (auto name : pending) {
        mTriggers.modify_if(name, [&](auto& pair) {
            if (pair.second.instance) return;

            logger.debug("constructing trigger ({trigger})"_tr("trigger"_a = name));

            pair.second.instance = pair.second.factory();
            if (!pair.second.instance) {
                logger.error("failed to construct trigger ({trigger})"_tr("trigger"_a = name));
                return;
            }

            // The condition may already hold (for example, a hot load while the
            // server is running): apply right away. Otherwise the trigger keeps
            // its subscription and applies when its event fires.
            if (pair.second.instance->active()) {
                logger.debug("trigger ({trigger}) applies immediately"_tr("trigger"_a = name));
                pair.second.instance->apply();
            }
        });
    }
}

void FeatureManager::destroyFeatures() noexcept {
    mod::LeviOptimize::getInstance().getSelf().getLogger().debug(
        "destroying {count} feature(s)"_tr("count"_a = mFeatures.size())
    );
    mFeatures.for_each_m([](auto& pair) { pair.second.instance.reset(); });
}

void FeatureManager::destroyTriggers() noexcept {
    mod::LeviOptimize::getInstance().getSelf().getLogger().debug(
        "destroying {count} trigger(s)"_tr("count"_a = mTriggers.size())
    );
    mTriggers.for_each_m([](auto& pair) { pair.second.instance.reset(); });
}

// ---- lifecycle ------------------------------------------------------------

void FeatureManager::enable() {
    if (mEnabled.load()) return;

    auto& logger = mod::LeviOptimize::getInstance().getSelf().getLogger();

    logger.debug(
        "enabling: {features} feature(s), {triggers} trigger(s)"_tr(
            "features"_a = mFeatures.size(),
            "triggers"_a = mTriggers.size()
        )
    );

    createFeatures();
    if (!mBindingsChecked.load()) warnMissingTriggers();

    mEnabled = true;
    createTriggers();
    refresh();

    logger.debug("enabled"_tr());
}

void FeatureManager::disable() {
    if (!mEnabled.load()) return;

    auto& logger = mod::LeviOptimize::getInstance().getSelf().getLogger();

    logger.debug("disabling"_tr());

    mEnabled = false;
    refresh();
    destroyTriggers();

    logger.debug("disabled"_tr());
}

void FeatureManager::shutdown() {
    mod::LeviOptimize::getInstance().getSelf().getLogger().debug("shutting down"_tr());

    disable();
    destroyFeatures();
    mLoaded          = false;
    mBindingsChecked = false;
    mDocument        = utils::Json::object();
}

void FeatureManager::refresh() noexcept {
    auto& logger        = mod::LeviOptimize::getInstance().getSelf().getLogger();
    auto  managerEnabled = mEnabled.load();

    for (auto const& ref : featureSnapshot()) {
        auto allowed = managerEnabled && ref.instance->enabled();

        if (!allowed) {
            if (ref.instance->running()) {
                logger.debug(
                    "stopping feature ({feature}): manager={manager}, config={config}"_tr(
                        "feature"_a = ref.name,
                        "manager"_a = managerEnabled,
                        "config"_a  = ref.instance->enabled()
                    )
                );
                ref.instance->stop();
            }
            continue;
        }
        if (ref.instance->running()) continue;
        if (!ref.trigger.empty() && !triggerActive(ref.trigger)) {
            logger.debug(
                "feature ({feature}) is waiting for trigger ({trigger})"_tr(
                    "feature"_a = ref.name,
                    "trigger"_a = ref.trigger
                )
            );
            continue;
        }

        logger.debug("starting feature ({feature})"_tr("feature"_a = ref.name));

        if (auto result = ref.instance->start(); !result) {
            logger.error(
                "feature ({feature}) failed to start: {error}"_tr(
                    "feature"_a = ref.name,
                    "error"_a   = result.error().message()
                )
            );
        }
    }
}

void FeatureManager::warnMissingTriggers() {
    auto& logger = mod::LeviOptimize::getInstance().getSelf().getLogger();

    for (auto const& ref : featureSnapshot()) {
        if (!ref.trigger.empty() && !mTriggers.if_contains(ref.trigger, [](auto const&) {})) {
            logger.warn(
                "feature ({feature}) binds trigger ({trigger}), but it is not registered"_tr(
                    "feature"_a = ref.name,
                    "trigger"_a = ref.trigger
                )
            );
        }
        if (!ref.stopTrigger.empty() && !mTriggers.if_contains(ref.stopTrigger, [](auto const&) {})) {
            logger.warn(
                "feature ({feature}) binds stop trigger ({trigger}), but it is not registered"_tr(
                    "feature"_a = ref.name,
                    "trigger"_a = ref.stopTrigger
                )
            );
        }
    }
    mBindingsChecked = true;
}

void FeatureManager::applyTrigger(std::string_view trigger) noexcept {
    auto& logger = mod::LeviOptimize::getInstance().getSelf().getLogger();

    if (!mEnabled.load()) {
        logger.debug("ignoring trigger ({trigger}): the manager is disabled"_tr("trigger"_a = trigger));
        return;
    }

    auto started = size_t{};
    auto stopped = size_t{};

    for (auto const& ref : featureSnapshot()) {
        if (ref.stopTrigger != trigger) continue;
        if (!ref.instance->running()) continue;

        logger.debug("trigger ({trigger}) stops feature ({feature})"_tr("trigger"_a = trigger, "feature"_a = ref.name));
        ref.instance->stop();
        ++stopped;
    }

    for (auto const& ref : featureSnapshot()) {
        if (ref.trigger != trigger || ref.stopTrigger == trigger) continue;
        if (!ref.instance->enabled() || ref.instance->running()) continue;

        logger.debug("trigger ({trigger}) starts feature ({feature})"_tr("trigger"_a = trigger, "feature"_a = ref.name));

        if (auto result = ref.instance->start(); !result) {
            logger.error(
                "feature ({feature}) failed to start: {error}"_tr(
                    "feature"_a = ref.name,
                    "error"_a   = result.error().message()
                )
            );
            continue;
        }
        ++started;
    }

    logger.debug(
        "trigger ({trigger}) applied: {started} started, {stopped} stopped"_tr(
            "trigger"_a = trigger,
            "started"_a = started,
            "stopped"_a = stopped
        )
    );
}

// ---- queries --------------------------------------------------------------

ll::coro::Generator<std::string_view> FeatureManager::featureNames() const {
    auto names = std::vector<std::string_view>{};
    names.reserve(mFeatures.size());
    mFeatures.for_each([&](auto const& pair) { names.push_back(pair.first); });

    for (auto name : names) co_yield name;
}

ll::coro::Generator<std::string_view> FeatureManager::featureNames(features::FeatureSide side) const {
    auto names = std::vector<std::string_view>{};
    mFeatures.for_each([&](auto const& pair) {
        if (pair.second.side != side && pair.second.side != features::FeatureSide::both) return;
        names.push_back(pair.first);
    });

    for (auto name : names) co_yield name;
}

ll::coro::Generator<std::string_view> FeatureManager::triggerNames() const {
    auto names = std::vector<std::string_view>{};
    names.reserve(mTriggers.size());
    mTriggers.for_each([&](auto const& pair) { names.push_back(pair.first); });

    for (auto name : names) co_yield name;
}

bool FeatureManager::isRunning(std::string_view feature) const noexcept {
    auto running = false;
    mFeatures.if_contains(feature, [&](auto const& pair) {
        running = pair.second.instance && pair.second.instance->running();
    });
    return running;
}

bool FeatureManager::isEnabled(std::string_view feature) const noexcept {
    auto enabled = false;
    mFeatures.if_contains(feature, [&](auto const& pair) {
        enabled = pair.second.instance && pair.second.instance->enabled();
    });
    return enabled;
}

std::shared_ptr<features::FeatureBase> FeatureManager::findFeature(std::string_view feature) const noexcept {
    auto instance = std::shared_ptr<features::FeatureBase>{};
    mFeatures.if_contains(feature, [&](auto const& pair) { instance = pair.second.instance; });
    return instance;
}

bool FeatureManager::isApplicable(std::string_view feature, features::FeatureSide side) const noexcept {
    auto applicable = false;
    mFeatures.if_contains(feature, [&](auto const& pair) {
        applicable = pair.second.side == side || pair.second.side == features::FeatureSide::both;
    });
    return applicable;
}

} // namespace lo::manager
