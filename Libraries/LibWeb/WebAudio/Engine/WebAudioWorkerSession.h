/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibWeb/WebAudio/Types.h>
#include <LibWeb/WebAudio/Worklet/WorkletModule.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>
#include <LibWeb/WebAudio/Worklet/WorkletPortBinding.h>

namespace WebAudioWorkerClient {

class WebAudioClient;

}

namespace Web::WebAudio::Render {

class WebAudioClientRegistry;
class GraphResourceRegistry;

// WebContent-side owner of the WebAudioWorker session and associated shared-memory transports.
//
// This is the only realtime WebAudio execution model: rendering happens out-of-process in
// WebAudioWorker (with AudioServer owning the OS output device).
class WebAudioWorkerSession {
public:
    WebAudioWorkerSession();
    ~WebAudioWorkerSession();

    // WebAudio server client integration is owned by this layer.
    static void set_webaudio_client(NonnullRefPtr<WebAudioWorkerClient::WebAudioClient>);
    static RefPtr<WebAudioWorkerClient::WebAudioClient> webaudio_client();

    bool has_output_open(WebAudioClientRegistry const&) const;
    ErrorOr<void> ensure_output_open(WebAudioClientRegistry&, u32 target_latency_ms, u64 page_id);
    void shutdown_output(WebAudioClientRegistry&);

    void update_client_render_graph(
        WebAudioClientRegistry&, u64 client_id, f32 graph_sample_rate, ByteBuffer encoded_graph,
        NonnullOwnPtr<GraphResourceRegistry> resources,
        Vector<WorkletModule> worklet_modules,
        Vector<WorkletNodeDefinition> worklet_node_definitions,
        Vector<WorkletPortBinding> worklet_port_bindings);

    void set_client_suspended(WebAudioClientRegistry&, u64 client_id, bool suspended, u64 generation);

    bool try_copy_analyser_snapshot(
        WebAudioClientRegistry&, u64 client_id, NodeID analyser_node_id, u32 fft_size,
        Span<f32> out_time_domain, Span<f32> out_frequency_db, u64& out_render_quantum_index);

    bool try_copy_dynamics_compressor_reduction(
        WebAudioClientRegistry&, u64 client_id, NodeID compressor_node_id, f32& out_reduction_db, u64& out_render_quantum_index);

private:
    struct Impl;
    OwnPtr<Impl> m_impl;
};

}
