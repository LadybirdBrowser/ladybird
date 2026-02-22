/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/RenderProcessContext.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio::Realtime {

class RenderNode {
public:
    explicit RenderNode(NodeID node_id)
        : m_node_id(node_id)
    {
    }

    virtual ~RenderNode() = default;

    NodeID node_id() const { return m_node_id; }

    virtual void process(RenderProcessContext&, Vector<Vector<AudioBus const*>> const& inputs) = 0;

    virtual size_t output_count() const { return 1; }
    virtual AudioBus const& output(size_t output_index) const = 0;

protected:
    NodeID m_node_id { 0 };
};

}
