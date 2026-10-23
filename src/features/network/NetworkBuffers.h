// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Adapted from Endstone batched_network_peer.cpp and fe86ade3f.
// Modifications: reentrant scratch leases and checked batch framing.

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lo::network_opt::detail {

class SendScratch {
    struct Slots {
        std::vector<std::unique_ptr<std::string>> buffers;
        std::size_t                               depth{};
    };
    static Slots& slots() {
        // A nontrivial thread_local destructor can run on a BDS worker after
        // the mod DLL was unloaded. Keep only a trivial TLS pointer; module
        // ownership frees all slots once the unload guard permits unloading.
        // Slots from exited threads stay owned until then, never per peer.
        static std::mutex                          mutex;
        static std::vector<std::unique_ptr<Slots>> owners;
        thread_local Slots*                        current{};
        if (!current) {
            auto            slot  = std::make_unique<Slots>();
            auto*           value = slot.get();
            std::lock_guard lock(mutex);
            owners.push_back(std::move(slot));
            current = value;
        }
        return *current;
    }
    Slots&       mSlots;
    std::string* mBuffer;

public:
    SendScratch() : mSlots(slots()) {
        if (mSlots.depth == mSlots.buffers.size()) mSlots.buffers.push_back(std::make_unique<std::string>());
        mBuffer = mSlots.buffers[mSlots.depth].get();
        ++mSlots.depth;
        mBuffer->clear();
    }
    ~SendScratch() { --mSlots.depth; }
    SendScratch(SendScratch const&)            = delete;
    SendScratch& operator=(SendScratch const&) = delete;
    std::string& buffer() { return *mBuffer; }
};

inline bool batchIsCompressible(std::size_t compressible, std::size_t total) {
    return total != 0 && static_cast<float>(compressible) / static_cast<float>(total) > 0.2F;
}

inline bool appendBatchPacket(std::string& stream, std::string_view packet) {
    if (packet.size() > std::numeric_limits<std::uint32_t>::max()) return false;
    auto length = static_cast<std::uint32_t>(packet.size());
    do {
        auto byte   = static_cast<std::uint8_t>(length & 0x7F);
        length    >>= 7;
        stream.push_back(static_cast<char>(byte | (length ? 0x80 : 0)));
    } while (length);
    stream.append(packet);
    return true;
}

// Advances only after a complete varuint32 and complete packet were validated.
// A null result is a broken batch, not an empty successful packet.
inline std::optional<std::string_view> readBatchPacket(std::string_view batch, std::size_t& position) {
    auto          cursor = position;
    std::uint32_t length{};
    for (unsigned index = 0; index < 5; ++index) {
        if (cursor >= batch.size()) return std::nullopt;
        auto byte = static_cast<std::uint8_t>(batch[cursor++]);
        if (index == 4 && (byte & 0xF0) != 0) return std::nullopt;
        length |= static_cast<std::uint32_t>(byte & 0x7F) << (index * 7);
        if ((byte & 0x80) == 0) {
            if (length > batch.size() - cursor) return std::nullopt;
            auto packet = batch.substr(cursor, length);
            position    = cursor + length;
            return packet;
        }
    }
    return std::nullopt;
}

} // namespace lo::network_opt::detail
