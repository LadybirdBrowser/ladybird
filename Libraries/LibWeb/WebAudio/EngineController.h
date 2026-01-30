/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ByteBuffer.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibWeb/WebAudio/Engine/GraphResources.h>
#include <LibWeb/WebAudio/Types.h>
#include <LibWeb/WebAudio/Worklet/WorkletModule.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>
#include <LibWeb/WebAudio/Worklet/WorkletPortBinding.h>

namespace Web::WebAudio {

class AssociatedTaskQueue;
class BaseAudioContext;
class ControlMessageQueue;

namespace Render {

class WebAudioClientRegistry;

}

class EngineController final : public Weakable<EngineController> {
public:
    using ClientId = u64;

    static EngineController& the();

    struct DeviceFormat {
        u32 sample_rate { 0 };
        u32 channel_count { 0 };
    };

    ErrorOr<DeviceFormat> ensure_output_device_open(ClientId client_id, u32 target_latency_ms, u64 page_id);

    ClientId register_client(BaseAudioContext&, ControlMessageQueue&, AssociatedTaskQueue&, Atomic<u64>& current_frame, Atomic<u64>& suspend_state, Atomic<u64>& underrun_frames_total);
    void unregister_client(ClientId);

    void set_client_suspended(ClientId, bool suspended, u64 generation);

    void update_client_render_graph(ClientId,
        f32 graph_sample_rate,
        ByteBuffer encoded_graph,
        NonnullOwnPtr<Render::GraphResourceRegistry> resources,
        Vector<Render::WorkletModule> worklet_modules = {},
        Vector<Render::WorkletNodeDefinition> worklet_node_definitions = {},
        Vector<Render::WorkletPortBinding> worklet_port_bindings = {});

    void refresh_client_timing(ClientId);

    void stop_if_unused();

    bool try_copy_analyser_snapshot(ClientId, NodeID analyser_node_id, u32 fft_size, Span<f32> out_time_domain, Span<f32> out_frequency_db, u64& out_render_quantum_index);
    bool try_copy_dynamics_compressor_reduction(ClientId, NodeID compressor_node_id, f32& out_reduction_db, u64& out_render_quantum_index);

private:
    EngineController();

    OwnPtr<Render::WebAudioClientRegistry> m_engine;
};

}
