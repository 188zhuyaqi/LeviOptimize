// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Adapted from Endstone cd97eea16d90c1ec686e54683bb444e2f17c495c.
#pragma once

namespace lo::worker_backlog_opt::detail {

class BacklogSetting {
    bool installed{};

public:
    bool install(float& value) {
        if (installed || value != 0.05F) return false;
        value     = 0.0F;
        installed = true;
        return true;
    }

    bool restore(float& value) {
        if (!installed) return false;
        installed = false;
        if (value != 0.0F) return false;
        value = 0.05F;
        return true;
    }
};

} // namespace lo::worker_backlog_opt::detail
