// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Adapted from Endstone 03be20b63 / 6790acf3c. Shared registrations and queued
// function tokens outlive option instances and pin the DLL unload gate.

#include "features/features.h"
#include "FullFrameTiming.h"
#include "LeviOptimize.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "ll/api/memory/Hook.h"
#include "ll/api/memory/Symbol.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/thread/GlobalThreadPauser.h"
#include "mc/server/ServerInstance.h"
#include "mc/server/ServerInstanceInitArguments.h"
#include "mc/server/server_initialization/ServerInitResult.h"
#include "mc/world/events/ServerInstanceEventCoordinator.h"
#include "mc/world/events/ServerInstanceEventListener.h"
#include "mc/world/level/Level.h"

namespace lo::full_frame_timing {
namespace {
constexpr char ServerDtorSymbol[] = "??1ServerInstance@@UEAA@XZ";
constexpr char QueueSymbol[]      = "?queueForServerThread@ServerInstance@@QEAAXV?$function@$$A6AXXZ@std@@@Z";

std::mutex                   timingMutex;
FullFrameTimingState         timing;
std::atomic_bool             enabled{};
std::atomic_uint64_t         requestedGeneration{};
std::atomic<ServerInstance*> primaryOwner{};
std::atomic_size_t           activeCallbacks{}, taskTokens{};
thread_local ServerInstance* frameOwner{};

struct Registration;
std::recursive_mutex                       lifecycleMutex;
std::vector<std::shared_ptr<Registration>> registrations;
std::uint64_t                              nextGeneration{};

struct CallbackGuard {
    std::shared_ptr<Registration> keepAlive;
    explicit CallbackGuard(Registration* registration = nullptr);
    ~CallbackGuard();
};

struct Registration final : ServerInstanceEventListener, std::enable_shared_from_this<Registration> {
    Bedrock::NonOwnerPointer<ServerInstance> instance;
    ServerInstance*                          identity;
    std::uint64_t                            generation;
    ServerInstanceEventCoordinator*          coordinator{};
    std::atomic_bool                         active{true};
    std::atomic_size_t                       callbacks{};
    bool                                     attached{}, queued{}, dying{};

    Registration(ServerInstance& value, std::uint64_t epoch)
    : instance(value.mControlBlock, &value),
      identity(&value),
      generation(epoch) {}

