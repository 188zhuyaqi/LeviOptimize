#include "features/timing/FullFrameTiming.h"

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "mc/server/commands/CommandFlag.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/server/commands/CommandPermissionLevel.h"

namespace lo::command {

void registerPerformanceCommand() {
    auto& command = ll::command::CommandRegistrar::getInstance(false).getOrCreateCommand(
        "loprofile",
        "Show full server-frame TPS and MSPT.",
        CommandPermissionLevel::Host,
        CommandFlagValue::None
    );
    command.overload().execute([](CommandOrigin const&, CommandOutput& output) {
        if (!full_frame_timing::isEnabled()) {
            output.error("Enable features.optFullFrameTiming to collect server-frame samples.");
            return;
        }
        auto stats = full_frame_timing::getSnapshot();
        if (stats.samples == 0) {
            output.error("Waiting for a completed server tick.");
            return;
        }
        output.success(
            "TPS {:.2f} (avg {:.2f}); MSPT {:.3f} (avg {:.3f}, last {} ticks).",
            stats.currentTps,
            stats.averageTps,
            stats.currentMspt,
            stats.averageMspt,
            stats.samples
        );
    });
}

} // namespace lo::command
