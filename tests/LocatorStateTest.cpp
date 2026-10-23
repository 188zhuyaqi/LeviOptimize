#include "features/locator/LocatorState.h"
#include <iostream>
#include <optional>
#include <stdexcept>
#include <vector>

using namespace lo::locator_opt::detail;
void require(bool value) {
    if (!value) throw std::runtime_error("locator state check failed");
}
int main() {
    require(classify("lambda_checkPlayerPairsAndMaybeSendPackets_1") == PairAction::Check);
    require(classify("lambda_sendPacketsHidingAllPlayers_2") == PairAction::Hide);
    require(classify("unrecognized callback") == PairAction::Other);
    require(textureMatches(10, 10, 20));
    require(!textureMatches(20, 10, 20));
    require(textureMatches(20, 10, std::nullopt));
    auto nan = std::numeric_limits<float>::quiet_NaN();
    require(!textureMatches(nan, 0, std::nullopt));
    require(!iconChanged(1, 2, 1, 2));
    require(!iconChanged(std::numeric_limits<float>::epsilon(), 0, 0, 0));
    require(iconChanged(2 * std::numeric_limits<float>::epsilon(), 0, 0, 0));
    require(iconChanged(nan, 0, 0, 0));
    SentPositions<int> positions;
    auto               a = positions.slot(1), b = positions.slot(2);
    positions.at(a, b) = {17, SentState::Position};
    positions.at(b, a) = {0, SentState::Nullopt};
    for (int id = 3; id < 300; ++id) (void)positions.slot(id);
    require(positions.at(a, b).position == 17);
    require(positions.at(b, a).state == SentState::Nullopt);
    positions.remove(2);
    auto replacement = positions.slot(-9);
    require(replacement == b);
    require(positions.at(a, replacement).state == SentState::Absent);
    require(positions.at(replacement, a).state == SentState::Absent);
    positions.clear();
    require(positions.slot(1) == 0);

    std::vector<int> keys{10, 20}, values{100, 200};
    try {
        VectorWindow window(keys, values, std::vector<int>{7}, std::vector<int>{70});
        require(keys == std::vector<int>{7} && values == std::vector<int>{70});
        window.suspend();
        require(keys == std::vector<int>({10, 20}));
        window.suspend();
        window.resume();
        require(keys == std::vector<int>({10, 20}));
        keys.push_back(30);
        values.push_back(300);
        window.resume();
        require(keys == std::vector<int>{7});
        throw 1;
    } catch (int) {}
    require(keys == std::vector<int>({10, 20, 30}));
    require(values == std::vector<int>({100, 200, 300}));
    {
        VectorWindow window(keys, values);
        require(keys.empty() && values.empty());
        window.clearReal();
    }
    require(keys.empty() && values.empty());
    {
        VectorWindow window(keys, values, std::vector<int>{1}, std::vector<int>{2});
        window.abandon();
    }
    require(keys == std::vector<int>{1} && values == std::vector<int>{2});
    unsigned invalidations{};
    {
        InvalidateOnException guard([&]() noexcept { ++invalidations; });
    }
    require(invalidations == 0);
    try {
        InvalidateOnException guard([&]() noexcept { ++invalidations; });
        throw std::runtime_error("native packet action failed");
    } catch (std::runtime_error const&) {}
    require(invalidations == 1);
    {
        InvalidateOnException outer([&]() noexcept { invalidations += 100; });
        try {
            InvalidateOnException inner([&]() noexcept { ++invalidations; });
            throw 1;
        } catch (int) {}
    }
    require(invalidations == 2);
    std::cout << "PASS matrix growth, slot retirement/reuse, clear, window resume/unwind/retirement, exception "
                 "invalidation\n";
}
