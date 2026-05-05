/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Weakable.h>
#include <LibIPC/TransportHandle.h>
#include <LibJS/Forward.h>
#include <LibThreading/Mutex.h>
#include <LibWeb/Export.h>
#include <LibWebAudio/GraphNodes/GraphNodeTypes.h>
#include <LibWebAudio/LibWebAudio.h>
#include <LibWebAudio/Script/AudioWorkletProcessorHost.h>
#include <LibWebAudio/Script/WorkletModule.h>
#include <LibWebAudio/Script/WorkletPortBinding.h>

namespace Web::WebAudio::Render {

class WEB_API RealtimeAudioWorkletProcessorHost final : public AudioWorkletProcessorHost
    , public Weakable<RealtimeAudioWorkletProcessorHost> {
public:
    RealtimeAudioWorkletProcessorHost(u64 initial_current_frame, float initial_sample_rate,
        Vector<WorkletModule> modules,
        Vector<WorkletNodeDefinition> node_definitions,
        Vector<WorkletPortBinding> port_bindings);
    virtual ~RealtimeAudioWorkletProcessorHost() override;

    void enqueue_worklet_module(WorkletModule);
    void enqueue_node_definitions(Vector<WorkletNodeDefinition>);
    void enqueue_port_bindings(Vector<WorkletPortBinding>);
    void synchronize_node_definitions(Vector<WorkletNodeDefinition> const&);
    void set_processor_error_callback(Function<void(NodeID)>);
    void set_processor_registration_callback(
        Function<void(String const&, Vector<Render::AudioParamDescriptor> const&, u64)>);
    void set_worklet_module_evaluation_callback(
        Function<void(u64 module_id, u64 required_generation, bool success, String const& error_name,
            String const& error_message, Vector<String> failed_processor_registrations)>);
    void service_render_thread_state(u64 current_frame, float sample_rate);

    virtual bool
    process_audio_worklet(NodeID node_id, RenderContext& process_context, String const& processor_name,
        size_t number_of_inputs, size_t number_of_outputs,
        Span<size_t const> output_channel_count,
        Vector<Vector<AudioBus const*>> const& inputs, Span<AudioBus*> outputs,
        Span<AudioWorkletProcessorHost::ParameterSpan const> parameters) override;

private:
    struct RenderThreadState;

    struct SharedNode {
        NodeID node_id;
        ByteString processor_name;
        size_t number_of_inputs { 1 };
        size_t number_of_outputs { 1 };
        Optional<Vector<size_t>> output_channel_count;
        size_t channel_count { 2 };
        ChannelCountMode channel_count_mode { ChannelCountMode::Max };
        ChannelInterpretation channel_interpretation { ChannelInterpretation::Speakers };
        Vector<ByteString> parameter_names;

        Optional<Vector<WorkletParameterDataEntry>> parameter_data;
        Optional<IPC::MessageDataType> serialized_processor_options;
    };

    RenderThreadState& ensure_render_thread_state();
    RenderThreadState create_render_thread_state();
    void evaluate_modules(RenderThreadState&, Vector<WorkletModule> const&);
    void initialize_ports(RenderThreadState&);
    void ensure_node_exists(RenderThreadState&, WorkletNodeDefinition const&);
    void try_attach_port_transport(RenderThreadState&, NodeID);
    void ensure_ready_processor_instances(RenderThreadState&);
    static bool has_pending_worklet_tasks(RenderThreadState&);
    static void pump_event_loops(RenderThreadState&);
    u64 stabilize_registration_generation(RenderThreadState&);
    void process_pending_updates(RenderThreadState&);
    JS::Object* ensure_processor_instance(RenderThreadState&, SharedNode&);

    SharedNode* find_node(NodeID);

    void consume_pending_updates(Vector<WorkletModule>& out_new_modules,
        Vector<WorkletNodeDefinition>& out_node_definitions,
        Vector<WorkletPortBinding>& out_port_bindings);
    void notify_processor_registered(String const&, Vector<AudioParamDescriptor> const&);
    void notify_module_evaluated(u64 module_id, u64 required_generation, bool success, String const& error_name,
        String const& error_message, Vector<String> failed_processor_registrations);

    // Worklet thread state.
    Vector<WorkletModule> m_modules;
    HashMap<ByteString, Vector<String>> m_failed_processor_registrations_by_url;
    HashMap<NodeID, IPC::TransportHandle> m_processor_port_handles;

    Threading::Mutex m_nodes_mutex;
    // Nodes are append-only and live for the host lifetime.
    // The render thread and request queue use raw SharedNode pointers via snapshots.
    // If we ever need to reclaim nodes under churn, add an epoch-based retirement scheme.
    Vector<NonnullOwnPtr<SharedNode>> m_nodes;
    HashMap<NodeID, SharedNode*> m_nodes_by_id;

    Threading::Mutex m_update_mutex;
    Vector<WorkletModule> m_pending_modules;
    Vector<WorkletNodeDefinition> m_pending_node_definitions;
    Vector<WorkletPortBinding> m_pending_port_bindings;

    Threading::Mutex m_callback_mutex;
    Function<void(NodeID)> m_processor_error_callback;
    Function<void(String const&, Vector<AudioParamDescriptor> const&, u64)> m_processor_registration_callback;
    Function<void(u64 module_id, u64 required_generation, bool success, String const& error_name,
        String const& error_message, Vector<String> failed_processor_registrations)>
        m_module_evaluation_callback;

    OwnPtr<RenderThreadState> m_render_thread_state;

    Atomic<u64> m_processor_registration_generation { 0 };

    u64 m_initial_current_frame { 0 };
    float m_initial_sample_rate { 44100.0f };
};

} // namespace Web::WebAudio::Render
