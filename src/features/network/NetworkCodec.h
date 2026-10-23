// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Adapted from Endstone compressed_network_peer.cpp (4624070dd, fe86ade3f).
// Modifications: bounded raw payloads, checked growth, strict frame consumption.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <libdeflate.h>
#include <snappy.h>

namespace lo::network_opt::detail {

enum class CompressionAlgorithm : std::uint16_t { Deflate = 0, Snappy = 1, None = 0xFFFF };

struct CompressionSettings {
    bool                 enabled{};
    bool                 hybrid{};
    std::uint16_t        threshold{};
    CompressionAlgorithm algorithm{};
    std::size_t          maxDecompressedSize{};
};

class NetworkCodec {
    struct CompressorDeleter {
        void operator()(libdeflate_compressor* value) const { libdeflate_free_compressor(value); }
    };
    struct DecompressorDeleter {
        void operator()(libdeflate_decompressor* value) const { libdeflate_free_decompressor(value); }
    };

    std::unique_ptr<libdeflate_compressor, CompressorDeleter>     mCompressor;
    std::unique_ptr<libdeflate_compressor, CompressorDeleter>     mStoreCompressor;
    std::unique_ptr<libdeflate_decompressor, DecompressorDeleter> mDecompressor;
    std::mutex                                                    mSendMutex;
    std::mutex                                                    mReceiveMutex;

    bool deflate(std::string_view input, int level, std::string& out, std::size_t prefix) {
        auto& compressor = level == 0 ? mStoreCompressor : mCompressor;
        if (!compressor) compressor.reset(libdeflate_alloc_compressor(level));
        if (!compressor) return false;
        auto bound = libdeflate_deflate_compress_bound(compressor.get(), input.size());
        if (bound > out.max_size() - prefix) return false;
        out.resize(prefix + bound);
        auto written =
            libdeflate_deflate_compress(compressor.get(), input.data(), input.size(), out.data() + prefix, bound);
        if (!written) return false;
        out.resize(prefix + written);
        return true;
    }

    bool inflate(std::string_view input, std::size_t limit, std::string& out) {
        if (!mDecompressor) mDecompressor.reset(libdeflate_alloc_decompressor());
        if (!mDecompressor) return false;
        auto guess    = input.size() > std::numeric_limits<std::size_t>::max() / 6 ? limit : input.size() * 6;
        auto capacity = std::min(limit, std::max<std::size_t>(guess, 1024));
        for (;;) {
            out.resize(capacity);
            std::size_t consumed{}, written{};
            auto        result = libdeflate_deflate_decompress_ex(
                mDecompressor.get(),
                input.data(),
                input.size(),
                out.data(),
                capacity,
                &consumed,
                &written
            );
            if (result == LIBDEFLATE_SUCCESS) {
                if (consumed != input.size() || written > limit) return false;
                out.resize(written);
                return true;
            }
            if (result != LIBDEFLATE_INSUFFICIENT_SPACE || capacity >= limit) return false;
            capacity = capacity > limit / 2 ? limit : std::max<std::size_t>(1, capacity * 2);
        }
    }

public:
    bool encode(std::string_view input, CompressionSettings const& settings, bool compressible, std::string& out) {
        std::lock_guard lock(mSendMutex);
        out.clear();
        if (!settings.enabled || (!settings.hybrid && settings.algorithm == CompressionAlgorithm::None)) {
            out.assign(input);
            return true;
        }
        auto prefix   = settings.hybrid ? std::size_t{1} : 0;
        auto compress = compressible && settings.threshold != 0 && input.size() >= settings.threshold;
        // Without the per-batch hybrid byte there is no raw/store marker for
        // Snappy: even a small/incompressible batch must use negotiated Snappy.
        if (!settings.hybrid && settings.algorithm == CompressionAlgorithm::Snappy) compress = true;
        std::uint8_t header = 0xFF;
        if (!compress) {
            if (settings.hybrid) {
                out.resize(prefix);
                out.append(input);
            } else if (!deflate(input, 0, out, prefix)) {
                out.clear();
                return false;
            }
        } else if (settings.algorithm == CompressionAlgorithm::Snappy) {
            auto bound = snappy::MaxCompressedLength(input.size());
            if (bound < input.size() || bound > out.max_size() - prefix) return false;
            out.resize(prefix + bound);
            std::size_t written{};
            snappy::RawCompress(input.data(), input.size(), out.data() + prefix, &written);
            out.resize(prefix + written);
            header = 1;
        } else if (settings.algorithm == CompressionAlgorithm::Deflate) {
            if (!deflate(input, 6, out, prefix)) {
                out.clear();
                return false;
            }
            header = 0;
        } else if (settings.algorithm == CompressionAlgorithm::None && settings.hybrid) {
            out.resize(prefix);
            out.append(input);
        } else {
            return false;
        }
        if (settings.hybrid) {
            if (header != 0xFF && out.size() - prefix >= input.size()) {
                header = 0xFF;
                out.resize(prefix);
                out.append(input);
            }
            out.front() = static_cast<char>(header);
        }
        return true;
    }

    bool decode(std::string_view input, CompressionSettings const& settings, std::string& out) {
        std::lock_guard lock(mReceiveMutex);
        out.clear();
        if (!settings.enabled) {
            out.assign(input);
            return true;
        }
        if (input.empty()) return false;
        auto algorithm = settings.algorithm;
        if (settings.hybrid) {
            auto header = static_cast<std::uint8_t>(input.front());
            input.remove_prefix(1);
            if (header == 0xFF) {
                if (input.size() > settings.maxDecompressedSize) return false;
                out.assign(input);
                return true;
            }
            algorithm = static_cast<CompressionAlgorithm>(header);
        } else if (algorithm == CompressionAlgorithm::None) {
            if (input.size() > settings.maxDecompressedSize) return false;
            out.assign(input);
            return true;
        }
        if (algorithm != settings.algorithm) return false;
        if (algorithm == CompressionAlgorithm::Snappy) {
            std::size_t size{};
            if (!snappy::GetUncompressedLength(input.data(), input.size(), &size)
                || size > settings.maxDecompressedSize) {
                return false;
            }
            out.resize(size);
            if (snappy::RawUncompress(input.data(), input.size(), out.data())) return true;
        } else if (algorithm == CompressionAlgorithm::Deflate && inflate(input, settings.maxDecompressedSize, out)) {
            return true;
        }
        out.clear();
        return false;
    }
};

} // namespace lo::network_opt::detail
