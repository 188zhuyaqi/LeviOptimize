# LeviOptimize

![English](https://img.shields.io/badge/English-inactive?style=for-the-badge)
[![中文](https://img.shields.io/badge/简体中文-informational?style=for-the-badge)](README.zh.md)

A performance optimization mod for Minecraft Bedrock Dedicated Server (BDS), "LeviOptimize" is designed to enhance server performance by addressing issues related to low ticks per second (TPS). Operating within the LeviLamina framework, it modifies BDS code to achieve notable performance improvements.

> **Migration status (2026-10-23)**: the mod is being restructured for a shared server/client codebase. The complete 0.17 server implementation is archived under [`old/`](old/README.md) and is **not built**; the current `src/` tree contains only the new architecture skeleton. See [the architecture design](docs/ARCHITECTURE.md) for the target model, feature lifecycle, and migration plan. The feature list below documents the archived implementation, not the current binary.

## Table of Contents

- [LeviOptimize](#levioptimize)
  - [Table of Contents](#table-of-contents)
  - [Security](#security)
  - [Installation](#installation)
    - [Using Lip](#using-lip)
    - [Manual Installation](#manual-installation)
  - [Usage](#usage)
  - [Commands](#commands)
  - [Features](#features)
  - [Contributing](#contributing)
  - [License](#license)

## Security

LeviOptimize is an independent project not affiliated with Mojang or Microsoft, thus not officially supported by them. It is open-source and free to use. Users should note that LeviOptimize is not liable for any potential damage to servers or data. Please proceed with caution and use at your own risk.

## Installation

### Using Lip

```sh
lip install github.com/LiteLDev/LeviOptimize
```

### Manual Installation

1. Download the latest release from the [Releases page](https://github.com/LiteLDev/LeviOptimize/releases).
2. Unpack the `LeviOptimize-windows-x64.zip` file.
3. Copy the `LeviOptimize` folder to `plugins/` for a dedicated server or `mods/` for the client.

## Usage

Upon its first run, LeviOptimize generates its configuration in `plugins/LeviOptimize/config/` on a dedicated server and `mods/LeviOptimize/config/` on a client. Each installation owns its own `config.json`; the two files are not interchangeable.

The v22 format follows iEconomicSystem: a top-level `$schema` and `version`, plus a flat `features` array. Each entry selects a feature with its stable `type`; missing fields are filled from defaults and unknown feature types are preserved:

```json
{
    "$schema": "config.schema.json",
    "version": 22,
    "features": [
        {
            "type": "tick_probe",
            "enable": false,
            "log_every_ticks": 200,
            "fail_on_start": false
        }
    ]
}
```

The schema is shipped as `config/config.schema.json` next to the config file. Version 21 files using the old `"features": { "tickProbe": {...} }` object layout are migrated automatically on load.

## Commands

Current skeleton (server commands):

- **`loreload`** (host permission): re-reads `config.json`, pushes the new values into the running feature instances, and reconciles them without restarting the process. It prints the per-feature outcome (started / stopped / failed / blocked). Features that must be installed before the client loop, the client world or the server world remain `RestartRequired` if they were first enabled after their deadline.
- **`lostatus`** (host permission): prints the runtime context (`side`, `world`, `clientLoop`, `clientWorld`, `serverWorld`) and every feature's id, start policy, state, wait reason and last lifecycle error, so a failed or waiting feature can be identified without reading the log.

`LeviOptimize::reloadConfig()` returns `ll::Expected<ReconcileReport>`; `FeatureManager::status(id)` and `statuses()` expose the same state to client UI or tests.

The commands below belong to the archived implementation in [`old/`](old/README.md):

- **`timing`**: Displays server performance statistics, focusing on TPS and detailed ECS performance metrics.
- **`loprofile`**: Displays full-frame MSPT/TPS and the last 20 collected tick samples when `features.optFullFrameTiming` is enabled. Registration is controlled by `commands.performanceCommand`.

> **Note**: If your application crashes immediately upon using the `timing` command, with the exception pointing to a null pointer, please check the version of your msvcp140.dll. Consider updating or repairing the Visual C++ Redistributable, or simply copy a working msvcp140.dll file from a functioning device to your BDS root directory.

## Features

> Archived 0.17 behaviour, kept for reference only. None of these features are compiled into the current skeleton; migration status is tracked in [the architecture design](docs/ARCHITECTURE.md).

- **`optMovingBlock`**: Improves FPS and TPS by optimizing block entities' processing during piston movements.
- **`fixChunkLeak`**: Removes disconnected players' map trackers through BDS's native cleanup, including decorations and dirty state, so their chunk views can unload.
- **`optSeenPercent`**: Caches "SeenPercent" values to improve TPS, with a note of caution regarding potential hash collisions.
- **`optPushEntity`**: Improves server performance by reducing TPS drops from entity cramming with three key options:
  - `disableVec0Push`: Stops entities from being squeezed when their collision vector is zero, enhancing performance without altering gameplay significantly.
  - `maxPushTimes`: Sets a limit on the number of times entities can be pushed per tick, with `-1` indicating this limit is disabled by default. Adjust carefully to avoid negatively impacting gameplay.
  - `unlimitedPlayerPush`: No longer limit the pushes related to players, and it is only effect when the `maxPushTimes` is enabled.
- **`optPlayerNetworking`**: Rebuilds each chunk generation influence list once after player networking, instead of once per player.
- **`optRandomChunk`**: Samples a snapshot of chunk keys for autosave, avoiding repeated linear walks through the chunk map. Newly added keys enter the snapshot within 400 samples; expired or unloaded chunks are skipped.
- **`optCircuitSceneGraph`**: Removes stale redstone power associations before circuit updates, while retaining components pending addition.
- **`optChunkGenDispatcher`**: Experimental Windows option from Endstone's `perf/chunk-gen-dispatcher` branch. Allows four times as many chunk generation tasks in flight. It applies only when the resolved function boundary and instruction signature match; otherwise it logs a warning and leaves BDS unchanged. More concurrent tasks can increase CPU and memory use.
- **`optChunkCaches`**: Raises chunk blending and chunk existence cache limits from 10,000/30,000 to about one million entries, reducing cache resets with dispersed players. Each change requires a unique instruction match inside its resolved function and is restored on disable. This increases retained memory.
- **`optVanillaWaypoints`**: Checks tracked actor IDs once per locator waypoint update to avoid repeated group scans. Changes to the locator setting and missing live players still use the native update.
- **`optEntityPacketRecipients`**: Server-only entity broadcast path that reads the actor ID once and batches recipients using each player's nearby actors. It preserves subclient identifiers and falls back before sending if required actor IDs are invalid. Mods overriding entity relevance need compatibility testing.

- **`optNetwork`**: Installs libdeflate/Snappy compression peers for new connections, removes the trace peer from that chain, and reuses the native batch stream and reentrant per-thread buffers. Existing connections need to reconnect.
- **`optParkedChunks`**: Parks unavailable publisher queue nodes and rechecks them after chunk events, preserving native queue nodes on recovery and disable.
- **`optWorkerBacklog`**: Sets the async worker backlog delay from 0.05 seconds to zero after checking the target field and expected value.
- **`optPlayerSnapshot`**: Reuses validated active-player snapshots for dimension iteration and active-player counts, with native fallback during incompatible nested traversal.
- **`optRandomTickPrefetch`**: Predicts vanilla random-tick samples from a copy of its live seed and prefetches storage without advancing the game RNG.
- **`optLocator`**: Adds locator pair/slot caches and the waypoint icon/visibility update path. It can be used with the separate manager option `optVanillaWaypoints`.
- **`optFullFrameTiming`**: Measures server-frame work, including pre-level work and catch-up ticks, for `loprofile`. Inter-frame idle time is excluded.

All fourteen options added by this port default to `false`; the archived configuration version is 20. Enable individual keys in `features` and restart to test them. Native symbol/layout assumptions and remaining runtime checks are listed in [the developer handoff](docs/NATIVE_PORT_HANDOFF.zh.md) and [the port audit](docs/ENDSTONE_PORTING.md). Missing required symbols skip the affected hook group. Disabling networking/timing may leave native tasks or references pending; DLL unload is refused until they drain.

## Contributing

We welcome questions, issues, and PRs. Feel free to contribute by:

- Creating an issue to ask questions or report bugs.
- Submitting PRs with improvements or new features.

## License

GPL-3.0-or-later © LiteLDev

Adapted Endstone code retains its Apache-2.0 notices; see [third-party notices](THIRD_PARTY_NOTICES.md).
