/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Realtime/RenderGraphImpl.h>
#include <LibWeb/WebAudio/RenderGraph.h>

namespace Web::WebAudio {

using Realtime::RenderGraphImpl;

RenderGraph::RenderGraph(RenderGraphDescription const& description, f32 sample_rate)
{
    m_impl = new RenderGraphImpl(description, sample_rate);
}

RenderGraph::~RenderGraph()
{
    delete m_impl;
    m_impl = nullptr;
}

AudioBus const& RenderGraph::render_destination_for_current_quantum()
{
    return m_impl->render_destination_for_current_quantum();
}

void RenderGraph::begin_quantum(size_t current_frame)
{
    m_impl->begin_quantum(current_frame);
}

}
