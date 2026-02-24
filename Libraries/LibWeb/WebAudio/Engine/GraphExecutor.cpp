/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/Math.h>
#include <AK/StringBuilder.h>
#include <AK/Try.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/GraphCompiler.h>
#include <LibWeb/WebAudio/Engine/GraphExecutor.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>
#include <LibWeb/WebAudio/GraphNodes/AudioListenerGraphNode.h>
#include <LibWeb/WebAudio/GraphNodes/BiquadFilterGraphNode.h>
#include <LibWeb/WebAudio/RenderNodes/AnalyserRenderNode.h>
#include <LibWeb/WebAudio/RenderNodes/AudioListenerRenderNode.h>
#include <LibWeb/WebAudio/RenderNodes/DelayRenderNode.h>
#include <LibWeb/WebAudio/RenderNodes/DynamicsCompressorRenderNode.h>
#include <LibWeb/WebAudio/RenderNodes/RenderNode.h>

namespace Web::WebAudio::Render {

GraphExecutor::GraphExecutor(GraphDescription const& description, f32 sample_rate, size_t quantum_size, GraphResourceResolver const* resources)
    : m_description(description)
    , m_resources(resources)
{
    ASSERT_CONTROL_THREAD();
    m_context.sample_rate = sample_rate;
    m_context.quantum_size = quantum_size;
    GraphCompiler::build_nodes(*this, this->resources());

    for (size_t i = 0; i < m_nodes.size(); ++i) {
        if (m_node_types_by_index[i] == GraphNodeType::AudioListener) {
            m_context.listener = static_cast<AudioListenerRenderNode*>(m_nodes[i].ptr());
            break;
        }
    }

    auto param_count_for_node = [&](size_t node_index) -> size_t {
        auto const node_type = m_node_types_by_index[node_index];
        size_t param_count = RenderParamLayout::param_count(node_type);
        if (node_type == GraphNodeType::AudioWorklet) {
            auto const node_id = m_node_ids[node_index];
            auto node_it = m_description.nodes.find(node_id);
            if (node_it != m_description.nodes.end() && node_it->value.has<AudioWorkletGraphNode>())
                param_count = node_it->value.get<AudioWorkletGraphNode>().parameter_names.size();
        }
        return param_count;
    };

    // Initialize per-node per-param automation state and implicit param buses.
    m_param_automation_state.resize(m_nodes.size());
    m_param_automation_buses.resize(m_nodes.size());
    for (size_t node_index = 0; node_index < m_nodes.size(); ++node_index) {
        size_t const param_count = param_count_for_node(node_index);

        m_param_automation_state[node_index].resize(param_count);
        m_param_automation_buses[node_index].ensure_capacity(param_count);
        for (size_t param_index = 0; param_index < param_count; ++param_index)
            m_param_automation_buses[node_index].append(make<AudioBus>(1, m_context.quantum_size));
    }

    // Initialize the per-param intrinsic values and clamping ranges from node descriptions.
    for (size_t node_index = 0; node_index < m_nodes.size(); ++node_index) {
        auto const node_type = m_node_types_by_index[node_index];
        size_t const param_count = param_count_for_node(node_index);
        if (param_count == 0)
            continue;

        auto const node_id = m_node_ids[node_index];
        auto node_it = m_description.nodes.find(node_id);
        if (node_it == m_description.nodes.end())
            continue;
        auto const& node_desc = node_it->value;

        auto set_state = [&](size_t param_index, f32 initial_value, f32 min_value, f32 max_value) {
            if (param_index >= m_param_automation_state[node_index].size())
                return;
            auto& state = m_param_automation_state[node_index][param_index];
            state.initial_value = initial_value;
            state.default_value = initial_value;
            state.min_value = min_value;
            state.max_value = max_value;
        };

        switch (node_type) {
        case GraphNodeType::BiquadFilter:
            if (node_desc.has<BiquadFilterGraphNode>())
                node_desc.get<BiquadFilterGraphNode>().initialize_param_state(m_context.sample_rate, set_state);
            break;
        case GraphNodeType::DynamicsCompressor:
            if (node_desc.has<DynamicsCompressorGraphNode>())
                node_desc.get<DynamicsCompressorGraphNode>().initialize_param_state(set_state);
            break;
        case GraphNodeType::Gain:
            if (node_desc.has<GainGraphNode>())
                node_desc.get<GainGraphNode>().initialize_param_state(set_state);
            break;
        case GraphNodeType::Oscillator:
            if (node_desc.has<OscillatorGraphNode>())
                node_desc.get<OscillatorGraphNode>().initialize_param_state(set_state);
            break;
        case GraphNodeType::AudioBufferSource:
            if (node_desc.has<AudioBufferSourceGraphNode>())
                node_desc.get<AudioBufferSourceGraphNode>().initialize_param_state(set_state);
            break;
        case GraphNodeType::ConstantSource:
            if (node_desc.has<ConstantSourceGraphNode>())
                node_desc.get<ConstantSourceGraphNode>().initialize_param_state(set_state);
            break;
        case GraphNodeType::Delay:
            if (node_desc.has<DelayGraphNode>())
                node_desc.get<DelayGraphNode>().initialize_param_state(set_state);
            break;
        case GraphNodeType::AudioListener:
            if (node_desc.has<AudioListenerGraphNode>())
                node_desc.get<AudioListenerGraphNode>().initialize_param_state(set_state);
            break;
        case GraphNodeType::Panner:
            if (node_desc.has<PannerGraphNode>())
                node_desc.get<PannerGraphNode>().initialize_param_state(set_state);
            break;
        case GraphNodeType::StereoPanner:
            if (node_desc.has<StereoPannerGraphNode>())
                node_desc.get<StereoPannerGraphNode>().initialize_param_state(set_state);
            break;
        default:
            break;
        }
    }

    // Load automation tracks from the snapshot.
    for (auto const& automation : m_description.param_automations) {
        auto node_index_it = m_node_index_by_id.find(automation.destination);
        if (node_index_it == m_node_index_by_id.end())
            continue;
        auto const node_index = node_index_it->value;
        if (automation.destination_param_index >= m_param_automation_state[node_index].size())
            continue;

        auto& state = m_param_automation_state[node_index][automation.destination_param_index];
        state.initial_value = automation.initial_value;
        state.default_value = automation.default_value;
        state.min_value = automation.min_value;
        state.max_value = automation.max_value;
        state.automation_rate = automation.automation_rate;
        state.current_segment_index = 0;
        state.segments = automation.segments;
    }

    m_topology = GraphCompiler::build_topology(*this, m_description);
    GraphCompiler::rebuild_output_cache_capacity(*this);

    if (m_topology) {
        auto const destination_index = m_topology->destination_node_index;
        if (destination_index < m_topology->inputs_by_input.size()) {
            auto const destination_node_id = m_node_ids[destination_index];
            auto const& inputs = m_topology->inputs_by_input[destination_index];

            StringBuilder builder;
            for (size_t input_index = 0; input_index < inputs.size(); ++input_index) {
                auto const& connections = inputs[input_index];
                builder.appendff(" in{}:", input_index);
                for (auto const& connection : connections) {
                    auto source_node_id = m_node_ids[connection.source_node_index];
                    builder.appendff(" {}", source_node_id.value());
                }
            }
            WA_DBGLN("[GraphExecutor] destination {} audio inputs:{}", destination_node_id.value(), builder.string_view());
        }
    }
}

GraphExecutor::~GraphExecutor()
{
    // ASSERT_CONTROL_THREAD();
    delete m_pending_parameter_updates.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    for (auto& slot : m_retired_parameter_updates)
        delete slot.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);

