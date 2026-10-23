#pragma once

#include <memory>

namespace lo {

namespace network_opt {
struct NetworkOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;
    void                  call(bool);
    NetworkOpt();
    ~NetworkOpt();
    NetworkOpt(NetworkOpt&&) noexcept;
    NetworkOpt& operator=(NetworkOpt&&) noexcept;
};
} // namespace network_opt

namespace parked_chunks_opt {
struct ParkedChunksOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;
    void                  call(bool);
    ParkedChunksOpt();
    ~ParkedChunksOpt();
    ParkedChunksOpt(ParkedChunksOpt&&) noexcept;
    ParkedChunksOpt& operator=(ParkedChunksOpt&&) noexcept;
};
} // namespace parked_chunks_opt

namespace worker_backlog_opt {
struct WorkerBacklogOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;
    void                  call(bool);
    WorkerBacklogOpt();
    ~WorkerBacklogOpt();
    WorkerBacklogOpt(WorkerBacklogOpt&&) noexcept;
    WorkerBacklogOpt& operator=(WorkerBacklogOpt&&) noexcept;
};
} // namespace worker_backlog_opt

namespace player_snapshot_opt {
struct PlayerSnapshotOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;
    void                  call(bool);
    PlayerSnapshotOpt();
    ~PlayerSnapshotOpt();
    PlayerSnapshotOpt(PlayerSnapshotOpt&&) noexcept;
    PlayerSnapshotOpt& operator=(PlayerSnapshotOpt&&) noexcept;
};
} // namespace player_snapshot_opt

namespace random_tick_prefetch_opt {
struct RandomTickPrefetchOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;
    void                  call(bool);
    RandomTickPrefetchOpt();
    ~RandomTickPrefetchOpt();
    RandomTickPrefetchOpt(RandomTickPrefetchOpt&&) noexcept;
    RandomTickPrefetchOpt& operator=(RandomTickPrefetchOpt&&) noexcept;
};
} // namespace random_tick_prefetch_opt

namespace locator_opt {
struct LocatorOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;
    void                  call(bool);
    LocatorOpt();
    ~LocatorOpt();
    LocatorOpt(LocatorOpt&&) noexcept;
    LocatorOpt& operator=(LocatorOpt&&) noexcept;
};
} // namespace locator_opt

namespace full_frame_timing {
struct FullFrameTimingOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;
    void                  call(bool);
    FullFrameTimingOpt();
    ~FullFrameTimingOpt();
    FullFrameTimingOpt(FullFrameTimingOpt&&) noexcept;
    FullFrameTimingOpt& operator=(FullFrameTimingOpt&&) noexcept;
};
} // namespace full_frame_timing


namespace vanilla_waypoints_opt {
struct VanillaWaypointsOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;

    void call(bool);
    VanillaWaypointsOpt();
    ~VanillaWaypointsOpt();
    VanillaWaypointsOpt(VanillaWaypointsOpt&&) noexcept;
    VanillaWaypointsOpt& operator=(VanillaWaypointsOpt&&) noexcept;
};
} // namespace vanilla_waypoints_opt

namespace entity_packet_recipients_opt {
struct EntityPacketRecipientsOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;

    void call(bool);
    EntityPacketRecipientsOpt();
    ~EntityPacketRecipientsOpt();
    EntityPacketRecipientsOpt(EntityPacketRecipientsOpt&&) noexcept;
    EntityPacketRecipientsOpt& operator=(EntityPacketRecipientsOpt&&) noexcept;
};
} // namespace entity_packet_recipients_opt

namespace chunk_caches_opt {
struct ChunkCachesOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;

    void call(bool);
    ChunkCachesOpt();
    ~ChunkCachesOpt();
    ChunkCachesOpt(ChunkCachesOpt&&) noexcept;
    ChunkCachesOpt& operator=(ChunkCachesOpt&&) noexcept;
};
} // namespace chunk_caches_opt

namespace player_networking_opt {
struct PlayerNetworkingOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;

    void call(bool);
    PlayerNetworkingOpt();
    ~PlayerNetworkingOpt();
    PlayerNetworkingOpt(PlayerNetworkingOpt&&) noexcept;
    PlayerNetworkingOpt& operator=(PlayerNetworkingOpt&&) noexcept;
};
} // namespace player_networking_opt

namespace random_chunk_opt {
struct RandomChunkOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;

    void call(bool);
    RandomChunkOpt();
    ~RandomChunkOpt();
    RandomChunkOpt(RandomChunkOpt&&) noexcept;
    RandomChunkOpt& operator=(RandomChunkOpt&&) noexcept;
};
} // namespace random_chunk_opt

namespace circuit_scene_graph_opt {
struct CircuitSceneGraphOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;

    void call(bool);
    CircuitSceneGraphOpt();
    ~CircuitSceneGraphOpt();
    CircuitSceneGraphOpt(CircuitSceneGraphOpt&&) noexcept;
    CircuitSceneGraphOpt& operator=(CircuitSceneGraphOpt&&) noexcept;
};
} // namespace circuit_scene_graph_opt

namespace chunk_gen_dispatcher_opt {
struct ChunkGenDispatcherOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;

    void call(bool);
    ChunkGenDispatcherOpt();
    ~ChunkGenDispatcherOpt();
    ChunkGenDispatcherOpt(ChunkGenDispatcherOpt&&) noexcept;
    ChunkGenDispatcherOpt& operator=(ChunkGenDispatcherOpt&&) noexcept;
};
} // namespace chunk_gen_dispatcher_opt

namespace moving_block_opt {
struct MovingBlockOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;

    void call(bool);
    MovingBlockOpt();
    ~MovingBlockOpt();
    MovingBlockOpt(MovingBlockOpt&&) noexcept;
    MovingBlockOpt& operator=(MovingBlockOpt&&) noexcept;
};
} // namespace moving_block_opt

namespace chunk_leak_fix {
struct ChunkLeakFix {
    struct Impl;
    std::unique_ptr<Impl> impl;

    void call(bool);
    ChunkLeakFix();
    ~ChunkLeakFix();
    ChunkLeakFix(ChunkLeakFix&&) noexcept;
    ChunkLeakFix& operator=(ChunkLeakFix&&) noexcept;
};
} // namespace chunk_leak_fix

namespace push_entity_opt {
struct Config {
    bool enable              = false;
    bool disableVec0Push     = true;
    int  maxPushTimes        = -1;
    bool unlimitedPlayerPush = false;
};

struct PushEntityOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;

    void call(Config const&);
    PushEntityOpt();
    ~PushEntityOpt();
    PushEntityOpt(PushEntityOpt&&) noexcept;
    PushEntityOpt& operator=(PushEntityOpt&&) noexcept;
};
} // namespace push_entity_opt

namespace seen_percent_opt {
struct SeenPercentOpt {
    struct Impl;
    std::unique_ptr<Impl> impl;

    void call(bool);
    SeenPercentOpt();
    ~SeenPercentOpt();
    SeenPercentOpt(SeenPercentOpt&&) noexcept;
    SeenPercentOpt& operator=(SeenPercentOpt&&) noexcept;
};
} // namespace seen_percent_opt
} // namespace lo
