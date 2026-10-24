#pragma once
#include <concepts>
#include <type_traits>

namespace lo::base {

// What a feature's config struct has to provide. Checked once per feature type
// at registration.
template <class Config>
concept FeatureConfig =
    std::is_aggregate_v<Config> && std::copyable<Config>
    && requires(Config const& config) {
           { config.enable } -> std::convertible_to<bool>;
       };

// Optional change callback: a feature that wants to react to a reload defines it
// and compares the two structs; a feature that does not is simply left with the
// new struct. It has to be public, because the detection happens outside the
// class.
template <class T, class Config>
concept HasConfigHook = requires(T& self, Config const& oldConfig, Config const& nextConfig) {
    self.onConfigChanged(oldConfig, nextConfig);
};

} // namespace lo::base