    delete m_pending_topology.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    for (auto& slot : m_retired_topologies)
        delete slot.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);

    delete m_topology;
    m_topology = nullptr;
}

RenderContext& GraphExecutor::process_context()
{
    ASSERT_RENDER_THREAD();
    return m_context;
}

AudioBus const& GraphExecutor::render_destination_for_current_quantum()
{
    ASSERT_RENDER_THREAD();
    process_graph_if_needed();
    return m_topology->nodes[m_topology->destination_node_index].render_node->output(0);
}

void GraphExecutor::render_analysers_for_current_quantum()
{
    ASSERT_RENDER_THREAD();
    // Ensure analysers advance their internal state even when disconnected.
    process_graph_if_needed();
}

void GraphExecutor::begin_new_quantum(size_t current_frame)
{
    ASSERT_RENDER_THREAD();
    try_commit_pending_topology();
    try_commit_pending_parameter_updates();
    m_context.current_frame = current_frame;
    ++m_cache_generation;
}

void GraphExecutor::commit_pending_updates(size_t current_frame)
{
    ASSERT_RENDER_THREAD();
    // This allows the output driver to apply graph/param changes promptly without having to
    // render (and potentially advance time or fill buffers) just to hit the quantum boundary.
    try_commit_pending_topology();
    try_commit_pending_parameter_updates();
    m_context.current_frame = current_frame;
}

void GraphExecutor::schedule_source_start(NodeID node_id, Optional<size_t> start_frame)
{
    ASSERT_RENDER_THREAD();
    auto it = m_node_index_by_id.find(node_id);
    if (it == m_node_index_by_id.end())
        return;
    auto node_index = it->value;
    if (node_index >= m_nodes.size())
        return;
    m_nodes[node_index]->schedule_start(start_frame);
}

void GraphExecutor::schedule_source_stop(NodeID node_id, Optional<size_t> stop_frame)
{
    ASSERT_RENDER_THREAD();
    auto it = m_node_index_by_id.find(node_id);
    if (it == m_node_index_by_id.end())
        return;
    auto node_index = it->value;
    if (node_index >= m_nodes.size())
        return;
    m_nodes[node_index]->schedule_stop(stop_frame);
}

bool GraphExecutor::enqueue_topology_update(GraphDescription const& description)
{
    ASSERT_CONTROL_THREAD();
    if (description.destination_node_id != m_description.destination_node_id)
        return false;

    if (description.nodes.size() != m_description.nodes.size())
        return false;

    for (auto const node_id : m_node_ids) {
        auto old_node = m_description.nodes.get(node_id);
        auto new_node = description.nodes.get(node_id);
        if (!old_node.has_value()
            || !new_node.has_value()
            || graph_node_type(*old_node) != graph_node_type(*new_node)
            || GraphCompiler::classify_node_update(*old_node, *new_node) != GraphUpdateKind::None) {

            return false;
        }
    }

    auto* new_topology = GraphCompiler::build_topology(*this, description);
    delete m_pending_topology.exchange(new_topology, AK::MemoryOrder::memory_order_acq_rel);

    // Keep the control-thread description in sync immediately.
    // Note: The render thread commits topology updates asynchronously; m_description must remain
    // control-thread-owned to avoid data races.
    m_description.connections = description.connections;
    m_description.param_connections = description.param_connections;
    return true;
}

