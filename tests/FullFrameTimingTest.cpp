#include "features/timing/FullFrameTimingState.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using State = lo::full_frame_timing::FullFrameTimingState;
auto at(int milliseconds) { return State::TimePoint{std::chrono::milliseconds{milliseconds}}; }
void require(bool result, char const* expression, int line) {
    if (!result) throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
}
#define REQUIRE(...) require((__VA_ARGS__), #__VA_ARGS__, __LINE__)
bool near(double actual, double expected) { return std::fabs(actual - expected) < 0.000001; }

void includesNetworkBeforeAndAfterLevelTick() {
    State state;
    state.beginFrame(at(0));
    state.beginTick(at(15));
    state.completeTick();
    state.endFrame(at(80));
    auto stats = state.snapshot();
    REQUIRE(stats.samples == 1);
    REQUIRE(near(stats.currentMspt, 80));
    REQUIRE(near(stats.currentTps, 12.5));
    REQUIRE(near(stats.averageMspt, 80));
}
void splitsCatchUpFrameAtNextTickStart() {
    State state;
    state.beginFrame(at(0));
    state.beginTick(at(30));
    state.completeTick();
    state.beginTick(at(55));
    REQUIRE(near(state.snapshot().currentMspt, 55));
    state.completeTick();
    state.beginTick(at(90));
    REQUIRE(near(state.snapshot().currentMspt, 35));
    state.completeTick();
    state.endFrame(at(150));
    auto stats = state.snapshot();
    REQUIRE(stats.samples == 3);
    REQUIRE(near(stats.currentMspt, 60));
    REQUIRE(near(stats.averageMspt, 50));
}
void doesNotCountInterFrameIdleTime() {
    State state;
    for (int start : {0, 1000}) {
        state.beginFrame(at(start));
        state.beginTick(at(start + 5));
        state.completeTick();
        state.endFrame(at(start + 10));
    }
    REQUIRE(state.snapshot().samples == 2);
    REQUIRE(near(state.snapshot().averageMspt, 10));
}
void frameWithoutLevelTickProducesNoSample() {
    State state;
    state.beginFrame(at(0));
    state.endFrame(at(200));
    REQUIRE(state.snapshot().samples == 0);
}
void tickOutsideFrameProducesNoSample() {
    State state;
    state.beginTick(at(0));
    state.completeTick();
    state.endFrame(at(200));
    REQUIRE(state.snapshot().samples == 0);
}
void abortedFrameIsDiscarded() {
    State state;
    state.beginFrame(at(0));
    state.beginTick(at(1));
    state.cancelFrame();
    state.completeTick();
    state.endFrame(at(900));
    REQUIRE(state.snapshot().samples == 0);
    state.beginFrame(at(1000));
    state.beginTick(at(1001));
    state.completeTick();
    state.endFrame(at(1020));
    REQUIRE(state.snapshot().samples == 1);
    REQUIRE(near(state.snapshot().currentMspt, 20));
}
void freshFrameDiscardsAnUnclosedFrame() {
    State state;
    state.beginFrame(at(0));
    state.beginTick(at(1));
    state.completeTick();
    state.beginFrame(at(1000));
    state.beginTick(at(1001));
    state.completeTick();
    state.endFrame(at(1030));
    REQUIRE(state.snapshot().samples == 1);
    REQUIRE(near(state.snapshot().currentMspt, 30));
}
void duplicateEndDoesNotDuplicateSample() {
    State state;
    state.beginFrame(at(0));
    state.beginTick(at(1));
    state.completeTick();
    state.endFrame(at(20));
    state.endFrame(at(2000));
    REQUIRE(state.snapshot().samples == 1);
    REQUIRE(near(state.snapshot().currentMspt, 20));
}
void ringWindowRetiresOldSamples() {
    State state;
    for (int duration = 1; duration <= 21; ++duration) {
        state.beginFrame(at(0));
        state.beginTick(at(0));
        state.completeTick();
        state.endFrame(at(duration));
    }
    REQUIRE(state.snapshot().samples == 20);
    REQUIRE(near(state.snapshot().currentMspt, 21));
    REQUIRE(near(state.snapshot().averageMspt, 11.5));
}
void tpsUsesEachSampleAndIsCappedAt20() {
    State state;
    for (int duration : {100, 200}) {
        state.beginFrame(at(0));
        state.beginTick(at(0));
        state.completeTick();
        state.endFrame(at(duration));
    }
    REQUIRE(near(state.snapshot().currentTps, 5));
    REQUIRE(near(state.snapshot().averageTps, 7.5));
    REQUIRE(near(State::ticksPerSecond(0), 20));
    REQUIRE(near(State::ticksPerSecond(1), 20));
    REQUIRE(near(State::ticksPerSecond(50), 20));
}
void resetDiscardsSamplesAndPendingFrame() {
    State state;
    state.beginFrame(at(0));
    state.completeTick();
    state.endFrame(at(100));
    state.beginFrame(at(200));
    state.completeTick();
    state.reset();
    state.endFrame(at(2000));
    auto stats = state.snapshot();
    REQUIRE(stats.samples == 0);
    REQUIRE(stats.currentMspt == 0);
    REQUIRE(stats.currentTps == 0);
}
void rejectsBackwardClockSample() {
    State state;
    state.beginFrame(at(10));
    state.beginTick(at(11));
    state.completeTick();
    state.endFrame(at(0));
    REQUIRE(state.snapshot().samples == 0);
}
} // namespace

int main() {
    std::pair<char const*, void (*)()> const cases[]{
        {"full-frame boundaries",   includesNetworkBeforeAndAfterLevelTick},
        {"catch-up frame split",    splitsCatchUpFrameAtNextTickStart     },
        {"exclude idle time",       doesNotCountInterFrameIdleTime        },
        {"no tick in frame",        frameWithoutLevelTickProducesNoSample },
        {"tick outside frame",      tickOutsideFrameProducesNoSample      },
        {"abort recovery",          abortedFrameIsDiscarded               },
        {"unclosed frame recovery", freshFrameDiscardsAnUnclosedFrame     },
        {"duplicate end",           duplicateEndDoesNotDuplicateSample    },
        {"rolling window",          ringWindowRetiresOldSamples           },
        {"TPS formula",             tpsUsesEachSampleAndIsCappedAt20      },
        {"reset",                   resetDiscardsSamplesAndPendingFrame   },
        {"clock order",             rejectsBackwardClockSample            },
    };
    int failed{};
    for (auto const& [name, test] : cases) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (std::exception const& error) {
            ++failed;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << std::size(cases) - failed << '/' << std::size(cases) << " tests passed\n";
    return failed ? 1 : 0;
}
