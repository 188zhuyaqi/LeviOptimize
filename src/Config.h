#pragma once

#include "features/features.h"
#include "ll/api/reflection/Dispatcher.h"

namespace lo {
using ll::reflection::Dispatcher;
struct Config {

    int version = 20;

    struct {
        Dispatcher<bool, moving_block_opt::MovingBlockOpt>                        optMovingBlock = false;
        Dispatcher<bool, chunk_leak_fix::ChunkLeakFix>                            fixChunkLeak   = true;
        Dispatcher<bool, seen_percent_opt::SeenPercentOpt>                        optSeenPercent = false;
        Dispatcher<push_entity_opt::Config, push_entity_opt::PushEntityOpt>       optPushEntity{};
        Dispatcher<bool, player_networking_opt::PlayerNetworkingOpt>              optPlayerNetworking       = false;
        Dispatcher<bool, random_chunk_opt::RandomChunkOpt>                        optRandomChunk            = false;
        Dispatcher<bool, circuit_scene_graph_opt::CircuitSceneGraphOpt>           optCircuitSceneGraph      = false;
        Dispatcher<bool, chunk_gen_dispatcher_opt::ChunkGenDispatcherOpt>         optChunkGenDispatcher     = false;
        Dispatcher<bool, chunk_caches_opt::ChunkCachesOpt>                        optChunkCaches            = false;
        Dispatcher<bool, vanilla_waypoints_opt::VanillaWaypointsOpt>              optVanillaWaypoints       = false;
        Dispatcher<bool, entity_packet_recipients_opt::EntityPacketRecipientsOpt> optEntityPacketRecipients = false;
        Dispatcher<bool, network_opt::NetworkOpt>                                 optNetwork                = false;
        Dispatcher<bool, parked_chunks_opt::ParkedChunksOpt>                      optParkedChunks           = false;
        Dispatcher<bool, worker_backlog_opt::WorkerBacklogOpt>                    optWorkerBacklog          = false;
        Dispatcher<bool, player_snapshot_opt::PlayerSnapshotOpt>                  optPlayerSnapshot         = false;
        Dispatcher<bool, random_tick_prefetch_opt::RandomTickPrefetchOpt>         optRandomTickPrefetch     = false;
        Dispatcher<bool, locator_opt::LocatorOpt>                                 optLocator                = false;
        Dispatcher<bool, full_frame_timing::FullFrameTimingOpt>                   optFullFrameTiming        = false;
    } features{};
    struct {
        bool timingCommand      = true;
        bool performanceCommand = true;
    } commands{};
};

} // namespace lo