bool GraphExecutor::enqueue_parameter_update(GraphDescription const& description)
{
    ASSERT_CONTROL_THREAD();
    if (description.destination_node_id != m_description.destination_node_id)
        return false;

    if (description.connections.size() != m_description.connections.size())
        return false;
    for (size_t i = 0; i < description.connections.size(); ++i) {
        auto const& a = description.connections[i];
        auto const& b = m_description.connections[i];
        if (a.source != b.source || a.destination != b.destination || a.source_output_index != b.source_output_index || a.destination_input_index != b.destination_input_index)
            return false;
    }

    if (description.param_connections.size() != m_description.param_connections.size())
        return false;
    for (size_t i = 0; i < description.param_connections.size(); ++i) {
        auto const& a = description.param_connections[i];
        auto const& b = m_description.param_connections[i];
        if (a.source != b.source || a.destination != b.destination || a.source_output_index != b.source_output_index || a.destination_param_index != b.destination_param_index)
            return false;
    }

    if (description.nodes.size() != m_node_ids.size())
        return false;

    auto* batch = new ParameterUpdateBatch;
    batch->nodes_by_index.resize_with_default_value(m_node_ids.size(), GraphNodeDescription { OhNoesGraphNode {} });
    batch->param_automations = description.param_automations;

    for (size_t node_index = 0; node_index < m_node_ids.size(); ++node_index) {
        auto const node_id = m_node_ids[node_index];
        auto maybe_desc = description.nodes.get(node_id);
        if (!maybe_desc.has_value()) {
            delete batch;
            return false;
        }
        if (graph_node_type(*maybe_desc) != m_node_types_by_index[node_index]) {
            delete batch;
            return false;
        }

        auto old_desc = m_description.nodes.get(node_id);
        VERIFY(old_desc.has_value());
        auto node_update = GraphCompiler::classify_node_update(*old_desc, *maybe_desc);
        if (node_update != GraphUpdateKind::Parameter && node_update != GraphUpdateKind::None) {
            delete batch;
            return false;
        }
        batch->nodes_by_index[node_index] = *maybe_desc;
    }

    delete m_pending_parameter_updates.exchange(batch, AK::MemoryOrder::memory_order_acq_rel);

    // Keep the control-thread description in sync immediately.
    // Note: The render thread applies the update asynchronously; m_description must remain
    // control-thread-owned to avoid data races.
    for (size_t node_index = 0; node_index < m_node_ids.size(); ++node_index) {
        auto const node_id = m_node_ids[node_index];
        m_description.nodes.set(node_id, batch->nodes_by_index[node_index]);
    }
    m_description.param_automations = description.param_automations;
    return true;
}

void GraphExecutor::collect_retired_updates()
{
    ASSERT_CONTROL_THREAD();
    collect_retired_updates_for_teardown();
}

void GraphExecutor::collect_retired_updates_for_teardown()
{
#ifndef NDEBUG
    // Retired update deletion is safe to run from either the WebAudio control thread or a
    // render thread during teardown (e.g. OfflineAudioContext render worker). We still assert
    // that the caller has marked the thread role to catch accidental calls from arbitrary threads.
    ASSERT(::Web::WebAudio::current_thread_is_control_thread() || ::Web::WebAudio::current_thread_is_render_thread());
#endif

    for (auto& slot : m_retired_topologies)
        delete slot.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);

    for (auto& slot : m_retired_parameter_updates)
        delete slot.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
}

GraphUpdateKind GraphExecutor::classify_update(GraphDescription const& description) const
{
    ASSERT_CONTROL_THREAD();
    return GraphCompiler::classify_update(m_description, description);
}

size_t GraphExecutor::analyser_count() const
{
    ASSERT_RENDER_THREAD();
    return m_analyser_node_indices.size();
}

NodeID GraphExecutor::analyser_node_id(size_t analyser_index) const
{
    ASSERT_RENDER_THREAD();
    if (analyser_index >= m_analyser_node_indices.size())
        return 0;
    return m_node_ids[m_analyser_node_indices[analyser_index]];
}

bool GraphExecutor::copy_analyser_time_domain_data(size_t analyser_index, Span<f32> output) const
{
    ASSERT_RENDER_THREAD();
    if (analyser_index >= m_analyser_node_indices.size())
        return false;

    auto node_index = m_analyser_node_indices[analyser_index];
    VERIFY(node_index < m_nodes.size());
    VERIFY(m_node_types_by_index[node_index] == GraphNodeType::Analyser);

    return static_cast<AnalyserRenderNode&>(*m_nodes[node_index]).copy_analyser_time_domain_data(output);
}

bool GraphExecutor::copy_analyser_frequency_data_db(size_t analyser_index, Span<f32> output) const
{
    ASSERT_RENDER_THREAD();
    if (analyser_index >= m_analyser_node_indices.size())
        return false;

    auto node_index = m_analyser_node_indices[analyser_index];
    VERIFY(node_index < m_nodes.size());
    VERIFY(m_node_types_by_index[node_index] == GraphNodeType::Analyser);

    return static_cast<AnalyserRenderNode&>(*m_nodes[node_index]).copy_analyser_frequency_data_db(output);
}

bool GraphExecutor::try_copy_dynamics_compressor_reduction(NodeID compressor_node_id, f32& out_reduction_db) const
{
    ASSERT_RENDER_THREAD();
    auto node_index_it = m_node_index_by_id.find(compressor_node_id);
    if (node_index_it == m_node_index_by_id.end())
        return false;
    size_t const node_index = node_index_it->value;
    if (node_index >= m_node_types_by_index.size())
        return false;
    if (m_node_types_by_index[node_index] != GraphNodeType::DynamicsCompressor)
        return false;

    RenderNode* node = m_nodes[node_index].ptr();
    if (!node)
        return false;

    out_reduction_db = static_cast<DynamicsCompressorRenderNode&>(*node).reduction_db();
    return true;
}

