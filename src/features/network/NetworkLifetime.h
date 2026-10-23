// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

namespace lo::network_opt::detail {

// A native weak_ptr can outlive its peer. Its control block still contains a
// DLL-defined deleter, so count until the last weak owner releases it as well.
class PeerLifetimeCounter {
    std::atomic_size_t mCount{};
    struct Token {
        PeerLifetimeCounter& counter;
        explicit Token(PeerLifetimeCounter& owner) : counter(owner) {
            counter.mCount.fetch_add(1, std::memory_order_acq_rel);
        }
        ~Token() { counter.mCount.fetch_sub(1, std::memory_order_acq_rel); }
    };

public:
    std::size_t count() const noexcept { return mCount.load(std::memory_order_acquire); }

    template <class Peer, class... Args>
    std::shared_ptr<Peer> make(Args&&... args) {
        auto lifetime = std::make_shared<Token>(*this);
        return std::shared_ptr<Peer>{
            new Peer(std::forward<Args>(args)...),
            [lifetime = std::move(lifetime)](Peer* value) { delete value; }
        };
    }
};

} // namespace lo::network_opt::detail
