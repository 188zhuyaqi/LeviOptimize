// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Adapted from Endstone's WorkerPool layout and cd97eea16.
#pragma once

#include <cstddef>
#include <string_view>

#include "ll/api/memory/Symbol.h"
#include "mc/deps/core/threading/WorkerPool.h"
#include "mc/deps/core/utility/NonOwnerPointer.h"

namespace lo::worker_backlog_opt::adapter {

// Developer porting point: this is BDS's data symbol, not the SDK's synthetic
// MinecraftWorkerPool::ASYNC() accessor. No Endstone absolute address is used.
inline constexpr std::string_view AsyncSymbol =
    "?ASYNC@MinecraftWorkerPool@@2V?$NonOwnerPointer@VWorkerPool@@@Bedrock@@A";

inline Bedrock::NonOwnerPointer<WorkerPool>* resolveAsync() {
    return reinterpret_cast<Bedrock::NonOwnerPointer<WorkerPool>*>(ll::memory::SymbolView(AsyncSymbol).resolve(true));
}

// TODO(target ABI): replace this adapter with the named SDK member when it is
// published. Endstone main bb0475b64 identifies the float immediately after
// pool_priority_ as min_backlog_duration_seconds_. LL 26.51.7 describes that slot
// as mUnk853032. Its Windows x64 offset is 252; the observed 0.05F guard remains
// mandatory. This assumption requires a target-binary runtime smoke test.
inline float& minBacklog(WorkerPool& pool) {
    static_assert(offsetof(WorkerPool, mUnk853032) == 252);
    static_assert(sizeof(pool.mUnk853032) == sizeof(float));
    return pool.mUnk853032.as<float>();
}

} // namespace lo::worker_backlog_opt::adapter