void GraphExecutor::apply_update_offline(GraphDescription const& description)
{
    ASSERT_RENDER_THREAD();
    auto update_kind = GraphCompiler::classify_update(m_description, description);

    m_description.destination_node_id = description.destination_node_id;
    m_description.nodes = description.nodes;
    m_description.connections = description.connections;
    m_description.param_connections = description.param_connections;
    m_description.param_automations = description.param_automations;

    if (update_kind == GraphUpdateKind::RebuildRequired) {
        m_node_index_by_id.clear();
        m_node_ids.clear();
        m_node_types_by_index.clear();
        m_nodes.clear();
        m_analyser_node_indices.clear();
        m_param_automation_state.clear();
        m_param_automation_buses.clear();

        m_context.listener = nullptr;
        GraphCompiler::build_nodes(*this, resources());
        for (size_t i = 0; i < m_nodes.size(); ++i) {
            if (m_node_types_by_index[i] == GraphNodeType::AudioListener) {
                m_context.listener = static_cast<AudioListenerRenderNode*>(m_nodes[i].ptr());
                break;
            }
        }

        auto param_count_for_node = [&](size_t node_index) -> size_t {
            auto const node_type = m_node_types_by_index[node_index];
            size_t param_count = RenderParamLayout::param_count(node_type);
            if (node_type == GraphNodeType::AudioWorklet) {
                auto const node_id = m_node_ids[node_index];
                auto node_it = m_description.nodes.find(node_id);
                if (node_it != m_description.nodes.end() && node_it->value.has<AudioWorkletGraphNode>())
                    param_count = node_it->value.get<AudioWorkletGraphNode>().parameter_names.size();
            }
            return param_count;
        };

        m_param_automation_state.resize(m_nodes.size());
        m_param_automation_buses.resize(m_nodes.size());
        for (size_t node_index = 0; node_index < m_nodes.size(); ++node_index) {
            size_t const param_count = param_count_for_node(node_index);

            m_param_automation_state[node_index].resize(param_count);
            m_param_automation_buses[node_index].ensure_capacity(param_count);
            for (size_t param_index = 0; param_index < param_count; ++param_index)
                m_param_automation_buses[node_index].append(make<AudioBus>(1, m_context.quantum_size));
        }
    } else {
        for (auto const& it : description.nodes) {
            auto node_index_it = m_node_index_by_id.find(it.key);
            if (node_index_it == m_node_index_by_id.end())
                continue;
            m_nodes[node_index_it->value]->apply_description_offline(it.value);
        }
    }

    delete m_topology;
    m_topology = GraphCompiler::build_topology(*this, m_description);
    GraphCompiler::rebuild_output_cache_capacity(*this);
    m_cache_generation = 1;
    m_last_processed_generation = 0;

    // Refresh automation state from the new description.
    for (size_t node_index = 0; node_index < m_param_automation_state.size(); ++node_index) {
        for (size_t param_index = 0; param_index < m_param_automation_state[node_index].size(); ++param_index) {
            auto& state = m_param_automation_state[node_index][param_index];
            state.current_segment_index = 0;
            state.segments.clear();
            state.automation_rate = AutomationRate::ARate;
        }
    }

    // Reinitialize intrinsic values and clamp ranges from node descriptions.
    for (size_t node_index = 0; node_index < m_nodes.size(); ++node_index) {
        auto const node_type = m_node_types_by_index[node_index];
        size_t const param_count = m_param_automation_state[node_index].size();
        if (param_count == 0)
            continue;

        auto const node_id = m_node_ids[node_index];
        auto node_it = m_description.nodes.find(node_id);
        if (node_it == m_description.nodes.end())
            continue;
        auto const& node_desc = node_it->value;

        auto set_state = [&](size_t param_index, f32 initial_value, f32 min_value, f32 max_value) {
            if (param_index >= m_param_automation_state[node_index].size())
                return;
            auto& state = m_param_automation_state[node_index][param_index];
            state.initial_value = initial_value;
            state.default_value = initial_value;
            state.min_value = min_value;
            state.max_value = max_value;
        };

        switch (node_type) {
        case GraphNodeType::BiquadFilter:
            if (node_desc.has<BiquadFilterGraphNode>())
                node_desc.get<BiquadFilterGraphNode>().initialize_param_state(m_context.sample_rate, set_state);
            break;
        case GraphNodeType::DynamicsCompressor:
            if (node_desc.has<DynamicsCompressorGraphNode>())
                node_desc.get<DynamicsCompressorGraphNode>().initialize_param_state(set_state);
            break;
        case GraphNodeType::Gain:
            if (node_desc.has<GainGraphNode>())
                node_desc.get<GainGraphNode>().initialize_param_state(set_state);
            break;
        case GraphNodeType::Oscillator:
            if (node_desc.has<OscillatorGraphNode>()) {
                auto const& osc = node_desc.get<OscillatorGraphNode>();
                set_state(OscillatorParamIndex::frequency, osc.frequency, 0.0f, AK::NumericLimits<f32>::max());
                set_state(OscillatorParamIndex::detune, osc.detune_cents, -AK::NumericLimits<f32>::max(), AK::NumericLimits<f32>::max());
            }
            break;
        case GraphNodeType::AudioBufferSource:
            if (node_desc.has<AudioBufferSourceGraphNode>()) {
                auto const& buf = node_desc.get<AudioBufferSourceGraphNode>();
                set_state(AudioBufferSourceParamIndex::playback_rate, buf.playback_rate, 0.0f, AK::NumericLimits<f32>::max());
                set_state(AudioBufferSourceParamIndex::detune, buf.detune_cents, -AK::NumericLimits<f32>::max(), AK::NumericLimits<f32>::max());
            }
            break;
        case GraphNodeType::ConstantSource:
            if (node_desc.has<ConstantSourceGraphNode>())
                set_state(ConstantSourceParamIndex::offset, node_desc.get<ConstantSourceGraphNode>().offset, -AK::NumericLimits<f32>::max(), AK::NumericLimits<f32>::max());
            break;
        case GraphNodeType::Delay:
            if (node_desc.has<DelayGraphNode>()) {
                auto const& delay = node_desc.get<DelayGraphNode>();
                set_state(DelayParamIndex::delay_time, delay.delay_time_seconds, 0.0f, max(delay.max_delay_time_seconds, 0.0f));
            }
            break;
        case GraphNodeType::AudioListener:
            if (node_desc.has<AudioListenerGraphNode>())
                node_desc.get<AudioListenerGraphNode>().initialize_param_state(set_state);
            break;
        case GraphNodeType::Panner:
            if (node_desc.has<PannerGraphNode>())
                node_desc.get<PannerGraphNode>().initialize_param_state(set_state);
            break;
        case GraphNodeType::StereoPanner:
            if (node_desc.has<StereoPannerGraphNode>())
                set_state(StereoPannerParamIndex::pan, node_desc.get<StereoPannerGraphNode>().pan, -1.0f, 1.0f);
            break;
        default:
            break;
        }
    }

    for (auto const& automation : m_description.param_automations) {
        auto node_index_it = m_node_index_by_id.find(automation.destination);
        if (node_index_it == m_node_index_by_id.end())
            continue;
        auto const node_index = node_index_it->value;
        if (automation.destination_param_index >= m_param_automation_state[node_index].size())
            continue;

        auto& state = m_param_automation_state[node_index][automation.destination_param_index];
        state.initial_value = automation.initial_value;
        state.default_value = automation.default_value;
        state.min_value = automation.min_value;
        state.max_value = automation.max_value;
        state.automation_rate = automation.automation_rate;
        state.current_segment_index = 0;
        state.segments = automation.segments;
    }
}

