// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace lo::network_opt {

// Call after disabling features. Existing network tasks can still own peers;
// their custom vtables and destructors must remain mapped until they release.
// The count includes expired peers' control blocks retained by weak owners.
// canUnload also retires resident hooks after all callback leases have drained.
[[nodiscard]] bool        canUnload() noexcept;
[[nodiscard]] std::size_t liveWrapperCount() noexcept;

} // namespace lo::network_opt
