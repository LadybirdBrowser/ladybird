/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Span.h>
#include <LibCore/EventLoop.h>
#include <LibThreading/ConditionVariable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebAudio/Types.h>

namespace Web::WebAudio {

class ScriptProcessorNode;

}

namespace Web::WebAudio::Render {

struct RenderContext;

// Host interface for ScriptProcessorNode processing.
// Implementations run on a JS-capable thread (typically the control thread) and are injected
// via RenderContext so offline and realtime backends can share the same render node.
class ScriptProcessorHost {
public:
    virtual ~ScriptProcessorHost() = default;

    // Processes one ScriptProcessor block (bufferSize frames) and writes planar output.
    // playback_time_seconds corresponds to the intended playback time of the produced output.
    virtual bool process_script_processor(
        NodeID node_id,
        RenderContext&,
        double playback_time_seconds,
        size_t buffer_size,
        size_t input_channel_count,
        size_t output_channel_count,
        Span<ReadonlySpan<f32>> input_channels,
        Span<Span<f32>> output_channels)
        = 0;
};

class OfflineScriptProcessorHost final : public ScriptProcessorHost {
public:
    OfflineScriptProcessorHost(JS::Realm& realm, BaseAudioContext& context, HashMap<NodeID, GC::Root<ScriptProcessorNode>> const& nodes)
        : m_realm(realm)
        , m_context(context)
        , m_nodes(nodes)
    {
    }

    virtual bool process_script_processor(NodeID node_id, RenderContext&, double playback_time_seconds, size_t buffer_size, size_t input_channel_count, size_t output_channel_count, Span<ReadonlySpan<f32>> input_channels, Span<Span<f32>> output_channels) override;

private:
    JS::Realm& m_realm;
    BaseAudioContext& m_context;
    HashMap<NodeID, GC::Root<ScriptProcessorNode>> const& m_nodes;
};

class RealtimeScriptProcessorHost final : public ScriptProcessorHost {
public:
    RealtimeScriptProcessorHost(JS::Realm& realm, BaseAudioContext& context, NonnullRefPtr<Core::WeakEventLoopReference> control_event_loop, HashMap<NodeID, GC::Root<ScriptProcessorNode>> const& nodes)
        : m_realm(realm)
        , m_context(context)
        , m_control_event_loop(move(control_event_loop))
        , m_nodes(nodes)
    {
    }

    virtual bool process_script_processor(NodeID node_id, RenderContext&, double playback_time_seconds, size_t buffer_size, size_t input_channel_count, size_t output_channel_count, Span<ReadonlySpan<f32>> input_channels, Span<Span<f32>> output_channels) override;

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

    JS::Realm& m_realm;
    BaseAudioContext& m_context;
    NonnullRefPtr<Core::WeakEventLoopReference> m_control_event_loop;
    HashMap<NodeID, GC::Root<ScriptProcessorNode>> const& m_nodes;

    Atomic<u64> m_next_sequence { 0 };
    Atomic<i64> m_last_zero_input_log_ms { 0 };
};

}
