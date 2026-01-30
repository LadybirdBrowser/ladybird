/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/System.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio::Render {

struct WorkletPortBinding {
    NodeID node_id;
    int processor_port_fd { -1 };
};

inline void close_worklet_port_binding_fds(Vector<WorkletPortBinding>& bindings)
{
    for (auto& binding : bindings) {
        if (binding.processor_port_fd >= 0)
            (void)Core::System::close(binding.processor_port_fd);
        binding.processor_port_fd = -1;
    }
}

}
