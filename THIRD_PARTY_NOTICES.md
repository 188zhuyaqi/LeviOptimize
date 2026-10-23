# Third-party notices

The following files adapt code or algorithms from [Endstone](https://github.com/EndstoneMC/endstone):

- `src/features/PlayerNetworkingOpt.cpp` and `ChunkOptimizationState.h`: `099d6f028` (batched influence updates).
- `src/features/RandomChunkOpt.cpp` and `ChunkOptimizationState.h`: `6f0c32205` and `bec9a2fd7` (autosave key snapshots).
- `src/features/CircuitSceneGraphOpt.cpp`: `55e5e34be` (stale redstone relationships).
- `src/features/ChunkGenDispatcherOpt.cpp` and `ChunkGenDispatcherSignature.h`: `7049af40e7ec7364627d9a2a1e14ebe4c2eb714d` (generation task limits).
- `src/features/ChunkCachesOpt.cpp` and `ChunkCacheSignatures.h`: `2bf178a9972dd8cbe3fb3f81f10dd1077fafc4ce` (blending/existence cache limits).
- `src/features/VerifiedBytePatch.h`: shared guarded patch lifecycle adapted while porting the two patch groups above.
- `src/features/VanillaWaypointsOpt.cpp` and `VanillaWaypointsState.h`: `874c1fa310545487cc86f7f7f5c0e207514bdb3f` (locator waypoint membership checks).
- `src/features/EntityPacketRecipientsOpt.cpp` and `EntityPacketRecipientsState.h`: `fa7b3d4eb488c5180bf722d7777b12536e35cc50` (entity broadcast recipient selection, without the active-user snapshot).
- `src/features/network/`: `4624070dd`, `e41e349ad`, `42b472028`, `fe86ade3f` (compression peers, batching and thread scratch).
- `src/features/parked_chunks/`: `099d6f0287bb0ac5f7cfc7653b41afec8083d4ba` (parked publisher queue nodes).
- `src/features/WorkerBacklog*`: `cd97eea16` (async worker backlog delay).
- `src/features/player_snapshot/`: `fa7b3d4eb488c5180bf722d7777b12536e35cc50` (active-player snapshots and iteration).
- `src/features/random_tick/`: `6e7417c00`, `fdea21001` (random-tick prefetch).
- `src/features/locator/`: `874c1fa310545487cc86f7f7f5c0e207514bdb3f` (pair/slot caches and waypoint updates).
- `src/features/timing/`: `03be20b63`, `6790acf3c` (full-frame measurement and catch-up tick accounting).

Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.

These portions are licensed under the Apache License, Version 2.0. A copy is included in [LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt). The modified files identify the adaptations for LeviLamina, including scoped batch state, cache lifetime cleanup, and bounded instruction matching with restoration on disable.

`ChunkLeakFix.cpp` also follows the native map cleanup approach described by Endstone commit `d520c1938`; it calls BDS's existing method rather than copying Endstone's implementation of that method.

LeviOptimize's project license is provided in [LICENSE](LICENSE).

## Compression libraries

The build statically links libdeflate 1.25, copyright Eric Biggers, under the MIT license; its full notice is included in [LICENSES/libdeflate-MIT.txt](LICENSES/libdeflate-MIT.txt).

The build statically links Snappy 1.2.2, copyright Google Inc., under the BSD 3-Clause license; its full upstream COPYING file is included in [LICENSES/snappy-BSD-3-Clause.txt](LICENSES/snappy-BSD-3-Clause.txt).

The packaged mod includes these licenses and this notice file.