void GraphExecutor::process_graph_if_needed()
{
    ASSERT_RENDER_THREAD();
    if (m_last_processed_generation == m_cache_generation)
        return;

    // https://webaudio.github.io/web-audio-api/#rendering-loop
    // 4.4: For each AudioNode in ordered node list, execute these steps:

    auto& topology = *m_topology;

    static constexpr size_t max_mixing_channel_count = 32;

    auto compute_computed_number_of_channels = [](Topology::ChannelMixingSettings const& settings, size_t max_input_channels) -> size_t {
        size_t const safe_channel_count = max<size_t>(1, settings.channel_count);
        size_t const safe_max_input_channels = max<size_t>(1, max_input_channels);

        switch (settings.channel_count_mode) {
        case ChannelCountMode::Max:
            return safe_max_input_channels;
        case ChannelCountMode::ClampedMax:
            return min(safe_max_input_channels, safe_channel_count);
        case ChannelCountMode::Explicit:
            return safe_channel_count;
        }

        return safe_channel_count;
    };

    for (auto const node_index : topology.processing_order) {
        auto const& processing_node = topology.nodes[node_index];
        RenderNode* node = processing_node.render_node;
        VERIFY(node != nullptr);

        auto& per_input_connections = topology.inputs_by_input[node_index];
        auto& per_input_buses = topology.input_buses_scratch[node_index];
        VERIFY(per_input_buses.size() == per_input_connections.size());

        for (size_t input_index = 0; input_index < per_input_connections.size(); ++input_index) {
            auto const& connections = per_input_connections[input_index];
            auto& buses = per_input_buses[input_index];

            // Slot 0 is the mixed input bus for this quantum.
            VERIFY(buses.size() == connections.size() + 1);
            auto* mixed_bus = topology.input_mix_buses[node_index][input_index].ptr();
            buses[0] = mixed_bus;

            for (size_t i = 0; i < connections.size(); ++i) {
                auto const& c = connections[i];
                VERIFY(c.source_node_index < topology.nodes.size());
                auto* source_node = topology.nodes[c.source_node_index].render_node;
                VERIFY(source_node != nullptr);
                size_t const source_output_count = source_node->output_count();
                VERIFY(source_output_count > 0);
                size_t const clamped_source_output = min(c.source_output, source_output_count - 1);
                buses[i + 1] = &source_node->output(clamped_source_output);
            }

            // Mix all incoming connections at the graph edge (per input), per spec.
            // https://webaudio.github.io/web-audio-api/#channel-up-mixing-and-down-mixing
            if (!connections.is_empty()) {
                bool any_input_with_channels = false;
                size_t max_input_channels = 0;
                for (size_t i = 0; i < connections.size(); ++i) {
                    auto const* bus = buses[i + 1];
                    if (!bus)
                        continue;
                    size_t const channel_count = bus->channel_count();
                    if (channel_count == 0)
                        continue;
                    any_input_with_channels = true;
                    max_input_channels = max(max_input_channels, channel_count);
                }

                if (any_input_with_channels) {
                    size_t desired_channels = compute_computed_number_of_channels(topology.channel_mixing_by_node[node_index], max_input_channels);
                    desired_channels = clamp(desired_channels, 1ul, max_mixing_channel_count);
                    desired_channels = min(desired_channels, mixed_bus->channel_capacity());
                    mixed_bus->set_channel_count(desired_channels);

                    auto const* input_span = &buses[1];
                    auto const input_count = connections.size();
                    if (topology.channel_mixing_by_node[node_index].channel_interpretation == ChannelInterpretation::Discrete)
                        mix_inputs_discrete_into(*mixed_bus, ReadonlySpan<AudioBus const*>(input_span, input_count));
                    else
                        mix_inputs_into(*mixed_bus, ReadonlySpan<AudioBus const*>(input_span, input_count));
                } else {
                    bool keep_silent_input = false;
                    if (processing_node.node_type == GraphNodeType::AudioWorklet) {
                        NodeID const node_id = m_node_ids[processing_node.real_node_index];
                        auto node_it = m_description.nodes.find(node_id);
                        if (node_it != m_description.nodes.end() && node_it->value.has<AudioWorkletGraphNode>()) {
                            auto const& worklet_desc = node_it->value.get<AudioWorkletGraphNode>();
                            if (worklet_desc.number_of_outputs == 0)
                                keep_silent_input = true;
                        }
                    }

                    if (keep_silent_input) {
                        size_t desired_channels = compute_computed_number_of_channels(topology.channel_mixing_by_node[node_index], max_input_channels);
                        desired_channels = clamp(desired_channels, 1ul, max_mixing_channel_count);
                        desired_channels = min(desired_channels, mixed_bus->channel_capacity());
                        mixed_bus->set_channel_count(desired_channels);
                        mixed_bus->zero();
                    } else {
                        mixed_bus->set_channel_count(0);
                        mixed_bus->zero();
                        buses[0] = nullptr;
                    }
                }
            } else {
                mixed_bus->set_channel_count(0);
                mixed_bus->zero();
                buses[0] = nullptr;
            }

            // Hide raw input buses from nodes to avoid double-counting.
            for (size_t i = 0; i < connections.size(); ++i)
                buses[i + 1] = nullptr;
        }

        auto& per_param_connections = topology.param_inputs_by_param[node_index];
        auto& per_param_buses = topology.param_input_buses_scratch[node_index];
        VERIFY(per_param_buses.size() == per_param_connections.size());

        for (size_t param_index = 0; param_index < per_param_connections.size(); ++param_index) {
            auto const& connections = per_param_connections[param_index];
            auto& buses = per_param_buses[param_index];
            VERIFY(buses.size() == connections.size() + 1);

            // Slot 0 is the computed param bus for this quantum.
            size_t const param_owner_index = processing_node.param_owner_node_index;
            auto* computed_bus = m_param_automation_buses[param_owner_index][param_index].ptr();
            computed_bus->zero();
            buses[0] = computed_bus;

            for (size_t i = 0; i < connections.size(); ++i) {
                auto const& c = connections[i];
                VERIFY(c.source_node_index < topology.nodes.size());
                auto* source_node = topology.nodes[c.source_node_index].render_node;
                VERIFY(source_node != nullptr);
                size_t const source_output_count = source_node->output_count();
                VERIFY(source_output_count > 0);
                size_t const clamped_source_output = min(c.source_output, source_output_count - 1);
                buses[i + 1] = &source_node->output(clamped_source_output);
            }

            // Compute computedValue for this AudioParam (mono bus), centralizing k-rate vs a-rate.
            // computedValue = intrinsic + sum(downmix(param inputs))
            auto& state = m_param_automation_state[param_owner_index][param_index];
            computed_bus->zero();

            if (!connections.is_empty()) {
                // Sum/downmix param inputs to mono.
                // NOTE: This uses the same mixing rules as other audio inputs.
                mix_inputs_into(*computed_bus, ReadonlySpan<AudioBus const*>(&buses[1], connections.size()));
            }

            auto out = computed_bus->channel(0);

            auto evaluate_segment_at_frame = [&](GraphAutomationSegment const& segment, size_t frame) -> f32 {
                f64 const sample_time = static_cast<f64>(frame) / static_cast<f64>(m_context.sample_rate);
                if (sample_time <= segment.start_time)
                    return segment.start_value;
                if (sample_time >= segment.end_time)
                    return segment.end_value;

                f64 const denom = segment.end_time > segment.start_time ? (segment.end_time - segment.start_time) : 0.0;
                f64 const pos = denom > 0.0 ? clamp((sample_time - segment.start_time) / denom, 0.0, 1.0) : 0.0;

                switch (segment.type) {
                case GraphAutomationSegmentType::Constant:
                    return segment.start_value;
                case GraphAutomationSegmentType::LinearRamp:
                    return static_cast<f32>(static_cast<f64>(segment.start_value) + ((static_cast<f64>(segment.end_value) - static_cast<f64>(segment.start_value)) * pos));
                case GraphAutomationSegmentType::ExponentialRamp: {
                    // FIXME: Ensure full spec behavior for exponential ramps, including edge cases.
                    if (segment.start_value <= 0 || segment.end_value <= 0)
                        return segment.end_value;
                    f64 const ratio = static_cast<f64>(segment.end_value) / static_cast<f64>(segment.start_value);
                    return static_cast<f32>(static_cast<f64>(segment.start_value) * pow(ratio, pos));
                }
                case GraphAutomationSegmentType::Target: {
                    // value(t) = target + (start-target) * exp(-(t-start)/timeConstant)
                    if (segment.time_constant <= 0)
                        return segment.target;
                    f64 const dt_seconds = sample_time - segment.start_time;
                    f64 const k = exp(-dt_seconds / static_cast<f64>(segment.time_constant));
                    return static_cast<f32>(static_cast<f64>(segment.target) + ((static_cast<f64>(segment.start_value) - static_cast<f64>(segment.target)) * k));
                }
                case GraphAutomationSegmentType::ValueCurve: {
                    if (segment.curve.is_empty())
                        return segment.start_value;
                    if (segment.curve.size() == 1)
                        return segment.curve[0];
                    f64 const curve_duration = segment.curve_duration > 0 ? segment.curve_duration : max(0.0, segment.end_time - segment.start_time);
                    f64 const curve_pos = curve_duration > 0 ? clamp((sample_time - segment.curve_start_time) / curve_duration, 0.0, 1.0) : pos;
                    f64 const scaled = curve_pos * static_cast<f64>(segment.curve.size() - 1);
                    size_t const idx = static_cast<size_t>(floor(scaled));
                    size_t const next = min(idx + 1, segment.curve.size() - 1);
                    f64 const frac = scaled - static_cast<f64>(idx);
                    return static_cast<f32>(static_cast<f64>(segment.curve[idx]) + ((static_cast<f64>(segment.curve[next]) - static_cast<f64>(segment.curve[idx])) * frac));
                }
                }

                return segment.end_value;
            };

            auto add_intrinsic = [&](size_t start_frame) {
                if (state.segments.is_empty()) {
                    for (size_t i = 0; i < m_context.quantum_size; ++i)
                        out[i] += state.initial_value;
                    return;
                }

                // Advance segment cursor to the segment containing start_frame.
                while (state.current_segment_index + 1 < state.segments.size()
                    && state.segments[state.current_segment_index].end_frame <= start_frame) {
                    ++state.current_segment_index;
                }

                if (state.automation_rate == AutomationRate::KRate) {
                    auto const& seg = state.segments[min(state.current_segment_index, state.segments.size() - 1)];
                    f32 const v = evaluate_segment_at_frame(seg, start_frame);
                    for (size_t i = 0; i < m_context.quantum_size; ++i)
                        out[i] += v;
                    return;
                }

                for (size_t i = 0; i < m_context.quantum_size; ++i) {
                    size_t const frame = start_frame + i;
                    while (state.current_segment_index + 1 < state.segments.size()
                        && state.segments[state.current_segment_index].end_frame <= frame) {
                        ++state.current_segment_index;
                    }
                    auto const& seg = state.segments[min(state.current_segment_index, state.segments.size() - 1)];
                    out[i] += evaluate_segment_at_frame(seg, frame);
                }
            };

            add_intrinsic(m_context.current_frame);

            // NaN -> defaultValue and clamp at application time.
            for (size_t i = 0; i < m_context.quantum_size; ++i) {
                if (isnan(out[i]))
                    out[i] = state.default_value;
                out[i] = clamp(out[i], state.min_value, state.max_value);
            }

            // k-rate: sample at first sample-frame for the whole quantum.
            if (state.automation_rate == AutomationRate::KRate) {
                f32 v = out[0];
                v = static_cast<f32>(AK::round(static_cast<f64>(v) * 100000.0) / 100000.0);
                for (size_t i = 0; i < m_context.quantum_size; ++i)
                    out[i] = v;
            }

            // Hide raw input buses from nodes to avoid double-counting.
            for (size_t i = 0; i < connections.size(); ++i)
                buses[i + 1] = nullptr;
        }

        switch (processing_node.kind) {
        case Topology::ProcessingNodeKind::Real:
            node->process(m_context, per_input_buses, per_param_buses);
            break;
        case Topology::ProcessingNodeKind::DelayWriter:
            static_cast<DelayRenderNode*>(node)->process_cycle_writer(m_context, per_input_buses);
            break;
        case Topology::ProcessingNodeKind::DelayReader:
            static_cast<DelayRenderNode*>(node)->process_cycle_reader(m_context, per_param_buses, true);
            break;
        }

        if (processing_node.kind != Topology::ProcessingNodeKind::DelayWriter) {
            auto& per_node_cache = m_cached_outputs[node_index];
            size_t const output_count = node->output_count();
            VERIFY(per_node_cache.size() == output_count);
            for (size_t i = 0; i < output_count; ++i) {
                per_node_cache[i].generation = m_cache_generation;
                per_node_cache[i].bus = &node->output(i);
            }
        }
    }

    m_last_processed_generation = m_cache_generation;
}

