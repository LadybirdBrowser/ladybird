/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Weakable.h>
#include <LibGC/Weak.h>
#include <LibThreading/Mutex.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebAudio/AudioParamDescriptor.h>
#include <LibWeb/WebAudio/Engine/GraphResources.h>
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
    Atomic<u64>* underrun_frames_total { nullptr };
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

    ErrorOr<DeviceFormat> ensure_output_device_open(ClientId client_id, u32 target_latency_ms, u64 page_id);

    ClientId register_client(BaseAudioContext&, ControlMessageQueue&, AssociatedTaskQueue&, Atomic<u64>& current_frame, Atomic<u64>& suspend_state, Atomic<u64>& underrun_frames_total);
    void unregister_client(ClientId);

    void set_client_suspended(ClientId, bool suspended, u64 generation);

    // Updates the render graph using the same wire format used by AudioServer.
    // When running with the AudioServer backend, this forwards the bytes via IPC.
    // When running in-process, this decodes and applies the update locally.
    void update_client_render_graph(ClientId, f32 graph_sample_rate, ByteBuffer encoded_graph, NonnullOwnPtr<GraphResourceRegistry> resources, Vector<WorkletModule> worklet_modules = {}, Vector<WorkletNodeDefinition> worklet_node_definitions = {}, Vector<WorkletPortBinding> worklet_port_bindings = {});

    void shutdown();
    size_t client_count() const;

    void handle_worklet_processor_error(u64 session_id, NodeID node_id);
    void handle_worklet_processor_registration(u64 session_id, String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation);
    void handle_worklet_module_evaluated(u64 session_id, u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> const& failed_processor_registrations);

    bool try_copy_analyser_snapshot(ClientId, NodeID analyser_node_id, u32 fft_size, Span<f32> out_time_domain, Span<f32> out_frequency_db, u64& out_render_quantum_index);
    bool try_copy_dynamics_compressor_reduction(ClientId, NodeID compressor_node_id, f32& out_reduction_db, u64& out_render_quantum_index);
    void refresh_client_timing(ClientId);

private:
    friend class WebAudioWorkerSession;

    ErrorOr<void> reopen_output_device(ClientId client_id, u32 new_target_latency_ms);
    void set_client_device_format(ClientId client_id, DeviceFormat format);
    void update_client_session_mapping(ClientId client_id, u64 session_id);
    Optional<ClientId> client_id_for_session(u64 session_id) const;

    struct ClientEntry {
        ClientState state;
        NonnullOwnPtr<WebAudioWorkerSession> worker_session;
        Optional<DeviceFormat> device_format;
        Optional<u64> page_id;
        u32 target_latency_ms { 50 };
        u32 min_target_latency_ms_from_graph { 0 };
        HashMap<String, Vector<Web::WebAudio::AudioParamDescriptor>> registered_processor_descriptors;
        HashTable<String> failed_processor_registrations;
        u64 last_registration_generation { 0 };
        u64 session_id { 0 };
    };

    ClientId m_next_client_id { 1 };

    mutable Threading::Mutex m_clients_mutex;
    HashMap<ClientId, ClientEntry> m_clients;
    HashMap<u64, ClientId> m_session_id_to_client;
};

}
