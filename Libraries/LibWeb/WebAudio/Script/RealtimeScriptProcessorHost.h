/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Span.h>
#include <LibCore/EventLoop.h>
#include <LibGC/Root.h>
#include <LibThreading/ConditionVariable.h>
#include <LibWeb/Forward.h>
#include <LibWebAudio/LibWebAudio.h>
#include <LibWebAudio/Script/ScriptProcessorHost.h>

namespace Web::WebAudio {

class ScriptProcessorNode;

}

namespace Web::WebAudio::Render {

class RealtimeScriptProcessorHost final : public ScriptProcessorHost {
public:
    RealtimeScriptProcessorHost(GC::Ref<JS::Realm>, GC::Ref<BaseAudioContext>,
        NonnullRefPtr<Core::WeakEventLoopReference>,
        HashMap<NodeID, GC::Root<ScriptProcessorNode>> const&);

    virtual bool process_script_processor(NodeID node_id, RenderContext&, double playback_time_seconds,
        size_t buffer_size, size_t input_channel_count,
        size_t output_channel_count, Span<ReadonlySpan<f32>> input_channels,
        Span<Span<f32>> output_channels) override;

private:
    struct Request : public AtomicRefCounted<Request> {
        Threading::Mutex mutex;
        Threading::ConditionVariable completed { mutex };
        bool done { false };
        bool ok { false };
        u64 sequence { 0 };
        NodeID node_id { 0 };
        double playback_time_seconds { 0.0 };
        size_t buffer_size { 0 };
        size_t input_channel_count { 0 };
        size_t output_channel_count { 0 };
        Vector<Vector<f32>> input_data;
        Vector<Span<f32>> output_spans;
    };

    bool perform_request_on_control_thread(Request& request);

    GC::Ref<JS::Realm> m_realm;
    GC::Ref<BaseAudioContext> m_context;
    NonnullRefPtr<Core::WeakEventLoopReference> m_control_event_loop;
    HashMap<NodeID, GC::Root<ScriptProcessorNode>> const& m_nodes;
    Atomic<u64> m_next_sequence { 0 };
};

} // namespace Web::WebAudio::Render
