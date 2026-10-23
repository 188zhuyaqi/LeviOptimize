#include "features/WorkerBacklogState.h"
#include "features/parked_chunks/ParkedQueue.h"
#include "features/random_tick/RandomTickSamples.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
void require(bool value, char const* expression, int line) {
    if (!value) throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
}
#define REQUIRE(...) require((__VA_ARGS__), #__VA_ARGS__, __LINE__)

using Queue  = std::unordered_map<int, std::weak_ptr<int>>;
using Parked = lo::parked_chunks_opt::detail::ParkedQueue<Queue>;
using State  = lo::parked_chunks_opt::detail::QueuedChunkState;

void nativeHashValidation() {
    Queue queue;
    auto  owner = std::make_shared<int>(1);
    for (int i = 0; i < 256; ++i) queue.emplace(i, owner);
    REQUIRE(lo::parked_chunks_opt::detail::checkQueueHash(queue));
    struct MutableHash {
        size_t const* salt;
        size_t        operator()(int value) const { return std::hash<int>{}(value) ^ *salt; }
    };
    size_t                                                   salt{};
    std::unordered_map<int, std::weak_ptr<int>, MutableHash> mismatched(0, MutableHash{&salt});
    for (int i = 0; i < 256; ++i) mismatched.emplace(i, owner);
    REQUIRE(lo::parked_chunks_opt::detail::checkQueueHash(mismatched));
    salt = 0x12345678;
    REQUIRE(!lo::parked_chunks_opt::detail::checkQueueHash(mismatched));
}

void parksOnlyUnsendableLiveMatchingKeys() {
    auto  owner = std::make_shared<int>(7);
    Queue queue{
        {1, owner},
        {2, owner},
        {3, owner},
        {4, owner},
        {5, {}   }
    };
    Parked state;
    state.parkUnsendable(
        queue,
        [](int key) { return key != 4; },
        [](int key) { return key == 1 ? State::Sendable
                           : key == 2 ? State::NotLoaded
                                      : State::OutOfView; }
    );
    REQUIRE(queue.size() == 3);
    REQUIRE(queue.contains(1) && queue.contains(4) && queue.contains(5));
    REQUIRE(state.parked.size() == 2);
    REQUIRE(state.parked.at(2).state == State::NotLoaded);
    REQUIRE(state.parked.at(3).state == State::OutOfView);
    REQUIRE(state.sendable.contains(1));
}

void loadAndViewRechecksReturnNodes() {
    auto  owner = std::make_shared<int>(7);
    Queue queue{
        {1, owner},
        {2, owner}
    };
    Parked state;
    state.parkUnsendable(
        queue,
        [](int) { return true; },
        [](int key) { return key == 1 ? State::NotLoaded : State::OutOfView; }
    );
    REQUIRE(queue.empty());
    state.recheck(queue, state.parked.find(1), [](int) { return State::Sendable; });
    REQUIRE(queue.contains(1) && !queue.contains(2));
    state.recheckAll(queue, [](int) { return State::Sendable; });
    REQUIRE(queue.size() == 2 && state.parked.empty());
}

void expiredParkedNodesDoNotReplaceNewNativeEntries() {
    auto  old     = std::make_shared<int>(1);
    auto  current = std::make_shared<int>(2);
    Queue queue{
        {1, old}
    };
    Parked state;
    state.parkUnsendable(queue, [](int) { return true; }, [](int) { return State::NotLoaded; });
    old.reset();
    queue.emplace(1, current);
    state.recheckAll(queue, [](int) { return State::Sendable; });
    REQUIRE(state.parked.empty());
    REQUIRE(queue.at(1).lock() == current);
}

void duplicateLiveEntriesFollowNativeMergeRule() {
    auto  original  = std::make_shared<int>(1);
    auto  duplicate = std::make_shared<int>(2);
    Queue queue{
        {1, original}
    };
    Parked state;
    state.parkUnsendable(queue, [](int) { return true; }, [](int) { return State::NotLoaded; });
    queue.emplace(1, duplicate);
    state.restore(queue);
    REQUIRE(queue.size() == 1);
    REQUIRE(queue.at(1).lock() == original);
    REQUIRE(state.parked.empty() && state.sendable.empty());
}

void disableRestoresNodesWithoutOwningChunks() {
    auto               owner    = std::make_shared<int>(1);
    std::weak_ptr<int> lifetime = owner;
    Queue              queue{
        {1, owner},
        {2, owner}
    };
    Parked state;
    state.parkUnsendable(queue, [](int) { return true; }, [](int) { return State::NotLoaded; });
    state.restore(queue);
    REQUIRE(queue.size() == 2);
    state.parkUnsendable(queue, [](int) { return true; }, [](int) { return State::NotLoaded; });
    owner.reset();
    REQUIRE(lifetime.expired());
    state.recheckAll(queue, [](int) { return State::Sendable; });
    REQUIRE(state.parked.empty() && queue.empty());
}

