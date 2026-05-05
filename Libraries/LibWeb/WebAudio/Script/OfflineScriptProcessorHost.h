/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibGC/Root.h>
#include <LibWeb/Forward.h>
#include <LibWebAudio/LibWebAudio.h>
#include <LibWebAudio/Script/ScriptProcessorHost.h>

namespace Web::WebAudio {

class ScriptProcessorNode;

}

namespace Web::WebAudio::Render {

class OfflineScriptProcessorHost final : public ScriptProcessorHost {
public:
    OfflineScriptProcessorHost(GC::Ref<JS::Realm> realm, GC::Ref<BaseAudioContext> context,
        HashMap<NodeID, GC::Root<ScriptProcessorNode>> const& nodes)
        : m_realm(realm)
        , m_context(context)
        , m_nodes(nodes)
    {
    }

    virtual bool process_script_processor(NodeID node_id, RenderContext&, double playback_time_seconds,
        size_t buffer_size, size_t input_channel_count,
        size_t output_channel_count, Span<ReadonlySpan<f32>> input_channels,
        Span<Span<f32>> output_channels) override;

private:
    GC::Ref<JS::Realm> m_realm;
    GC::Ref<BaseAudioContext> m_context;
    HashMap<NodeID, GC::Root<ScriptProcessorNode>> const& m_nodes;
};

}
