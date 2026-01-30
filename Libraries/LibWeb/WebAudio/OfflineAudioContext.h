/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <AK/Variant.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/OfflineAudioContextPrototype.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/Engine/OfflineAudioRenderThread.h>
#include <LibWeb/WebAudio/RenderGraph.h>
#include <LibWeb/WebAudio/Types.h>
#include <LibWeb/WebIDL/Types.h>

namespace Core {

class Notifier;
class WeakEventLoopReference;

}

namespace Web::WebAudio {

class AnalyserNode;
class AudioWorkletNode;
class AudioScheduledSourceNode;
class ScriptProcessorNode;

namespace Render {

class AudioWorkletProcessorHost;
class ScriptProcessorHost;

}

// https://webaudio.github.io/web-audio-api/#OfflineAudioContextOptions
struct OfflineAudioContextOptions {
    WebIDL::UnsignedLong number_of_channels { 1 };
    WebIDL::UnsignedLong length {};
    float sample_rate {};
    Variant<Bindings::AudioContextRenderSizeCategory, WebIDL::UnsignedLong> render_size_hint { Bindings::AudioContextRenderSizeCategory::Default };
};

// https://webaudio.github.io/web-audio-api/#OfflineAudioContext
class OfflineAudioContext final : public BaseAudioContext {
    WEB_PLATFORM_OBJECT(OfflineAudioContext, BaseAudioContext);
    GC_DECLARE_ALLOCATOR(OfflineAudioContext);

public:
    static WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> construct_impl(JS::Realm&, OfflineAudioContextOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> construct_impl(JS::Realm&,
        WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate);

    virtual ~OfflineAudioContext() override;

    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> start_rendering();
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> resume();
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> suspend(double suspend_time);

    WebIDL::UnsignedLong length() const;

    GC::Ptr<WebIDL::CallbackType> oncomplete();
    void set_oncomplete(GC::Ptr<WebIDL::CallbackType>);

private:
    OfflineAudioContext(JS::Realm&, WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void dispatch_scheduled_source_end_event(AudioScheduledSourceNode&) override;

    WebIDL::UnsignedLong m_length {};
    WebIDL::UnsignedLong m_number_of_channels {};
    bool m_rendering_started { false };

    struct SuspendRequest {
        double suspend_time_seconds { 0.0 };
        u32 suspend_frame_index { 0 };
        GC::Ref<WebIDL::Promise> promise;
        bool resolved { false };
    };

    Vector<SuspendRequest> m_suspend_requests;

    GC::Ptr<AudioBuffer> m_rendered_buffer;

    // State for an in-progress startRendering(). These are consumed on completion.
    Optional<GC::Ref<WebIDL::Promise>> m_pending_render_promise;
    HashMap<NodeID, GC::Ref<AnalyserNode>> m_pending_analyser_nodes;

    u64 m_offline_render_completion_id { 0 };
    NonnullRefPtr<Core::WeakEventLoopReference> m_control_event_loop;

    RefPtr<Core::Notifier> m_render_suspend_notifier;
    int m_render_suspend_read_fd { -1 };

    OwnPtr<Render::OfflineAudioRenderThread> m_render_thread;

    struct WorkletRenderState {
        Render::GraphDescription graph_description;
        Render::GraphResourceRegistry resources;
        OwnPtr<RenderGraph> graph;
        u32 frame_index { 0 };
        u32 length_in_sample_frames { 0 };
        u32 channel_count { 0 };
        u32 render_quantum_size { 0 };
        Vector<u32> suspend_frame_indices;
        size_t next_suspend_index { 0 };
    };

    OwnPtr<WorkletRenderState> m_worklet_render_state;
    GC::Ptr<JS::Realm> m_worklet_realm_for_rendering;
    HashMap<NodeID, GC::Root<JS::Object>> m_worklet_processor_instances;
    OwnPtr<Render::AudioWorkletProcessorHost> m_worklet_processor_host;
    HashMap<NodeID, GC::Root<AudioWorkletNode>> m_audio_worklet_nodes_for_rendering;

    HashMap<NodeID, GC::Root<ScriptProcessorNode>> m_script_processor_nodes_for_rendering;
    OwnPtr<Render::ScriptProcessorHost> m_script_processor_host;

    void begin_offline_rendering(GC::Ref<WebIDL::Promise> promise);
    void handle_offline_render_completion();

    void schedule_worklet_rendering_step();
    void render_worklet_step();

    void handle_offline_render_suspended(u32 frame_index);
    u32 quantum_aligned_frame_index_for_time(double time_seconds) const;

    static void handle_render_thread_completion(u64 completion_id);
};

}