void retirementWaitsForNestedTraversalAndUnwind() {
    using lo::parked_chunks_opt::detail::ActiveTraversal;
    using lo::parked_chunks_opt::detail::TraversalState;
    auto                          state       = std::make_shared<TraversalState>();
    std::weak_ptr<TraversalState> lifetime    = state;
    auto                          activeOwner = state;
    try {
        ActiveTraversal outer(*activeOwner);
        {
            ActiveTraversal inner(*activeOwner);
            activeOwner->retire();
            state.reset(); // Removing a registry entry must not destroy the active state.
            REQUIRE(!lifetime.expired() && activeOwner->active == 2);
            REQUIRE(!activeOwner->readyToRestore() && !activeOwner->discard);
        }
        REQUIRE(activeOwner->active == 1 && !activeOwner->readyToRestore());
        throw std::runtime_error("native callback");
    } catch (std::runtime_error const&) {}
    REQUIRE(activeOwner->readyToRestore() && !activeOwner->discard);
    activeOwner.reset();
    REQUIRE(lifetime.expired());
}

void reentrantNativeClearCancelsBeforeReusingQueueIterator() {
    namespace detail = lo::parked_chunks_opt::detail;
    auto  owner      = std::make_shared<int>(1);
    Queue queue{
        {1, owner}
    };
    Parked parked;
    parked.parkUnsendable(queue, [](int) { return true; }, [](int) { return State::NotLoaded; });
    queue.emplace(2, owner);
    detail::TraversalState lifecycle;
    struct Cancelled {};
    try {
        detail::ActiveTraversal active(lifecycle);
        parked.parkUnsendable(
            queue,
            [](int) { return true; },
            [&](int) -> State {
                // clearRegion can run inside getExistingChunk. Its callback retires
                // the state without touching the caller's parked/native iterators.
                lifecycle.retire(true);
                queue.clear();
                if (lifecycle.retired) throw Cancelled{};
                return State::NotLoaded;
            }
        );
        REQUIRE(false);
    } catch (Cancelled const&) {}
    REQUIRE(lifecycle.readyToRestore() && lifecycle.discard);
    parked.parked.clear(); // Native clear makes the extracted nodes obsolete too.
    parked.sendable.clear();
    REQUIRE(queue.empty() && parked.parked.empty());
}

void reentrantDisableDuringRecheckRestoresAfterTraversal() {
    namespace detail = lo::parked_chunks_opt::detail;
    auto  owner      = std::make_shared<int>(1);
    Queue queue{
        {1, owner},
        {2, owner}
    };
    Parked parked;
    parked.parkUnsendable(queue, [](int) { return true; }, [](int) { return State::NotLoaded; });
    detail::TraversalState lifecycle;
    struct Cancelled {};
    try {
        detail::ActiveTraversal active(lifecycle);
        parked.recheckAll(queue, [&](int) -> State {
            lifecycle.retire();
            REQUIRE(!lifecycle.readyToRestore() && parked.parked.size() == 2);
            throw Cancelled{};
        });
        REQUIRE(false);
    } catch (Cancelled const&) {}
    REQUIRE(lifecycle.readyToRestore() && !lifecycle.discard);
    parked.restore(queue);
    REQUIRE(queue.size() == 2 && parked.parked.empty());
    REQUIRE(queue.at(1).lock() == owner && queue.at(2).lock() == owner);
}

void sendableCacheIsPrunedWhenNativeSends() {
    auto  owner = std::make_shared<int>(1);
    Queue queue{
        {1, owner}
    };
    Parked state;
    int    checks{};
    auto   resolve = [&](int) {
        ++checks;
        return State::Sendable;
    };
    state.parkUnsendable(queue, [](int) { return true; }, resolve);
    state.parkUnsendable(queue, [](int) { return true; }, resolve);
    REQUIRE(checks == 1);
    queue.clear();
    state.parkUnsendable(queue, [](int) { return true; }, resolve);
    REQUIRE(state.sendable.empty());
    queue.emplace(1, owner);
    state.parkUnsendable(queue, [](int) { return true; }, resolve);
    REQUIRE(checks == 2);
}

void journalPreservesEventsThenReportsMissedCursors() {
    lo::parked_chunks_opt::detail::LoadedJournal<int> journal;
    journal.advance(1);
    journal.append(10);
    journal.append(20);
    auto first = journal.since(0);
    REQUIRE(!first.missed && first.end == 2 && first.positions == std::vector<int>{10, 20});
    journal.advance(1);
    REQUIRE(journal.since(0).positions == first.positions);
    journal.advance(2);
    REQUIRE(!journal.since(0).missed);
    journal.append(30);
    journal.advance(3);
    REQUIRE(journal.since(0).missed);
    auto tail = journal.since(2);
    REQUIRE(!tail.missed && tail.positions == std::vector<int>{30});
    REQUIRE(journal.since(journal.end() + 1).missed);
}

