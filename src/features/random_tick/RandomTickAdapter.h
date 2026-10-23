// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
#pragma once

#include <cstdint>
#include <string_view>

#include "ll/api/memory/Symbol.h"

namespace lo::random_tick_prefetch_opt::adapter {

inline constexpr std::string_view TickSymbol =
    "?tickImpl@LevelChunk@@QEAAXAEAVBlockSource@@AEBUTick@@V?$function@$$A6AXXZ@std@@@Z";
// The decorated spelling is verified with clang's MSVC ABI for the SDK method
// signature. TODO(target symbols): publish/verify this function-local static in
// the target symbol database. Missing data symbols leave the feature unhooked.
// This is deliberately not a BDS-version address or the removed seed-unlock patch.
inline constexpr std::string_view SeedSymbol =
    "?seed@?1??tickImpl@LevelChunk@@QEAAXAEAVBlockSource@@AEBUTick@@V?$function@$$A6AXXZ@std@@@Z@4IA";

using SeedResolver = std::uint32_t const* (*)();
// Developer integration point when an SDK exposes a verified static-data resolver.
// Set before enabling the feature. The resolver must return a stable, readable
// live seed; it must not call nextInt(), mutate the RNG, or return a copied seed.
inline SeedResolver seedResolverOverride{};

inline std::uint32_t const* resolveSeed() {
    if (seedResolverOverride) return seedResolverOverride();
    return reinterpret_cast<std::uint32_t const*>(ll::memory::SymbolView(SeedSymbol).resolve(true));
}

inline bool hasTickSymbol() { return ll::memory::SymbolView(TickSymbol).resolve(true) != nullptr; }

} // namespace lo::random_tick_prefetch_opt::adapter
