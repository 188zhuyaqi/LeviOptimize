// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>

#include "ll/api/memory/Symbol.h"

class BatchedNetworkPeer;

namespace lo::network_opt::detail {

// Keep optional native dependencies here. Names come from the LL 26.51
// declarations/clang assembly probe, with Endstone's protected override name
// accepted as an alternative. No address table is copied from another BDS.
struct RuntimeSymbols {
    using Flush = void (*)(BatchedNetworkPeer*, std::function<void()>&&);
    std::uintptr_t incoming{};
    std::uintptr_t destructor{};
    Flush          flush{};

    bool resolve() {
        constexpr std::array incomingNames{
            "?onNewIncomingConnection@NetworkSystem@@EEAA_NAEBVNetworkIdentifier@@$$QEAV?$shared_ptr@VNetworkPeer@@@"
            "std@@@Z",
            "?onNewIncomingConnection@NetworkSystem@@UEAA_NAEBVNetworkIdentifier@@$$QEAV?$shared_ptr@VNetworkPeer@@@"
            "std@@@Z"
        };
        for (auto name : incomingNames) {
            if (auto* address = ll::memory::SymbolView{name}.resolve(true)) {
                incoming = reinterpret_cast<std::uintptr_t>(address);
                break;
            }
        }
        flush = reinterpret_cast<Flush>(
            ll::memory::SymbolView{"?flush@BatchedNetworkPeer@@UEAAX$$QEAV?$function@$$A6AXXZ@std@@@Z"}.resolve(true)
        );
        constexpr std::array destructorNames{"??1NetworkSystem@@MEAA@XZ", "??1NetworkSystem@@UEAA@XZ"};
        for (auto name : destructorNames) {
            if (auto* address = ll::memory::SymbolView{name}.resolve(true)) {
                destructor = reinterpret_cast<std::uintptr_t>(address);
                break;
            }
        }
        return incoming && destructor && flush;
    }
};

} // namespace lo::network_opt::detail
