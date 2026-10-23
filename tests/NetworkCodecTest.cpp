// SPDX-License-Identifier: GPL-3.0-or-later
#include "features/network/NetworkBuffers.h"
#include "features/network/NetworkCodec.h"
#include "features/network/NetworkLifetime.h"

#include <array>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace lo::network_opt::detail;

namespace {
void require(bool condition) {
    if (!condition) throw std::runtime_error("requirement failed");
}
CompressionSettings settings(CompressionAlgorithm algorithm = CompressionAlgorithm::Deflate, bool hybrid = true) {
    return {true, hybrid, 16, algorithm, 1024 * 1024};
}
void roundTrip(std::string const& payload, CompressionSettings options, bool compressible = true) {
    NetworkCodec codec;
    std::string  encoded, decoded;
    require(codec.encode(payload, options, compressible, encoded));
    require(codec.decode(encoded, options, decoded));
    require(decoded == payload);
}
template <class Function>
void test(char const* name, Function&& function, unsigned& passed) {
    function();
    ++passed;
    std::cout << "PASS " << name << '\n';
}
} // namespace

int main() {
    unsigned passed{};
    try {
        test(
            "disabled binary passthrough",
            [] {
                auto option    = settings();
                option.enabled = false;
                roundTrip(std::string{"a\0b", 3}, option);
            },
            passed
        );
        test(
            "known raw DEFLATE fixture",
            [] {
                NetworkCodec codec;
                std::string  out;
                require(codec.decode(
                    std::string{"\xCB\x48\xCD\xC9\xC9\x07\x00", 7},
                    settings(CompressionAlgorithm::Deflate, false),
                    out
                ));
                require(out == "hello");
            },
            passed
        );
        test(
            "DEFLATE hybrid roundtrip and growth",
            [] { roundTrip(std::string(1024 * 1024, 'a'), settings()); },
            passed
        );
        test(
            "hybrid DEFLATE header",
            [] {
                NetworkCodec codec;
                std::string  encoded;
                require(codec.encode(std::string(1024, 'a'), settings(), true, encoded));
                require(encoded.front() == 0);
            },
            passed
        );
        test(
            "legacy DEFLATE roundtrip",
            [] { roundTrip(std::string(4096, 'x'), settings(CompressionAlgorithm::Deflate, false)); },
            passed
        );
        test(
            "legacy incompressible store block",
            [] { roundTrip("uncompressed legacy batch", settings(CompressionAlgorithm::Deflate, false), false); },
            passed
        );
        test(
            "hybrid threshold keeps bytes raw",
            [] {
                NetworkCodec codec;
                std::string  encoded;
                require(codec.encode("small", settings(), true, encoded));
                require(encoded == std::string{"\xFFsmall", 6});
            },
            passed
        );
        test(
            "hybrid ineffective compression falls back",
            [] {
                NetworkCodec codec;
                std::string  encoded;
                auto         option = settings();
                option.threshold    = 1;
                require(codec.encode("xy", option, true, encoded));
                require(encoded == std::string{"\xFFxy", 3});
            },
            passed
        );
        test(
            "zero threshold disables optional compression",
            [] {
                NetworkCodec codec;
                std::string  encoded;
                auto         option = settings();
                option.threshold    = 0;
                require(codec.encode(std::string(128, 'a'), option, true, encoded));
                require(static_cast<unsigned char>(encoded.front()) == 0xFF);
            },
            passed
        );
        test(
            "Snappy hybrid roundtrip",
            [] { roundTrip(std::string(16384, 's'), settings(CompressionAlgorithm::Snappy)); },
            passed
        );
        test(
            "Snappy legacy below threshold remains negotiated",
            [] { roundTrip("tiny", settings(CompressionAlgorithm::Snappy, false), false); },
            passed
        );
        test(
            "known raw Snappy fixture",
            [] {
                NetworkCodec codec;
                std::string  out;
                require(
                    codec.decode(std::string{"\x05\x10hello", 7}, settings(CompressionAlgorithm::Snappy, false), out)
                );
                require(out == "hello");
            },
            passed
        );
        test(
            "hybrid algorithm mismatch rejected",
            [] {
                NetworkCodec codec;
                std::string  encoded, out = "stale";
                require(codec.encode(std::string(1024, 's'), settings(CompressionAlgorithm::Snappy), true, encoded));
                require(!codec.decode(encoded, settings(), out));
                require(out.empty());
            },
            passed
        );
        test(
            "unknown hybrid algorithm rejected",
            [] {
                NetworkCodec codec;
                std::string  out;
                require(!codec.decode(std::string{"\x02x", 2}, settings(), out));
            },
            passed
        );
        test(
            "empty compressed frame rejected",
            [] {
                NetworkCodec codec;
                std::string  out;
                require(!codec.decode({}, settings(), out));
                require(!codec.decode(std::string{"\0", 1}, settings(), out));
            },
            passed
        );
        test(
            "truncated DEFLATE rejected",
            [] {
                NetworkCodec codec;
                std::string  encoded, out;
                require(codec.encode(std::string(1024, 'a'), settings(), true, encoded));
                encoded.pop_back();
                require(!codec.decode(encoded, settings(), out));
                require(out.empty());
            },
            passed
        );
        test(
            "trailing DEFLATE bytes rejected",
            [] {
                NetworkCodec codec;
                std::string  encoded, out;
                require(codec.encode(std::string(1024, 'a'), settings(), true, encoded));
                encoded.push_back('x');
                require(!codec.decode(encoded, settings(), out));
            },
            passed
        );
        test(
            "exact decompressed size cap",
            [] {
                NetworkCodec codec;
                std::string  encoded, out;
                auto         option = settings();
                require(codec.encode(std::string(8192, 'z'), option, true, encoded));
                option.maxDecompressedSize = 8192;
                require(codec.decode(encoded, option, out));
                option.maxDecompressedSize = 8191;
                require(!codec.decode(encoded, option, out));
                require(out.empty());
            },
            passed
        );
        test(
            "Snappy declared oversized length rejected",
            [] {
                NetworkCodec codec;
                std::string  out;
                require(!codec.decode(
                    std::string{"\x01\x80\x80\x80\x80\x08", 6},
                    settings(CompressionAlgorithm::Snappy),
                    out
                ));
            },
            passed
        );
        test(
            "corrupt Snappy rejected",
            [] {
                NetworkCodec codec;
                std::string  out;
                require(!codec.decode(std::string{"\x01\x05\x10he", 5}, settings(CompressionAlgorithm::Snappy), out));
                require(out.empty());
            },
            passed
        );
        test(
            "raw hybrid obeys decompressed cap",
            [] {
                NetworkCodec codec;
                std::string  out;
                auto         option        = settings();
                option.maxDecompressedSize = 2;
                require(!codec.decode(
                    std::string{
                        "\xFF"
                        "abc",
                        4
                    },
                    option,
                    out
                ));
                require(codec.decode(
                    std::string{
                        "\xFF"
                        "ab",
                        3
                    },
                    option,
                    out
                ));
                require(out == "ab");
            },
            passed
        );
        test(
            "none negotiation roundtrip",
            [] {
                roundTrip("none payload", settings(CompressionAlgorithm::None, false));
                roundTrip("none hybrid payload", settings(CompressionAlgorithm::None, true));
            },
            passed
        );
        test(
            "valid empty DEFLATE at zero output limit",
            [] {
                auto option                = settings(CompressionAlgorithm::Deflate, false);
                option.maxDecompressedSize = 0;
                roundTrip({}, option, false);
            },
            passed
        );
        test(
            "multiple batch packets and zero length",
            [] {
                std::string batch;
                require(appendBatchPacket(batch, "one"));
                require(appendBatchPacket(batch, {}));
                require(appendBatchPacket(batch, std::string(128, 'b')));
                std::size_t position{};
                require(readBatchPacket(batch, position) == "one");
                require(readBatchPacket(batch, position) == "");
                require(readBatchPacket(batch, position) == std::string(128, 'b'));
                require(position == batch.size());
            },
            passed
        );
        test(
            "truncated length does not advance",
            [] {
                std::size_t position{};
                require(!readBatchPacket(std::string{"\x80", 1}, position));
                require(position == 0);
            },
            passed
        );
        test(
            "overflow varuint32 rejected",
            [] {
                std::size_t position{};
                require(!readBatchPacket(std::string{"\xFF\xFF\xFF\xFF\x10", 5}, position));
                require(position == 0);
            },
            passed
        );
        test(
            "truncated packet does not advance",
            [] {
                std::size_t position{};
                require(!readBatchPacket(
                    std::string{
                        "\x05"
                        "abc",
                        4
                    },
                    position
                ));
                require(position == 0);
            },
            passed
        );
        test(
            "out of range cursor rejected",
            [] {
                std::size_t position = 4;
                require(!readBatchPacket("abc", position));
                require(position == 4);
            },
            passed
        );
        test(
            "compressibility boundary matches BDS batch ratio",
            [] {
                require(!batchIsCompressible(0, 0));
                require(!batchIsCompressible(20, 100));
                require(batchIsCompressible(21, 100));
            },
            passed
        );
        test(
            "nested scratch preserves outer data and address",
            [] {
                SendScratch outer;
                outer.buffer()                                    = "outer";
                auto*                                     address = &outer.buffer();
                std::vector<std::unique_ptr<SendScratch>> nested;
                for (unsigned i = 0; i < 128; ++i) {
                    nested.push_back(std::make_unique<SendScratch>());
                    nested.back()->buffer() = std::string(4096, 'x');
                }
                require(&outer.buffer() == address);
                require(outer.buffer() == "outer");
                while (!nested.empty()) nested.pop_back();
            },
            passed
        );
        test(
            "scratch is isolated between concurrent threads",
            [] {
                std::atomic<unsigned> ready{};
                std::array<bool, 2>   ok{};
                auto                  run = [&](unsigned index) {
                    SendScratch lease;
                    auto        expected = std::string(4096, static_cast<char>('a' + index));
                    lease.buffer()       = expected;
                    ready.fetch_add(1);
                    while (ready.load() != 2) std::this_thread::yield();
                    ok[index] = lease.buffer() == expected;
                };
                std::thread one(run, 0), two(run, 1);
                one.join();
                two.join();
                require(ok[0] && ok[1]);
            },
            passed
        );
        test(
            "peer count follows strong ownership",
            [] {
                PeerLifetimeCounter counter;
                auto                peer = counter.make<int>(42);
                require(counter.count() == 1 && *peer == 42);
                auto copy = peer;
                peer.reset();
                require(counter.count() == 1);
                copy.reset();
                require(counter.count() == 0);
            },
            passed
        );
        test(
            "expired weak ownership still blocks unload",
            [] {
                PeerLifetimeCounter counter;
                auto                peer = counter.make<int>(42);
                std::weak_ptr<int>  weak = peer;
                peer.reset();
                require(weak.expired());
                require(counter.count() == 1);
                weak.reset();
                require(counter.count() == 0);
            },
            passed
        );
        test(
            "failed peer construction releases lifetime token",
            [] {
                struct Failing {
                    Failing() { throw std::runtime_error("construction failed"); }
                };
                PeerLifetimeCounter counter;
                try {
                    counter.make<Failing>();
                    require(false);
                } catch (std::runtime_error const&) {
                    require(counter.count() == 0);
                }
            },
            passed
        );
    } catch (std::exception const& error) {
        std::cerr << "FAIL after " << passed << " tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << passed << '/' << passed << " tests passed\n";
}
