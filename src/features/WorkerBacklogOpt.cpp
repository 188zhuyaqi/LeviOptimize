// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Adapted from Endstone cd97eea16; adds expected-value restoration and an ABI adapter.

#include "features.h"
#include "LeviOptimize.h"
#include "WorkerBacklogAdapter.h"
#include "WorkerBacklogState.h"

#include "ll/api/event/EventBus.h"
#include "ll/api/event/server/ServerStartedEvent.h"
#include "ll/api/memory/Memory.h"

namespace lo::worker_backlog_opt {

struct WorkerBacklogOpt::Impl {
    Bedrock::NonOwnerPointer<WorkerPool> pool;
    detail::BacklogSetting               setting;
    ll::event::ListenerPtr               started;

    void apply() {
        auto* singleton = adapter::resolveAsync();
        if (!singleton || !*singleton) return;
        if (pool) return;
        auto& field = adapter::minBacklog(**singleton);
        bool  changed{};
        ll::memory::modify(&field, sizeof(field), [&] { changed = setting.install(field); });
        if (changed) {
            pool = *singleton;
        } else {
            getLogger().warn("optWorkerBacklog: expected 0.05F was not found; leaving worker scheduling unchanged.");
        }
    }

    Impl() {
        if (!adapter::resolveAsync()) {
            getLogger().warn("optWorkerBacklog: ASYNC data symbol is missing; check WorkerBacklogAdapter.h; skipped.");
            return;
        }
        apply();
        started = ll::event::EventBus::getInstance().emplaceListener<ll::event::ServerStartedEvent>(
            [this](ll::event::ServerStartedEvent&) { apply(); }
        );
    }

    ~Impl() {
        if (started) ll::event::EventBus::getInstance().removeListener(started);
        if (auto* live = pool.get()) {
            auto& field = adapter::minBacklog(*live);
            ll::memory::modify(&field, sizeof(field), [&] { setting.restore(field); });
        }
    }
};

void WorkerBacklogOpt::call(bool enable) {
    if (enable) {
        if (!impl) impl = std::make_unique<Impl>();
    } else {
        impl.reset();
    }
}

WorkerBacklogOpt::WorkerBacklogOpt()                                       = default;
WorkerBacklogOpt::~WorkerBacklogOpt()                                      = default;
WorkerBacklogOpt::WorkerBacklogOpt(WorkerBacklogOpt&&) noexcept            = default;
WorkerBacklogOpt& WorkerBacklogOpt::operator=(WorkerBacklogOpt&&) noexcept = default;

} // namespace lo::worker_backlog_opt
