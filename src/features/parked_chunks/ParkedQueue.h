// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Adapted from Endstone 099d6f0287bb0ac5f7cfc7653b41afec8083d4ba.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lo::parked_chunks_opt::detail {

enum class QueuedChunkState { Sendable, OutOfView, NotLoaded };

// A native callback may disable this feature or clear a publisher while the
// caller still owns queue iterators. Return extracted nodes only after every
// traversal has left; native clear/destroy instead invalidates those nodes.
struct TraversalState {
    size_t active{};
    bool   retired{};
    bool   discard{};

    void retire(bool clearingNative = false) {
        retired  = true;
        discard |= clearingNative && active != 0;
    }
    [[nodiscard]] bool readyToRestore() const { return retired && active == 0; }
};

class ActiveTraversal {
    TraversalState& state;

public:
    explicit ActiveTraversal(TraversalState& value) : state(value) { ++state.active; }
    ~ActiveTraversal() { --state.active; }
    ActiveTraversal(ActiveTraversal const&)            = delete;
    ActiveTraversal& operator=(ActiveTraversal const&) = delete;
};

template <class Queue>
bool checkQueueHash(Queue const& queue) {
    size_t count{};
    for (auto it = queue.begin(); it != queue.end(); ++it, ++count) {
        auto found = queue.find(it->first);
        if (found == queue.end() || &*found != &*it) return false;
    }
    return count == queue.size();
}

template <class Queue>
class ParkedQueue {
public:
    using Key = typename Queue::key_type;
    struct ParkedChunk {
        typename Queue::node_type node;
        QueuedChunkState          state;
    };
    using Parked = std::unordered_map<Key, ParkedChunk, typename Queue::hasher, typename Queue::key_equal>;
    Parked                                                                     parked;
    std::unordered_set<Key, typename Queue::hasher, typename Queue::key_equal> sendable;

    typename Parked::iterator unpark(Queue& queue, typename Parked::iterator it) {
        if (auto live = queue.find(it->first); live != queue.end()) {
            if (!it->second.node.mapped().expired()) live->second = it->second.node.mapped();
        } else {
            queue.insert(std::move(it->second.node));
        }
        return parked.erase(it);
    }

    void restore(Queue& queue) {
        for (auto it = parked.begin(); it != parked.end();) it = unpark(queue, it);
        sendable.clear();
    }

    template <class Resolve>
    typename Parked::iterator recheck(Queue& queue, typename Parked::iterator it, Resolve&& resolve) {
        if (it->second.node.mapped().expired()) return parked.erase(it);
        if (queue.contains(it->first)) return unpark(queue, it);
        auto state = resolve(it->first);
        if (state == QueuedChunkState::Sendable) return unpark(queue, it);
        it->second.state = state;
        return std::next(it);
    }

    template <class Resolve>
    void recheckAll(Queue& queue, Resolve&& resolve) {
        for (auto it = parked.begin(); it != parked.end();) it = recheck(queue, it, resolve);
    }

    template <class Belongs, class Resolve>
    void parkUnsendable(Queue& queue, Belongs&& belongs, Resolve&& resolve) {
        std::erase_if(sendable, [&](auto const& key) { return !queue.contains(key); });
        for (auto it = queue.begin(); it != queue.end();) {
            if (sendable.contains(it->first) || it->second.expired() || !belongs(it->first)) {
                ++it;
                continue;
            }
            auto state = resolve(it->first);
            if (state == QueuedChunkState::Sendable) {
                sendable.insert(it->first);
                ++it;
                continue;
            }
            auto key  = it->first;
            auto node = queue.extract(it++);
            try {
                auto [entry, inserted] = parked.try_emplace(key);
                if (inserted || entry->second.node.mapped().expired()) {
                    entry->second = ParkedChunk{std::move(node), state};
                }
            } catch (...) {
                if (!node.empty()) queue.insert(std::move(node));
                throw;
            }
        }
    }
};

// The journal retains the same two-tick event window as Endstone, with absolute
// cursors. Late clients request a complete recheck instead of losing load events.
template <class Position, size_t MaxEntries = 65536>
class LoadedJournal {
    static_assert(MaxEntries > 0);
    std::vector<Position> loaded;
    size_t                begin{};
    size_t                seen{};
    std::uint64_t         tick{};
    bool                  initialized{};

public:
    struct Slice {
        bool                  missed;
        size_t                end;
        std::vector<Position> positions;
    };
    void append(Position const& position) {
        // Streaming can continue without players calling the consumer hook.
        // Bound event memory and let older cursors request a complete recheck.
        if (loaded.size() == MaxEntries) {
            auto remove = loaded.size() / 2 + 1;
            loaded.erase(loaded.begin(), loaded.begin() + static_cast<std::ptrdiff_t>(remove));
            begin += remove;
            seen   = std::max(seen, begin);
        }
        loaded.push_back(position);
    }
    void advance(std::uint64_t currentTick) {
        if (initialized && currentTick == tick) return;
        tick        = currentTick;
        initialized = true;
        auto remove = std::min(seen - begin, loaded.size());
        loaded.erase(loaded.begin(), loaded.begin() + static_cast<std::ptrdiff_t>(remove));
        begin += remove;
        seen   = begin + loaded.size();
    }
    [[nodiscard]] size_t end() const { return begin + loaded.size(); }
    [[nodiscard]] Slice  since(size_t cursor) const {
        if (cursor < begin || cursor > end()) return {true, end(), {}};
        return {
            false,
            end(),
            {loaded.begin() + static_cast<std::ptrdiff_t>(cursor - begin), loaded.end()}
        };
    }
};

} // namespace lo::parked_chunks_opt::detail