void GraphExecutor::rebuild_output_cache_capacity_for_topology()
{
    ASSERT_RENDER_THREAD();
    size_t const node_count = m_topology ? m_topology->nodes.size() : 0;
    m_cached_outputs.resize(node_count);
    for (size_t node_index = 0; node_index < node_count; ++node_index) {
        auto& per_node_cache = m_cached_outputs[node_index];
        RenderNode* node = m_topology->nodes[node_index].render_node;
        size_t const output_count = node ? node->output_count() : 0;
        per_node_cache.resize(output_count);
        for (auto& entry : per_node_cache) {
            entry.generation = 0;
            entry.bus = nullptr;
        }
    }
}

void GraphExecutor::try_commit_pending_topology()
{
    ASSERT_RENDER_THREAD();
    size_t free_slot_index = topology_retired_slot_count;
    for (size_t i = 0; i < topology_retired_slot_count; ++i) {
        if (m_retired_topologies[i].load(AK::MemoryOrder::memory_order_acquire) == nullptr) {
            free_slot_index = i;
            break;
        }
    }

    if (free_slot_index == topology_retired_slot_count) {
        static Atomic<i64> s_last_log_ms { 0 };
        i64 now_ms = AK::MonotonicTime::now().milliseconds();
        i64 last_ms = s_last_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
        if ((now_ms - last_ms) > 1000 && s_last_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed))
            WA_DBGLN("[WebAudio] commit stalled: topology retired slots full");
        return;
    }

    auto* pending = m_pending_topology.load(AK::MemoryOrder::memory_order_acquire);
    if (!pending)
        return;

    pending = m_pending_topology.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    if (!pending)
        return;

    m_retired_topologies[free_slot_index].store(m_topology, AK::MemoryOrder::memory_order_release);
    m_topology = pending;
    m_last_processed_generation = 0;
    rebuild_output_cache_capacity_for_topology();
}

