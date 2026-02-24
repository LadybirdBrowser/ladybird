/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Weakable.h>
#include <LibGC/Weak.h>
#include <LibThreading/Mutex.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/Engine/GraphResources.h>
#include <LibWeb/WebAudio/Types.h>
#include <LibWeb/WebAudio/Worklet/WorkletModule.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>
#include <LibWeb/WebAudio/Worklet/WorkletPortBinding.h>
#include <LibWebAudio/SharedAudioBuffer.h>

namespace Web::WebAudio {

class AssociatedTaskQueue;
class BaseAudioContext;
class ControlMessageQueue;

}

namespace Web::WebAudio {

class SessionClientOfWebAudioWorker;

}

namespace Web::WebAudio::Render {

class SessionRouter;

struct ClientState {
    GC::Weak<Web::WebAudio::BaseAudioContext> context;
};

// WEB_API for tests
class WEB_API AudioContextRegistry final : public Weakable<AudioContextRegistry> {
public:
    struct OutputSessionFormat {
        u32 sample_rate { 0 };
        u32 channel_count { 0 };
    };

    using ClientId = u64;

    AudioContextRegistry();
    ~AudioContextRegistry();

    static void set_webaudio_client(NonnullRefPtr<::Web::WebAudio::SessionClientOfWebAudioWorker>);

    ErrorOr<void> open_webaudio_session(ClientId client_id, u32 target_latency_ms, u64 page_id, Function<void(ErrorOr<OutputSessionFormat> const&)> on_complete);
    Optional<OutputSessionFormat> output_session_format(ClientId client_id) const;

    ClientId register_client(BaseAudioContext&, ControlMessageQueue&, AssociatedTaskQueue&, Atomic<u64>& current_frame, Atomic<u8>& suspended, Atomic<u64>& suspend_generation, Atomic<u64>& underrun_frames_total);
    void unregister_client(ClientId);

    void set_client_suspended(ClientId, bool suspended, u64 generation);

    // Updates the render graph using the same wire format used by AudioServer.
    void update_client_render_graph(ClientId, f32 graph_sample_rate, ByteBuffer encoded_graph, Vector<SharedAudioBufferBinding> shared_audio_buffer_bindings, NonnullOwnPtr<GraphResourceRegistry> resources, Vector<WorkletModule> worklet_modules = {}, Vector<WorkletNodeDefinition> worklet_node_definitions = {}, Vector<WorkletPortBinding> worklet_port_bindings = {});

    void shutdown();
    size_t client_count() const;

    bool try_copy_analyser_snapshot(ClientId, NodeID analyser_node_id, u32 fft_size, Span<f32> out_time_domain, Span<f32> out_frequency_db, u64& out_render_quantum_index);
    bool try_copy_dynamics_compressor_reduction(ClientId, NodeID compressor_node_id, f32& out_reduction_db, u64& out_render_quantum_index);
    void refresh_client_timing(ClientId);

private:
    struct SessionLifecycleState {
        Optional<OutputSessionFormat> device_format;
        Optional<u64> page_id;
        u32 target_latency_ms { 50 };
        bool output_open_in_progress { false };
        Vector<Function<void(ErrorOr<OutputSessionFormat> const&)>> pending_output_open_callbacks;
        u32 min_target_latency_ms_from_graph { 0 };
        u64 mapped_session_id { 0 };
    };

    void set_graph_min_target_latency_ms(ClientId, u32);
    Optional<u64> mapped_session_id(ClientId) const;
    ErrorOr<void> maybe_reopen_webaudio_session(ClientId, SessionRouter&, u32 new_target_latency_ms);
    void close_webaudio_session(ClientId, SessionRouter&);
    ErrorOr<void> begin_open(ClientId, SessionRouter&, u32 target_latency_ms, u64 page_id);

    void on_client_session_mapping_changed(ClientId client_id, u64 old_session_id, u64 new_session_id);

    struct ClientEntry {
        ClientState state;
        NonnullOwnPtr<SessionRouter> worker_session;
        SessionLifecycleState session_state;
    };

    ClientId m_next_client_id { 1 };

    mutable Threading::Mutex m_clients_mutex;
    HashMap<ClientId, ClientEntry> m_clients;
};

}
