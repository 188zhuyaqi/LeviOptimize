// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Layout sources: Endstone 874c1fa31, shared_types/world/level/world_position.h
// and platform/brstd/associative_adapter.h. No BDS addresses are embedded here.
#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <type_traits>
#include <vector>

#include "mc/deps/core/math/Vec3.h"
#include "mc/world/level/PlayerLocationSender.h"
#include "mc/world/level/WorldPosition.h"

namespace lo::locator_opt::compat {
struct WorldPositionLayout {
    Vec3 position;
    int  dimension;
};
static_assert(sizeof(WorldPositionLayout) == 16 && offsetof(WorldPositionLayout, dimension) == 12);
static_assert(sizeof(WorldPosition) == 16 && alignof(WorldPosition) == 4);
static_assert(sizeof(PlayerLocationSender) == 128);
static_assert(sizeof(PlayerLocationSender::PlayerLocationData) == 24);

inline Vec3 position(WorldPosition const& value) noexcept {
    static_assert(std::is_trivially_copyable_v<Vec3>);
    Vec3 result;
    std::memcpy(std::addressof(result), std::addressof(value), sizeof(result));
    return result;
}

// The SDK exposes the key accessor but not values/extract/replace. Endstone's
// adapter stores exactly its key and mapped vectors, with an empty comparator.
template <class Map>
class FlatMap {
    Map& mMap;

public:
    using Keys   = typename Map::key_container_type;
    using Values = typename Map::mapped_container_type;
    static_assert(sizeof(Map) == sizeof(Keys) + sizeof(Values));
    static_assert(alignof(Map) == alignof(Keys));
    explicit FlatMap(Map& map) : mMap(map) {}
    Keys&   keys() const noexcept { return const_cast<Keys&>(mMap.keys()); }
    Values& values() const noexcept {
        auto* bytes = reinterpret_cast<std::byte*>(std::addressof(mMap));
        return *std::launder(reinterpret_cast<Values*>(bytes + sizeof(Keys)));
    }
    bool valid() const noexcept {
        return static_cast<void*>(std::addressof(mMap)) == static_cast<void*>(std::addressof(keys()))
            && keys().size() == values().size();
    }
    void clear() const noexcept {
        keys().clear();
        values().clear();
    }
};
} // namespace lo::locator_opt::compat
