# Optimization helper tests

Ten C++23 suites exercise production helpers without a BDS executable. The network suite links the same libdeflate 1.25 and Snappy 1.2.2 packages as the mod. Checks remain active in release builds with `NDEBUG`.

From a Windows developer terminal, configure and run the same targets as CI:

```powershell
xmake f -a x64 -m release -p windows --target_type=server --with_tests=y -y
xmake -y
xmake test -v
```

`with_tests` defaults to `false` for normal mod builds. Each `tests/*Test.cpp` becomes a `test_<basename>` binary, and a failing check returns a nonzero exit status. Test executables stay in the build directory and are not packaged with the mod.

| Suite | Production behavior covered |
| --- | --- |
| ChunkOptimizationStateTest | Scoped influence batching, nesting/exception recovery, thread isolation, autosave snapshot refresh, changing map sizes and owned selected keys. |
| ChunkGenDispatcherSignatureTest | Exact instruction fixtures, wildcard displacement, truncation, ambiguity and bounded function scans. |
| ChunkCacheSignaturesTest | Both capacity signatures, expected bytes, corruption, boundaries and ambiguous/already-patched sequences. |
| VanillaWaypointsTest | Tracked/self/expired IDs, joins/leaves, signed boundaries and lookup short circuiting. |
| EntityPacketRecipientsTest | Relevance, subclients, invalid IDs and fallback before any partial send. |
| ChunkStreamingStateTest | Native queue hash validation, node parking/restoration, journal cursors and bounded retention, worker delay guards, deterministic random-tick seed predictions. |
| NetworkCodecTest | Raw DEFLATE/Snappy fixtures and negotiation, payload limits, malformed/trailing data, framing, nested and concurrent scratch buffers, strong/weak peer control-block lifetimes. |
| PlayerSnapshotTest | Snapshot identity/change detection, nested visits, exception unwinding and retirement. |
| LocatorStateTest | Pair-slot growth/reuse, visibility/texture comparisons and native vector windows through suspend, exceptions and retirement. |
| FullFrameTimingTest | Full-frame boundaries, catch-up ticks, idle exclusion, aborted/empty frames, initialized rolling averages and backward clock handling. |

The pure suites can also be built directly with `clang++ -std=c++23 -Wall -Wextra -Werror -Isrc`; add thread support as required by the platform and link libdeflate/Snappy for NetworkCodecTest. The Xmake commands above locate those dependencies automatically.

These tests do not load BDS or validate hook resolution, native ABI/layouts, DLL unload scheduling, packet delivery, chunk lifecycle, redstone, or measured TPS gains. Those require the runtime checks in [the handoff](../docs/NATIVE_PORT_HANDOFF.zh.md).
