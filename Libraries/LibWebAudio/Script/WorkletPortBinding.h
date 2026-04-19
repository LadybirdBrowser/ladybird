/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/TransportHandle.h>
#include <LibWebAudio/LibWebAudio.h>

namespace Web::WebAudio::Render {

struct WorkletPortBinding {
    NodeID node_id;
    IPC::TransportHandle processor_port_handle;
};

inline void close_worklet_port_binding_fds(Vector<WorkletPortBinding>& bindings) { bindings.clear(); }

} // namespace Web::WebAudio::Render