    bool collecting(ServerInstance& value) const noexcept {
        return identity == &value && active.load(std::memory_order_acquire) && enabled.load(std::memory_order_acquire)
            && requestedGeneration.load(std::memory_order_acquire) == generation
            && primaryOwner.load(std::memory_order_acquire) == identity;
    }
    EventResult onServerInitializeStart(ServerInstance&) override {
        CallbackGuard guard(this);
        return EventResult::KeepGoing;
    }
    EventResult onServerInitializeEnd(ServerInstance&) override {
        CallbackGuard guard(this);
        return EventResult::KeepGoing;
    }
    EventResult onServerMinecraftInitialized(ServerInstance&, Bedrock::NotNullNonOwnerPtr<Minecraft> const&) override {
        CallbackGuard guard(this);
        return EventResult::KeepGoing;
    }
    EventResult onServerLevelInitialized(ServerInstance&, Level&) override {
        CallbackGuard guard(this);
        return EventResult::KeepGoing;
    }
    EventResult onServerUpdateStart(ServerInstance& value) override {
        CallbackGuard guard(this);
        if (collecting(value)) {
            frameOwner = &value;
            std::lock_guard lock(timingMutex);
            timing.beginFrame(FullFrameTimingState::Clock::now());
        }
        return EventResult::KeepGoing;
    }
    EventResult onServerUpdateEnd(ServerInstance& value) override {
        CallbackGuard guard(this);
        if (collecting(value) && frameOwner == &value) {
            std::lock_guard lock(timingMutex);
            timing.endFrame(FullFrameTimingState::Clock::now());
            frameOwner = nullptr;
        }
        return EventResult::KeepGoing;
    }
    EventResult onServerSuspend(ServerInstance& value) override {
        CallbackGuard guard(this);
        if (collecting(value)) {
            std::lock_guard lock(timingMutex);
            timing.cancelFrame();
            if (frameOwner == &value) frameOwner = nullptr;
        }
        return EventResult::KeepGoing;
    }
    EventResult onServerResume(ServerInstance&) override {
        CallbackGuard guard(this);
        return EventResult::KeepGoing;
    }
    EventResult onServerThreadStarted(ServerInstance& value) override {
        CallbackGuard guard(this);
        if (collecting(value)) {
            std::lock_guard lock(timingMutex);
            timing.reset();
        }
        return EventResult::KeepGoing;
    }
    EventResult onServerThreadStopped(ServerInstance& value) override {
        CallbackGuard guard(this);
        if (collecting(value)) {
            std::lock_guard lock(timingMutex);
            timing.reset();
            if (frameOwner == &value) frameOwner = nullptr;
        }
        return EventResult::KeepGoing;
    }
    EventResult onStartLeaveGame(ServerInstance&) override {
        CallbackGuard guard(this);
        return EventResult::KeepGoing;
    }
    EventResult onEvent(ServerInstanceNotificationEvent const&) override {
        CallbackGuard guard(this);
        return EventResult::KeepGoing;
    }
};

CallbackGuard::CallbackGuard(Registration* registration) {
    activeCallbacks.fetch_add(1, std::memory_order_acq_rel);
    if (registration) {
        keepAlive = registration->shared_from_this();
        registration->callbacks.fetch_add(1, std::memory_order_acq_rel);
    }
}
CallbackGuard::~CallbackGuard() {
    if (keepAlive) keepAlive->callbacks.fetch_sub(1, std::memory_order_acq_rel);
    keepAlive.reset();
    activeCallbacks.fetch_sub(1, std::memory_order_acq_rel);
}

// Copies of BDS's std::function share one token. Even an already executed task
// prevents DLL unloading until BDS destroys its retained function object.
struct TaskToken {
    TaskToken() { taskTokens.fetch_add(1, std::memory_order_acq_rel); }
    ~TaskToken() { taskTokens.fetch_sub(1, std::memory_order_acq_rel); }
};

void forgetLocked(std::shared_ptr<Registration> const& registration) { std::erase(registrations, registration); }

bool detachAtBoundaryLocked(std::shared_ptr<Registration> const& registration) {
    if (!registration->attached) {
        forgetLocked(registration);
        return true;
    }
    if (registration->dying || !registration->instance.get()) return false;
    auto* coordinator = registration->coordinator;
    if (!coordinator || !coordinator->mEventsToProcess.empty()
        || registration->callbacks.load(std::memory_order_acquire) != 0)
        return false;
    std::erase(coordinator->mListeners, registration.get());
    std::erase(coordinator->mPendingRegistrations, registration.get());
    if (coordinator->mPendingRegistrations.empty()) coordinator->mHasPendingRegistrations = false;
    registration->attached    = false;
    registration->coordinator = nullptr;
    forgetLocked(registration);
    return true;
}

void queueBoundary(std::shared_ptr<Registration> const& registration) noexcept {
    try {
        Bedrock::NonOwnerPointer<ServerInstance> instance;
        {
            std::lock_guard lock(lifecycleMutex);
            if (registration->queued || registration->dying) return;
            instance = registration->instance;
            if (!instance) {
                // Retain an attached unknown coordinator and refuse unloading.
                if (!registration->attached) forgetLocked(registration);
                return;
            }
            registration->queued = true;
        }
        auto token = std::make_shared<TaskToken>();
        // Serialize the native call with the pre-destructor hook. NonOwnerPointer
        // alone does not extend the instance's lifetime. Recursion also permits
        // SDKs that execute a same-thread queue request immediately.
        std::lock_guard nativeAccess(lifecycleMutex);
        if (registration->dying || !instance.get()) {
            registration->queued = false;
            return;
        }
        instance->queueForServerThread([registration, token] {
            CallbackGuard   callback;
            std::lock_guard lock(lifecycleMutex);
            registration->queued = false;
            if (registration->dying) return;
            auto current = ll::service::getServerInstance();
            bool wanted  = registration->active.load(std::memory_order_acquire)
                        && requestedGeneration.load(std::memory_order_acquire) == registration->generation && current
                        && &*current == registration->identity;
            if (!wanted) {
                registration->active.store(false, std::memory_order_release);
                detachAtBoundaryLocked(registration);
                return;
            }
            if (registration->attached) return;
            auto* live = registration->instance.get();
            if (!live) {
                forgetLocked(registration);
                return;
            }
            auto* coordinator = live->mEventCoordinator.get().get();
            if (!coordinator) return;
            // Native registration uses its pending queue during event dispatch.
            // The shared record remains resident until both native lists clear.
            if (coordinator->registerListener(gsl::not_null<ServerInstanceEventListener*>{registration.get()})) {
                registration->coordinator = coordinator;
                registration->attached    = true;
                primaryOwner.store(live, std::memory_order_release);
            }
        });
    } catch (...) {
        // Allocation/queue failures retain the record for the next unload retry
        // or the owning instance's destructor. No native pointer is freed here.
        std::lock_guard lock(lifecycleMutex);
        registration->queued = false;
    }
}

void attachCurrent(ServerInstance* candidate = nullptr) {
    auto current    = ll::service::getServerInstance();
    auto generation = requestedGeneration.load(std::memory_order_acquire);
    if (!generation || !current || (candidate && candidate != &*current)) return;
    std::vector<std::shared_ptr<Registration>> retired;
    std::shared_ptr<Registration>              registration;
    {
        std::lock_guard lock(lifecycleMutex);
        for (auto const& existing : registrations) {
            if (existing->generation != generation || !existing->active.load()) continue;
            if (existing->identity == &*current) return;
            existing->active.store(false, std::memory_order_release);
            retired.push_back(existing);
        }
        primaryOwner.store(nullptr, std::memory_order_release);
        registration = std::make_shared<Registration>(*current, generation);
        registrations.push_back(registration);
    }
    for (auto const& old : retired) queueBoundary(old);
    queueBoundary(registration);
}

void retireGeneration(std::uint64_t generation) noexcept {
    try {
        std::vector<std::shared_ptr<Registration>> retired;
        {
            std::lock_guard lock(lifecycleMutex);
            for (auto const& registration : registrations) {
                if (registration->generation == generation)
                    registration->active.store(false, std::memory_order_release);
            }
            for (auto const& registration : registrations) {
                if (registration->generation == generation) retired.push_back(registration);
            }
        }
        for (auto const& registration : retired) queueBoundary(registration);
    } catch (...) {
        // Inactive shared records pin unload until cleanup can be retried.
    }
}

void beforeServerDestroyed(ServerInstance& instance) {
    std::lock_guard lock(lifecycleMutex);
    for (auto const& registration : registrations) {
        if (registration->identity != &instance || registration->dying) continue;
        registration->active.store(false, std::memory_order_release);
        auto* coordinator = registration->coordinator;
        if (registration->attached && coordinator && coordinator->mEventsToProcess.empty()
            && registration->callbacks.load(std::memory_order_acquire) == 0) {
            std::erase(coordinator->mListeners, registration.get());
            std::erase(coordinator->mPendingRegistrations, registration.get());
            if (coordinator->mPendingRegistrations.empty()) coordinator->mHasPendingRegistrations = false;
            registration->attached = false;
        }
        // Never invalidate a native dispatch iterator. The coordinator is a
        // non-owning member: instance destruction alone cannot prove that this
        // listener pointer disappeared from a still-dispatching external owner.
        registration->dying = true;
    }
    if (primaryOwner.load() == &instance) primaryOwner.store(nullptr, std::memory_order_release);
}

void afterServerDestroyed(ServerInstance* identity) {
    std::lock_guard lock(lifecycleMutex);
    std::erase_if(registrations, [&](auto const& registration) {
        if (registration->identity != identity) return false;
        // If shutdown was nested in event dispatch, retain the inactive listener
        // and refuse DLL unload rather than assume the external coordinator died.
        // Its dying flag prevents any future dereference of the coordinator.
        if (registration->attached) return false;
        registration->coordinator = nullptr;
        return true;
    });
}

LL_TYPE_INSTANCE_HOOK(
    TimingInitializeHook,
    ll::memory::HookPriority::Normal,
    ServerInstance,
    &ServerInstance::initializeServer,
    ServerInitialization::ServerInitResult,
    ServerInstanceInitArguments&& args
) {
    CallbackGuard callback;
    auto          result = origin(std::move(args));
    try {
        attachCurrent(this);
    } catch (...) {
        getLogger().warn("optFullFrameTiming: could not queue timing registration; native initialization is retained.");
    }
    return result;
}

LL_TYPE_INSTANCE_HOOK(
    TimingServerDtorHook,
    ll::memory::HookPriority::Normal,
    ServerInstance,
    ll::memory::SymbolView{ServerDtorSymbol},
    void
) {
    CallbackGuard callback;
    beforeServerDestroyed(*this);
    auto* identity = static_cast<ServerInstance*>(this);
    origin();
    afterServerDestroyed(identity);
    // Trampolines are released only by canUnload after every callback has left.
}

LL_TYPE_INSTANCE_HOOK(TimingLevelTickHook, ll::memory::HookPriority::Highest, Level, &Level::$tick, void) {
    CallbackGuard            callback;
    static thread_local bool insideTick{};
    if (!enabled.load(std::memory_order_acquire) || insideTick || !frameOwner
        || frameOwner != primaryOwner.load(std::memory_order_acquire)) {
        origin();
        return;
    }
    auto currentLevel = ll::service::getLevel();
    if (!currentLevel || &*currentLevel != this) {
        origin();
        return;
    }
    struct TickScope {
        bool& flag;
        explicit TickScope(bool& value) : flag(value) { flag = true; }
        ~TickScope() { flag = false; }
    } scope(insideTick);
    {
        std::lock_guard lock(timingMutex);
        timing.beginTick(FullFrameTimingState::Clock::now());
    }
    try {
        origin();
    } catch (...) {
        std::lock_guard lock(timingMutex);
        timing.cancelFrame();
        throw;
    }
    std::lock_guard lock(timingMutex);
    timing.completeTick();
}

using TimingHooks = ll::memory::HookRegistrar<TimingInitializeHook, TimingServerDtorHook, TimingLevelTickHook>;
std::optional<TimingHooks> residentHooks;

bool ensureHooks() {
    std::lock_guard lock(lifecycleMutex);
    if (residentHooks) return true;
    if (!ll::memory::SymbolView(ServerDtorSymbol).resolve(true) || !ll::memory::SymbolView(QueueSymbol).resolve(true)) {
        getLogger().warn("optFullFrameTiming: destructor/queue symbol missing; no timing hooks were installed.");
        return false;
    }
    residentHooks.emplace();
    return true;
}
} // namespace

struct FullFrameTimingOpt::Impl {
    std::uint64_t generation{};
    Impl() {
        if (!ensureHooks()) return;
        {
            std::lock_guard lock(lifecycleMutex);
            generation = ++nextGeneration;
        }
        requestedGeneration.store(generation, std::memory_order_release);
        {
            std::lock_guard lock(timingMutex);
            timing.reset();
        }
        enabled.store(true, std::memory_order_release);
        try {
            attachCurrent();
        } catch (...) {
            auto expected = generation;
            if (requestedGeneration.compare_exchange_strong(expected, 0)) {
                enabled.store(false, std::memory_order_release);
                primaryOwner.store(nullptr, std::memory_order_release);
            }
            retireGeneration(generation);
            generation = 0;
            getLogger().warn(
                "optFullFrameTiming: registration failed; timing is inactive and cleanup remains resident."
            );
        }
    }
    ~Impl() {
        if (!generation) return;
        auto expected = generation;
        if (requestedGeneration.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
            enabled.store(false, std::memory_order_release);
            primaryOwner.store(nullptr, std::memory_order_release);
            std::lock_guard lock(timingMutex);
            timing.reset();
        }
        retireGeneration(generation);
    }
};

bool           isEnabled() noexcept { return enabled.load(std::memory_order_acquire); }
TimingSnapshot getSnapshot() {
    std::lock_guard lock(timingMutex);
    return timing.snapshot();
}

bool canUnload() noexcept {
    try {
        if (requestedGeneration.load(std::memory_order_acquire) || enabled.load(std::memory_order_acquire))
            return false;
        std::vector<std::shared_ptr<Registration>> pending;
        {
            std::lock_guard lock(lifecycleMutex);
            pending = registrations;
        }
        for (auto const& registration : pending) queueBoundary(registration);
        pending.clear();
        std::lock_guard lock(lifecycleMutex);
        if (!registrations.empty() || activeCallbacks.load(std::memory_order_acquire)
            || taskTokens.load(std::memory_order_acquire))
            return false;
        if (!residentHooks) return true;
        // Freeze new entries, then check again. HookRegistrar's nested pause is
        // balanced, and no active detour can unhook its own trampoline.
        std::optional<ll::thread::GlobalThreadPauser> paused;
        if (ll::memory::shouldHookSuspendThreads()) paused.emplace();
        if (activeCallbacks.load(std::memory_order_acquire) || taskTokens.load(std::memory_order_acquire)) return false;
        residentHooks.reset();
        return true;
    } catch (...) {
        return false;
    }
}

void FullFrameTimingOpt::call(bool enable) {
    if (enable) {
        if (!impl) impl = std::make_unique<Impl>();
    } else {
        impl.reset();
    }
}
FullFrameTimingOpt::FullFrameTimingOpt()                                         = default;
FullFrameTimingOpt::~FullFrameTimingOpt()                                        = default;
FullFrameTimingOpt::FullFrameTimingOpt(FullFrameTimingOpt&&) noexcept            = default;
FullFrameTimingOpt& FullFrameTimingOpt::operator=(FullFrameTimingOpt&&) noexcept = default;
} // namespace lo::full_frame_timing
