// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace lo::parked_chunks_opt {

// Call after disabling features. Reentrant native callbacks may still be
// traversing queues or using the resident cleanup hooks' trampolines.
[[nodiscard]] bool canUnload() noexcept;

} // namespace lo::parked_chunks_opt
