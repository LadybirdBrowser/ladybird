/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibCore/SharedBufferStream.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio::Render {

class ScriptProcessorHost;

struct ScriptProcessorNodeState {
    u32 buffer_size { 0 };
    u32 input_channel_count { 0 };
    u32 output_channel_count { 0 };

    Core::SharedBufferStream request_stream;
    Core::SharedBufferStream response_stream;
    Vector<f32> output_scratch;
};

class ScriptProcessorRequestPump {
public:
    ScriptProcessorRequestPump() = default;

    void set_host(ScriptProcessorHost* host) { m_host = host; }

    void process(NodeID node_id, ScriptProcessorNodeState&);

private:
    void process_requests(NodeID node_id, ScriptProcessorNodeState&);

    ScriptProcessorHost* m_host { nullptr };
};

}
