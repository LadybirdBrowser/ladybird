/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/RenderGraphDescription.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio {

namespace Realtime {

class RenderGraphImpl;

}

class WEB_API RenderGraph {
public:
    explicit RenderGraph(RenderGraphDescription const& description, f32 sample_rate);
    ~RenderGraph();

    AudioBus const& render_destination_for_current_quantum();

    void begin_quantum(size_t current_frame);

private:
    Realtime::RenderGraphImpl* m_impl { nullptr };
};

}
