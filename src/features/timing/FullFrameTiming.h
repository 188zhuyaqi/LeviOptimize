#pragma once

#include "FullFrameTimingState.h"

namespace lo::full_frame_timing {

[[nodiscard]] bool           isEnabled() noexcept;
[[nodiscard]] TimingSnapshot getSnapshot();
// Retries safe retirement and releases hooks only after listeners, queued native
// function objects and active callbacks have drained. Otherwise reject DLL unload.
[[nodiscard]] bool canUnload() noexcept;

} // namespace lo::full_frame_timing