void backlogExpectedValueAndRestoration() {
    lo::worker_backlog_opt::detail::BacklogSetting setting;
    float                                          value = 0.05F;
    REQUIRE(setting.install(value) && value == 0.0F);
    REQUIRE(!setting.install(value));
    REQUIRE(setting.restore(value) && value == 0.05F);
    REQUIRE(!setting.restore(value));
    value = 0.2F;
    REQUIRE(!setting.install(value) && value == 0.2F);
}

void eventJournalBoundsMemoryWhenNoPlayerConsumes() {
    lo::parked_chunks_opt::detail::LoadedJournal<int, 4> journal;
    for (int i = 0; i < 100; ++i) journal.append(i);
    REQUIRE(journal.since(0).missed);
    auto tail = journal.since(journal.end() - 1);
    REQUIRE(!tail.missed && tail.positions == std::vector<int>{99});
    journal.advance(1);
    journal.advance(2);
    REQUIRE(!journal.since(journal.end()).missed);
}

void backlogPreservesThirdPartyValues() {
    lo::worker_backlog_opt::detail::BacklogSetting setting;
    float                                          value = 0.05F;
    REQUIRE(setting.install(value));
    value = 0.0125F;
    REQUIRE(!setting.restore(value) && value == 0.0125F);
    value = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(!setting.install(value) && std::isnan(value));
}

void sampleSequenceMatchesPlatformSeedRules() {
    namespace random                     = lo::random_tick_prefetch_opt::detail;
    std::uint32_t                seed    = 1;
    auto                         windows = random::samples(seed, 0, 3);
    std::array<std::uint16_t, 6> expected{0x8CB, 0xFC1, 0xB68, 0xA15, 0xAD3, 0x5CF};
    REQUIRE(seed == 1 && windows.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        REQUIRE(windows[i].subChunk == 0 && windows[i].index == expected[i]);
    }
    auto linuxSamples = random::samples(seed, 0, 3, random::SeedPlatform::Linux64BitLong);
    REQUIRE(linuxSamples[1].index == 0x13E && linuxSamples[4].index == 0x62C);
}

void sampleBoundsAndDisabledRules() {
    namespace random = lo::random_tick_prefetch_opt::detail;
    REQUIRE(random::samples(123, -1, 3).size() == 0);
    REQUIRE(random::samples(123, 2, 0).size() == 0);
    REQUIRE(random::samples(123, 2, -1).size() == 0);
    REQUIRE(random::samples(123, std::numeric_limits<int>::max(), 3).size() == 0);
    auto bounded = random::samples(0xFFFFFFFF, 23, std::numeric_limits<int>::max());
    REQUIRE(bounded.size() == random::MaxWarmedSamples);
    for (size_t i = 0; i < bounded.size(); ++i) {
        REQUIRE(bounded[i].subChunk < 24 && bounded[i].index < 4096);
    }
}
} // namespace

int main() {
    std::pair<char const*, void (*)()> const tests[]{
        {"native queue hash validation",            nativeHashValidation                                 },
        {"park unsendable matching keys",           parksOnlyUnsendableLiveMatchingKeys                  },
        {"load and view recheck",                   loadAndViewRechecksReturnNodes                       },
        {"expired node merge",                      expiredParkedNodesDoNotReplaceNewNativeEntries       },
        {"duplicate native node merge",             duplicateLiveEntriesFollowNativeMergeRule            },
        {"restore and weak ownership",              disableRestoresNodesWithoutOwningChunks              },
        {"nested traversal retirement and unwind",  retirementWaitsForNestedTraversalAndUnwind           },
        {"native clear cancels active iterator",    reentrantNativeClearCancelsBeforeReusingQueueIterator},
        {"disable cancels then restores recheck",   reentrantDisableDuringRecheckRestoresAfterTraversal  },
        {"sendable cache invalidation",             sendableCacheIsPrunedWhenNativeSends                 },
        {"loaded event journal",                    journalPreservesEventsThenReportsMissedCursors       },
        {"bounded event journal without consumers", eventJournalBoundsMemoryWhenNoPlayerConsumes         },
        {"backlog expected value and restore",      backlogExpectedValueAndRestoration                   },
        {"backlog foreign changes",                 backlogPreservesThirdPartyValues                     },
        {"random tick sample sequence",             sampleSequenceMatchesPlatformSeedRules               },
        {"random tick bounds and disable",          sampleBoundsAndDisabledRules                         },
    };
    size_t failed{};
    for (auto [name, run] : tests) {
        try {
            run();
            std::cout << "PASS " << name << '\n';
        } catch (std::exception const& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << std::size(tests) - failed << '/' << std::size(tests) << " tests passed\n";
    return failed ? 1 : 0;
}
