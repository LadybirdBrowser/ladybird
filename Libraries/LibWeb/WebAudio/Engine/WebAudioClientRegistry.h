/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/Weakable.h>
#include <LibGC/Weak.h>
#include <LibThreading/ConditionVariable.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/Engine/GraphResources.h>
#include <LibWeb/WebAudio/AudioParamDescriptor.h>
#include <LibWeb/WebAudio/Types.h>
#include <LibWeb/WebAudio/Worklet/WorkletModule.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>
#include <LibWeb/WebAudio/Worklet/WorkletPortBinding.h>

namespace Web::WebAudio {

class AssociatedTaskQueue;
class BaseAudioContext;
class ControlMessageQueue;

}

namespace WebAudioWorkerClient {

class WebAudioClient;

}

namespace Web::WebAudio::Render {

class WebAudioWorkerSession;

struct ClientState {
    GC::Weak<Web::WebAudio::BaseAudioContext> context;
    Atomic<u64>* current_frame { nullptr };
    Atomic<u64>* suspend_state { nullptr };
};

// WEB_API for tests
class WEB_API WebAudioClientRegistry final : public Weakable<WebAudioClientRegistry> {
public:
    struct DeviceFormat {
        u32 sample_rate { 0 };
        u32 channel_count { 0 };
    };

    using ClientId = u64;

    WebAudioClientRegistry();
    ~WebAudioClientRegistry();

    static void set_webaudio_client(NonnullRefPtr<WebAudioWorkerClient::WebAudioClient>);

    ErrorOr<DeviceFormat> ensure_output_device_open(u32 target_latency_ms, u64 page_id);

    ClientId register_client(BaseAudioContext&, ControlMessageQueue&, AssociatedTaskQueue&, Atomic<u64>& current_frame, Atomic<u64>& suspend_state);
    void unregister_client(ClientId);
    void set_client_render_graph(ClientId, f32 graph_sample_rate, ByteBuffer encoded_graph, NonnullOwnPtr<GraphResourceRegistry> resources, Vector<WorkletModule> worklet_modules, Vector<WorkletNodeDefinition> worklet_node_definitions, Vector<WorkletPortBinding> worklet_port_bindings);

    void set_client_suspended(ClientId, bool suspended, u64 generation);

    // Updates the render graph using the same wire format used by AudioServer.
    // When running with the AudioServer backend, this forwards the bytes via IPC.
    // When running in-process, this decodes and applies the update locally.
    void update_client_render_graph(ClientId, f32 graph_sample_rate, ByteBuffer encoded_graph, NonnullOwnPtr<GraphResourceRegistry> resources, Vector<WorkletModule> worklet_modules = {}, Vector<WorkletNodeDefinition> worklet_node_definitions = {}, Vector<WorkletPortBinding> worklet_port_bindings = {});

    void shutdown();
    size_t client_count() const;

    void handle_worklet_processor_error(NodeID node_id);
    void handle_worklet_processor_registration(String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation);

    bool try_copy_analyser_snapshot(ClientId, NodeID analyser_node_id, u32 fft_size, Span<f32> out_time_domain, Span<f32> out_frequency_db, u64& out_render_quantum_index);
    bool try_copy_dynamics_compressor_reduction(ClientId, NodeID compressor_node_id, f32& out_reduction_db, u64& out_render_quantum_index);

private:
    friend class WebAudioWorkerSession;

    ErrorOr<void> reopen_output_device(u32 new_target_latency_ms);

    NonnullOwnPtr<WebAudioWorkerSession> m_worker_session;

    u32 m_target_latency_ms { 50 };

    ClientId m_next_client_id { 1 };

    mutable Threading::Mutex m_clients_mutex;
    HashMap<ClientId, ClientState> m_clients;

    Threading::Mutex m_device_format_mutex;
    Threading::ConditionVariable m_device_format_selected;
    Optional<DeviceFormat> m_device_format;

    u32 m_min_target_latency_ms_from_graph { 0 };
    Optional<u64> m_page_id;
};

}
