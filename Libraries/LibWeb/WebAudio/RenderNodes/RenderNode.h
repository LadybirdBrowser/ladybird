/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/Engine/GraphDescription.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>
#include <LibWeb/WebAudio/Engine/RenderContext.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio::Render {

class RenderNode {
public:
    explicit RenderNode(NodeID node_id)
        : m_node_id(node_id)
    {
    }

    virtual ~RenderNode() = default;

    NodeID node_id() const { return m_node_id; }

    virtual void process(RenderContext&, Vector<Vector<AudioBus const*>> const& inputs, Vector<Vector<AudioBus const*>> const& param_inputs) = 0;

    virtual size_t output_count() const { return 1; }
    virtual AudioBus const& output(size_t output_index) const = 0;

    // Called on the render thread at a quantum boundary when applying a ControlMessage.
    // Default no-op for nodes that are not AudioScheduledSourceNode-backed.
    virtual void schedule_start(Optional<size_t>) { }
    virtual void schedule_stop(Optional<size_t>) { }

    // apply_description is the realtime-safe node update hook.
    // It is called on the render thread at a quantum boundary when applying a ParameterUpdate.
    // Implementations must not allocate, lock, or perform unbounded work.
    virtual void apply_description(GraphNodeDescription const&) { }

    // apply_description_offline is the non-realtime update hook.
    // It may allocate/rescale internal buffers and is only intended for offline/suspended contexts.
    // By default, this forwards to apply_description.
    virtual void apply_description_offline(GraphNodeDescription const& description) { apply_description(description); }

protected:
    NodeID m_node_id { 0 };
};

}