void GraphExecutor::try_commit_pending_parameter_updates()
{
    ASSERT_RENDER_THREAD();
    size_t free_slot_index = topology_retired_slot_count;
    for (size_t i = 0; i < topology_retired_slot_count; ++i) {
        if (m_retired_parameter_updates[i].load(AK::MemoryOrder::memory_order_acquire) == nullptr) {
            free_slot_index = i;
            break;
        }
    }

    if (free_slot_index == topology_retired_slot_count) {
        static Atomic<i64> s_last_log_ms { 0 };
        i64 now_ms = AK::MonotonicTime::now().milliseconds();
        i64 last_ms = s_last_log_ms.load(AK::MemoryOrder::memory_order_relaxed);
        if ((now_ms - last_ms) > 1000 && s_last_log_ms.compare_exchange_strong(last_ms, now_ms, AK::MemoryOrder::memory_order_relaxed))
            WA_DBGLN("[WebAudio] commit stalled: parameter-update retired slots full");
        return;
    }

    auto* pending = m_pending_parameter_updates.load(AK::MemoryOrder::memory_order_acquire);
    if (!pending)
        return;

    pending = m_pending_parameter_updates.exchange(nullptr, AK::MemoryOrder::memory_order_acq_rel);
    if (!pending)
        return;

    VERIFY(pending->nodes_by_index.size() == m_nodes.size());
    for (size_t node_index = 0; node_index < m_nodes.size(); ++node_index) {
        m_nodes[node_index]->apply_description(pending->nodes_by_index[node_index]);
    }

    // Keep intrinsic (value-setter) parameter values in sync with the node descriptions.
    // This matters for audio-rate param connections (modulation): computedValue = intrinsic + sum(param inputs).
    // Automation segments are applied separately via m_param_automation_state[node][param].segments.
    auto update_intrinsic = [&](size_t node_index, size_t param_index, f32 intrinsic_value) {
        if (node_index >= m_param_automation_state.size())
            return;
        if (param_index >= m_param_automation_state[node_index].size())
            return;
        m_param_automation_state[node_index][param_index].initial_value = intrinsic_value;
    };

    for (size_t node_index = 0; node_index < pending->nodes_by_index.size(); ++node_index) {
        auto const node_type = m_node_types_by_index[node_index];
        auto const& node_desc = pending->nodes_by_index[node_index];

        switch (node_type) {
        case GraphNodeType::BiquadFilter:
            if (node_desc.has<BiquadFilterGraphNode>()) {
                node_desc.get<BiquadFilterGraphNode>().update_intrinsic_values([&](size_t param_index, f32 intrinsic_value) {
                    update_intrinsic(node_index, param_index, intrinsic_value);
                });
            }
            break;
        case GraphNodeType::DynamicsCompressor:
            if (node_desc.has<DynamicsCompressorGraphNode>()) {
                node_desc.get<DynamicsCompressorGraphNode>().update_intrinsic_values([&](size_t param_index, f32 intrinsic_value) {
                    update_intrinsic(node_index, param_index, intrinsic_value);
                });
            }
            break;
        case GraphNodeType::Gain:
            if (node_desc.has<GainGraphNode>()) {
                node_desc.get<GainGraphNode>().update_intrinsic_values([&](size_t param_index, f32 intrinsic_value) {
                    update_intrinsic(node_index, param_index, intrinsic_value);
                });
            }
            break;
        case GraphNodeType::Oscillator:
            if (node_desc.has<OscillatorGraphNode>()) {
                node_desc.get<OscillatorGraphNode>().update_intrinsic_values([&](size_t param_index, f32 intrinsic_value) {
                    update_intrinsic(node_index, param_index, intrinsic_value);
                });
            }
            break;
        case GraphNodeType::AudioBufferSource:
            if (node_desc.has<AudioBufferSourceGraphNode>()) {
                node_desc.get<AudioBufferSourceGraphNode>().update_intrinsic_values([&](size_t param_index, f32 intrinsic_value) {
                    update_intrinsic(node_index, param_index, intrinsic_value);
                });
            }
            break;
        case GraphNodeType::ConstantSource:
            if (node_desc.has<ConstantSourceGraphNode>()) {
                node_desc.get<ConstantSourceGraphNode>().update_intrinsic_values([&](size_t param_index, f32 intrinsic_value) {
                    update_intrinsic(node_index, param_index, intrinsic_value);
                });
            }
            break;
        case GraphNodeType::Delay:
            if (node_desc.has<DelayGraphNode>()) {
                node_desc.get<DelayGraphNode>().update_intrinsic_values([&](size_t param_index, f32 intrinsic_value) {
                    update_intrinsic(node_index, param_index, intrinsic_value);
                });
            }
            break;
        case GraphNodeType::AudioListener:
            if (node_desc.has<AudioListenerGraphNode>()) {
                node_desc.get<AudioListenerGraphNode>().update_intrinsic_values([&](size_t param_index, f32 intrinsic_value) {
                    update_intrinsic(node_index, param_index, intrinsic_value);
                });
            }
            break;
        case GraphNodeType::Panner:
            if (node_desc.has<PannerGraphNode>()) {
                node_desc.get<PannerGraphNode>().update_intrinsic_values([&](size_t param_index, f32 intrinsic_value) {
                    update_intrinsic(node_index, param_index, intrinsic_value);
                });
            }
            break;
        case GraphNodeType::StereoPanner:
            if (node_desc.has<StereoPannerGraphNode>()) {
                node_desc.get<StereoPannerGraphNode>().update_intrinsic_values([&](size_t param_index, f32 intrinsic_value) {
                    update_intrinsic(node_index, param_index, intrinsic_value);
                });
            }
            break;
        default:
            break;
        }
    }

    // Parameter updates may include automation timeline changes.
    for (size_t node_index = 0; node_index < m_param_automation_state.size(); ++node_index) {
        for (size_t param_index = 0; param_index < m_param_automation_state[node_index].size(); ++param_index) {
            auto& state = m_param_automation_state[node_index][param_index];
            state.current_segment_index = 0;
            state.segments.clear();
        }
    }
    for (auto const& automation : pending->param_automations) {
        auto node_index_it = m_node_index_by_id.find(automation.destination);
        if (node_index_it == m_node_index_by_id.end())
            continue;
        auto const node_index = node_index_it->value;
        if (automation.destination_param_index >= m_param_automation_state[node_index].size())
            continue;

        auto& state = m_param_automation_state[node_index][automation.destination_param_index];
        state.initial_value = automation.initial_value;
        state.default_value = automation.default_value;
        state.min_value = automation.min_value;
        state.max_value = automation.max_value;
        state.automation_rate = automation.automation_rate;
        state.current_segment_index = 0;
        state.segments = automation.segments;
    }

    // A parameter update may affect rendered output even within the current cache generation.
    // Ensure the next destination render re-processes the graph.
    m_last_processed_generation = 0;

    m_retired_parameter_updates[free_slot_index].store(pending, AK::MemoryOrder::memory_order_release);
}

}
