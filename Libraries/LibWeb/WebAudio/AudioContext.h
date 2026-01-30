/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibCore/Timer.h>
#include <LibGC/Root.h>
#include <LibWeb/Bindings/AudioContextPrototype.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/WebAudio/AudioPlaybackStats.h>
#include <LibWeb/WebAudio/AudioSinkInfo.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/MediaElementAudioSourceNode.h>
#include <LibWeb/WebAudio/MediaStreamAudioDestinationNode.h>
#include <LibWeb/WebAudio/MediaStreamAudioSourceNode.h>
#include <LibWeb/WebAudio/MediaStreamTrackAudioSourceNode.h>

namespace Web::WebAudio {

class ScriptProcessorNode;

}

namespace Web::DOM {

class DocumentObserver;

}

namespace Web::WebAudio::Render {

struct GraphDescription;
class GraphResourceRegistry;
class ScriptProcessorHost;

}

namespace Core {

class WeakEventLoopReference;

}

namespace Web::WebAudio {

struct AudioSinkOptions {
    Bindings::AudioSinkType type { Bindings::AudioSinkType::None };
};

struct AudioContextOptions {
    Variant<Bindings::AudioContextLatencyCategory, double> latency_hint { Bindings::AudioContextLatencyCategory::Interactive };
    Optional<float> sample_rate;
    Optional<Variant<String, AudioSinkOptions>> sink_id;
};

struct AudioTimestamp {
    double context_time { 0 };
    HighResolutionTime::DOMHighResTimeStamp performance_time { 0 };
};

// https://webaudio.github.io/web-audio-api/#AudioContext
class AudioContext final : public BaseAudioContext {
    WEB_PLATFORM_OBJECT(AudioContext, BaseAudioContext);
    GC_DECLARE_ALLOCATOR(AudioContext);

public:
    static WebIDL::ExceptionOr<GC::Ref<AudioContext>> construct_impl(JS::Realm&, Optional<AudioContextOptions> const& context_options = {});

    virtual ~AudioContext() override;

    double base_latency() const { return m_base_latency; }
    double output_latency() const { return m_output_latency; }
    Variant<String, GC::Ref<AudioSinkInfo>> sink_id() const { return m_sink_id; }
    void set_onsinkchange(WebIDL::CallbackType*);
    WebIDL::CallbackType* onsinkchange();
    void set_onerror(WebIDL::CallbackType*);
    WebIDL::CallbackType* onerror();
    GC::Ref<AudioPlaybackStats> playback_stats() const { return *m_playback_stats; }
    AudioTimestamp get_output_timestamp();
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> resume();
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> suspend();
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> close();
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> set_sink_id(Variant<String, AudioSinkOptions> const&);

    void forcibly_close();

    WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> create_media_element_source(GC::Ptr<HTML::HTMLMediaElement>);
    WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioSourceNode>> create_media_stream_source(GC::Ref<MediaCapture::MediaStream>);
    WebIDL::ExceptionOr<GC::Ref<MediaStreamTrackAudioSourceNode>> create_media_stream_track_source(GC::Ref<MediaCapture::MediaStreamTrack>);
    WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioDestinationNode>> create_media_stream_destination(AudioNodeOptions const& = {});

    virtual bool try_copy_realtime_analyser_data(NodeID, u32 fft_size, Span<f32> out_time_domain, Span<f32> out_frequency_db, u64& out_render_quantum_index) const override;
    virtual bool try_copy_realtime_dynamics_compressor_reduction(NodeID compressor_node_id, f32& out_reduction_db, u64& out_render_quantum_index) const override;

private:
    friend class AudioPlaybackStats;

    explicit AudioContext(JS::Realm& realm);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    double m_base_latency { 0 };
    double m_output_latency { 0 };

    u32 m_target_latency_ms { 20 };

    bool m_allowed_to_start = true;
    Vector<GC::Ref<WebIDL::Promise>> m_pending_resume_promises;
    bool m_suspended_by_user = false;
    bool m_suspended_by_visibility { false };
    GC::Ptr<DOM::DocumentObserver> m_document_observer;

    GC::Ptr<AudioPlaybackStats> m_playback_stats;

    bool m_sample_rate_is_explicit { false };

    Variant<String, GC::Ref<AudioSinkInfo>> m_sink_id { String {} };
    Variant<String, GC::Ref<AudioSinkInfo>> m_sink_id_at_construction { String {} };

    Optional<u64> m_audio_service_client_id;

    struct PendingRenderThreadStateAck {
        GC::Ref<WebIDL::Promise> promise;
        u64 generation { 0 };
        bool suspended { false };
    };

    u64 m_next_suspend_state_generation { 1 };
    Vector<PendingRenderThreadStateAck> m_pending_render_thread_state_acks;
    RefPtr<Core::Timer> m_render_thread_state_ack_timer;

    NonnullRefPtr<Core::WeakEventLoopReference> m_control_event_loop;
    HashMap<NodeID, GC::Root<ScriptProcessorNode>> m_script_processor_nodes_for_rendering;
    OwnPtr<Render::ScriptProcessorHost> m_script_processor_host;

    bool start_rendering_audio_graph();
    void stop_rendering_audio_graph();

    void ensure_render_thread_state_ack_timer_running();
    void snapshot_render_graph_and_prepare_resources(Render::GraphResourceRegistry& resources, Render::GraphDescription& out_graph_description);

    void process_render_thread_state_acks();

    void refresh_timing_page_for_stats();

    virtual void on_audio_graph_changed() override;
    virtual void on_scheduled_source_end_added() override;
};

}
