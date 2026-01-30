/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/QuickSort.h>
#include <AK/StdLibExtras.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/WebAudio/AnalyserNode.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioBufferSourceNode.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/AudioParamMap.h>
#include <LibWeb/WebAudio/AudioWorkletNode.h>
#include <LibWeb/WebAudio/BiquadFilterNode.h>
#include <LibWeb/WebAudio/ChannelMergerNode.h>
#include <LibWeb/WebAudio/ChannelSplitterNode.h>
#include <LibWeb/WebAudio/ConstantSourceNode.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/DelayNode.h>
#include <LibWeb/WebAudio/DynamicsCompressorNode.h>
#include <LibWeb/WebAudio/Engine/GraphResources.h>
#include <LibWeb/WebAudio/GainNode.h>
#include <LibWeb/WebAudio/MediaElementAudioSourceNode.h>
#include <LibWeb/WebAudio/MediaElementAudioSourceProvider.h>
#include <LibWeb/WebAudio/OhNoesNode.h>
#include <LibWeb/WebAudio/OscillatorNode.h>
#include <LibWeb/WebAudio/RenderGraphSnapshot.h>
#include <LibWeb/WebAudio/ScriptProcessorNode.h>
#include <LibWeb/WebAudio/StereoPannerNode.h>

#include <cmath>

namespace Web::WebAudio {

using namespace Render;

static OscillatorType to_render_oscillator_type(Bindings::OscillatorType type)
{
    switch (type) {
    case Bindings::OscillatorType::Sine:
        return OscillatorType::Sine;
    case Bindings::OscillatorType::Square:
        return OscillatorType::Square;
    case Bindings::OscillatorType::Sawtooth:
        return OscillatorType::Sawtooth;
    case Bindings::OscillatorType::Triangle:
        return OscillatorType::Triangle;
    case Bindings::OscillatorType::Custom:
        return OscillatorType::Custom;
    }

    return OscillatorType::Sine;
}

static ChannelCountMode to_render_channel_count_mode(Bindings::ChannelCountMode mode)
{
    switch (mode) {
    case Bindings::ChannelCountMode::Max:
        return ChannelCountMode::Max;
    case Bindings::ChannelCountMode::ClampedMax:
        return ChannelCountMode::ClampedMax;
    case Bindings::ChannelCountMode::Explicit:
        return ChannelCountMode::Explicit;
    }

    return ChannelCountMode::Max;
}

static ChannelInterpretation to_render_channel_interpretation(Bindings::ChannelInterpretation interpretation)
{
    switch (interpretation) {
    case Bindings::ChannelInterpretation::Speakers:
        return ChannelInterpretation::Speakers;
    case Bindings::ChannelInterpretation::Discrete:
        return ChannelInterpretation::Discrete;
    }

    return ChannelInterpretation::Speakers;
}

static AutomationRate to_render_automation_rate(Bindings::AutomationRate rate)
{
    switch (rate) {
    case Bindings::AutomationRate::ARate:
        return AutomationRate::ARate;
    case Bindings::AutomationRate::KRate:
        return AutomationRate::KRate;
    }

    return AutomationRate::ARate;
}

static BiquadFilterType to_render_biquad_filter_type(Bindings::BiquadFilterType type)
{
    switch (type) {
    case Bindings::BiquadFilterType::Lowpass:
        return BiquadFilterType::Lowpass;
    case Bindings::BiquadFilterType::Highpass:
        return BiquadFilterType::Highpass;
    case Bindings::BiquadFilterType::Bandpass:
        return BiquadFilterType::Bandpass;
    case Bindings::BiquadFilterType::Lowshelf:
        return BiquadFilterType::Lowshelf;
    case Bindings::BiquadFilterType::Highshelf:
        return BiquadFilterType::Highshelf;
    case Bindings::BiquadFilterType::Peaking:
        return BiquadFilterType::Peaking;
    case Bindings::BiquadFilterType::Notch:
        return BiquadFilterType::Notch;
    case Bindings::BiquadFilterType::Allpass:
        return BiquadFilterType::Allpass;
    }

    return BiquadFilterType::Lowpass;
}

static size_t seconds_to_frames_clamped(double seconds, double sample_rate)
{
    if (!(sample_rate > 0))
        return 0;

    if (!isfinite(seconds))
        return seconds > 0 ? AK::NumericLimits<size_t>::max() : 0;

    if (seconds <= 0)
        return 0;

    double const max_seconds = static_cast<double>(AK::NumericLimits<size_t>::max()) / sample_rate;
    if (seconds >= max_seconds)
        return AK::NumericLimits<size_t>::max();

    return static_cast<size_t>(seconds * sample_rate);
}

static size_t seconds_to_frames_ceil_clamped(double seconds, double sample_rate)
{
    if (!(sample_rate > 0))
        return 0;

    if (!isfinite(seconds))
        return seconds > 0 ? AK::NumericLimits<size_t>::max() : 0;

    if (seconds <= 0)
        return 0;

    double const max_seconds = static_cast<double>(AK::NumericLimits<size_t>::max()) / sample_rate;
    if (seconds >= max_seconds)
        return AK::NumericLimits<size_t>::max();

    // Ceil to ensure segments that span a fractional frame still cover at least one sample.
    double const exact = seconds * sample_rate;
    double const rounded = ceil(exact);
    if (rounded >= static_cast<double>(AK::NumericLimits<size_t>::max()))
        return AK::NumericLimits<size_t>::max();
    return static_cast<size_t>(rounded);
}

static Optional<size_t> seconds_to_context_frames(Optional<double> seconds, double context_sample_rate)
{
    if (!seconds.has_value())
        return {};
    if (seconds.value() < 0)
        return {};
    return seconds_to_frames_clamped(seconds.value(), context_sample_rate);
}

static size_t seconds_to_buffer_frames(Optional<double> seconds, Optional<f32> buffer_sample_rate)
{
    if (!seconds.has_value() || seconds.value() <= 0)
        return 0;
    if (!buffer_sample_rate.has_value() || buffer_sample_rate.value() <= 0)
        return 0;
    return seconds_to_frames_clamped(seconds.value(), static_cast<double>(buffer_sample_rate.value()));
}

GraphDescription snapshot_render_graph(
    GC::Ref<AudioNode> destination_node,
    double context_sample_rate,
    HashMap<NodeID, GC::Ref<AnalyserNode>>* analyser_nodes_out,
    HashMap<NodeID, GC::Ref<AudioWorkletNode>>* audio_worklet_nodes_out,
    HashMap<NodeID, GC::Ref<ScriptProcessorNode>>* script_processor_nodes_out,
    Render::GraphResourceRegistry* resources_out)
{
    ASSERT_CONTROL_THREAD();
    GraphDescription graph;
    graph.destination_node_id = destination_node->node_id();

    HashMap<AudioBuffer const*, u64> buffer_id_by_buffer;
    u64 next_buffer_id = 1;

    HashTable<NodeID> visited;
    Vector<GC::Ref<AudioNode>> visited_nodes;

    GC::Ref<BaseAudioContext> context = destination_node->context();
    for (auto const& weak_node : context->audio_nodes_for_snapshot()) {
        if (!weak_node)
            continue;
        AudioNode& node = *weak_node.ptr();
        NodeID const node_id = node.node_id();
        if (visited.contains(node_id))
            continue;
        visited.set(node_id);
        visited_nodes.append(GC::Ref<AudioNode> { node });
    }

    auto append_param_automation = [&](NodeID destination_node_id, size_t destination_param_index, GC::Ref<AudioParam const> param) {
        // Snapshot minimal automation timeline for this param.
        // https://webaudio.github.io/web-audio-api/#computation-of-value
        // FIXME: This is a best-effort segment snapshot of the control-thread automation timeline.
        // It is used by the render thread to compute the intrinsic/timeline portion of computedValue.
        // While we snapshot multiple automation event types (setValueAtTime, ramps, targets, curves),
        // the segment generation model is still simplified and may diverge from the spec for some
        // edge cases and event interaction rules.
        GraphParamAutomation automation;
        automation.destination = destination_node_id;
        automation.destination_param_index = destination_param_index;
        automation.initial_value = param->unclamped_value();
        automation.default_value = param->default_value();
        automation.min_value = param->min_value();
        automation.max_value = param->max_value();
        automation.automation_rate = to_render_automation_rate(param->automation_rate());

        auto segments = param->generate_automation_segments();
        automation.segments.ensure_capacity(segments.size());
        for (auto const& segment : segments) {
            size_t const start_frame = seconds_to_frames_clamped(segment.start_time, context_sample_rate);
            size_t const end_frame = seconds_to_frames_ceil_clamped(segment.end_time, context_sample_rate);

            GraphAutomationSegment render_segment;
            render_segment.start_time = segment.start_time;
            render_segment.end_time = segment.end_time;
            switch (segment.type) {
            case AudioParam::AutomationSegment::Type::Constant:
                render_segment.type = GraphAutomationSegmentType::Constant;
                break;
            case AudioParam::AutomationSegment::Type::LinearRamp:
                render_segment.type = GraphAutomationSegmentType::LinearRamp;
                break;
            case AudioParam::AutomationSegment::Type::ExponentialRamp:
                render_segment.type = GraphAutomationSegmentType::ExponentialRamp;
                break;
            case AudioParam::AutomationSegment::Type::Target:
                render_segment.type = GraphAutomationSegmentType::Target;
                break;
            case AudioParam::AutomationSegment::Type::ValueCurve:
                render_segment.type = GraphAutomationSegmentType::ValueCurve;
                break;
            }

            render_segment.start_frame = start_frame;
            render_segment.end_frame = end_frame;
            render_segment.start_value = segment.start_value;
            render_segment.end_value = segment.end_value;
            render_segment.time_constant = segment.time_constant;
            render_segment.target = segment.target;
            render_segment.curve.ensure_capacity(segment.curve.size());
            for (auto v : segment.curve)
                render_segment.curve.append(v);

            automation.segments.append(move(render_segment));
        }

        graph.param_automations.append(move(automation));
    };

    for (auto const& node : visited_nodes) {
        NodeID const node_id = node->node_id();

        GraphNodeDescription node_description = OhNoesGraphNode {};
        if (is<AudioDestinationNode>(*node)) {
            DestinationGraphNode dest_desc;
            dest_desc.channel_count = static_cast<size_t>(node->channel_count());
            node_description = dest_desc;
        } else if (is<OscillatorNode>(*node)) {
            OscillatorNode const& oscillator = static_cast<OscillatorNode const&>(*node);
            OscillatorGraphNode osc_desc;
            osc_desc.type = to_render_oscillator_type(oscillator.type());
            osc_desc.frequency = oscillator.frequency()->value();
            osc_desc.detune_cents = oscillator.detune()->value();
            AudioScheduledSourceNode const& scheduled = static_cast<AudioScheduledSourceNode const&>(*node);
            osc_desc.start_frame = seconds_to_context_frames(scheduled.start_when_for_rendering(), context_sample_rate);
            osc_desc.stop_frame = seconds_to_context_frames(scheduled.stop_when_for_rendering(), context_sample_rate);
            node_description = osc_desc;
        } else if (is<GainNode>(*node)) {
            GainNode const& gain_node = static_cast<GainNode const&>(*node);
            GainGraphNode gain_desc;
            gain_desc.gain = gain_node.gain()->value();
            gain_desc.channel_count = static_cast<size_t>(gain_node.channel_count());
            gain_desc.channel_count_mode = to_render_channel_count_mode(gain_node.channel_count_mode());
            gain_desc.channel_interpretation = to_render_channel_interpretation(gain_node.channel_interpretation());
            node_description = gain_desc;
        } else if (is<DelayNode>(*node)) {
            DelayNode const& delay_node = static_cast<DelayNode const&>(*node);
            DelayGraphNode delay_desc;
            delay_desc.delay_time_seconds = delay_node.delay_time()->value();
            delay_desc.max_delay_time_seconds = delay_node.delay_time()->max_value();
            delay_desc.channel_count = static_cast<size_t>(delay_node.channel_count());
            delay_desc.channel_count_mode = to_render_channel_count_mode(delay_node.channel_count_mode());
            delay_desc.channel_interpretation = to_render_channel_interpretation(delay_node.channel_interpretation());
            node_description = delay_desc;
        } else if (is<DynamicsCompressorNode>(*node)) {
            DynamicsCompressorNode const& compressor_node = static_cast<DynamicsCompressorNode const&>(*node);
            DynamicsCompressorGraphNode compressor_desc;
            compressor_desc.threshold_db = compressor_node.threshold()->value();
            compressor_desc.knee_db = compressor_node.knee()->value();
            compressor_desc.ratio = compressor_node.ratio()->value();
            compressor_desc.attack_seconds = compressor_node.attack()->value();
            compressor_desc.release_seconds = compressor_node.release()->value();
            compressor_desc.channel_count = static_cast<size_t>(compressor_node.channel_count());
            compressor_desc.channel_count_mode = to_render_channel_count_mode(compressor_node.channel_count_mode());
            compressor_desc.channel_interpretation = to_render_channel_interpretation(compressor_node.channel_interpretation());
            node_description = compressor_desc;
        } else if (is<StereoPannerNode>(*node)) {
            StereoPannerNode const& panner_node = static_cast<StereoPannerNode const&>(*node);
            StereoPannerGraphNode panner_desc;
            panner_desc.pan = panner_node.pan()->value();
            panner_desc.channel_count = static_cast<size_t>(panner_node.channel_count());
            panner_desc.channel_count_mode = to_render_channel_count_mode(panner_node.channel_count_mode());
            panner_desc.channel_interpretation = to_render_channel_interpretation(panner_node.channel_interpretation());
            node_description = panner_desc;
        } else if (is<ChannelSplitterNode>(*node)) {
            ChannelSplitterNode& splitter_node = static_cast<ChannelSplitterNode&>(*node);
            ChannelSplitterGraphNode splitter_desc;
            splitter_desc.number_of_outputs = static_cast<size_t>(splitter_node.number_of_outputs());
            node_description = splitter_desc;
        } else if (is<ChannelMergerNode>(*node)) {
            ChannelMergerNode& merger_node = static_cast<ChannelMergerNode&>(*node);
            ChannelMergerGraphNode merger_desc;
            merger_desc.number_of_inputs = static_cast<size_t>(merger_node.number_of_inputs());
            node_description = merger_desc;
        } else if (is<AudioBufferSourceNode>(*node)) {
            AudioBufferSourceNode const& buffer_source = static_cast<AudioBufferSourceNode const&>(*node);

            AudioBufferSourceGraphNode buffer_desc;
            AudioScheduledSourceNode const& scheduled = static_cast<AudioScheduledSourceNode const&>(*node);

            buffer_desc.playback_rate = buffer_source.playback_rate()->value();
            buffer_desc.detune_cents = buffer_source.detune()->value();

            buffer_desc.loop = buffer_source.loop();

            buffer_desc.start_frame = seconds_to_context_frames(buffer_source.start_when_for_rendering(), context_sample_rate);
            buffer_desc.stop_frame = seconds_to_context_frames(scheduled.stop_when_for_rendering(), context_sample_rate);

            Optional<f32> buffer_sample_rate;
            if (auto buffer = buffer_source.buffer())
                buffer_sample_rate = static_cast<f32>(buffer->sample_rate());

            buffer_desc.offset_frame = seconds_to_buffer_frames(buffer_source.start_offset_for_rendering(), buffer_sample_rate);
            if (auto duration = buffer_source.start_duration_for_rendering(); duration.has_value() && buffer_sample_rate.has_value())
                buffer_desc.duration_in_sample_frames = seconds_to_buffer_frames(duration, buffer_sample_rate);

            buffer_desc.loop_start_frame = seconds_to_buffer_frames(buffer_source.loop_start(), buffer_sample_rate);
            buffer_desc.loop_end_frame = seconds_to_buffer_frames(buffer_source.loop_end(), buffer_sample_rate);

            if (auto buffer = buffer_source.buffer()) {
                buffer_desc.sample_rate = static_cast<f32>(buffer->sample_rate());
                buffer_desc.channel_count = static_cast<size_t>(buffer->number_of_channels());
                buffer_desc.length_in_sample_frames = static_cast<size_t>(buffer->length());

                // AudioBuffer sample payloads are captured into the resource registry and referenced by id.
                if (resources_out) {
                    auto const* buffer_ptr = buffer.ptr();
                    u64 buffer_id = 0;
                    if (auto it = buffer_id_by_buffer.find(buffer_ptr); it != buffer_id_by_buffer.end()) {
                        buffer_id = it->value;
                    } else {
                        buffer_id = next_buffer_id++;
                        buffer_id_by_buffer.set(buffer_ptr, buffer_id);

                        Vector<Vector<f32>> channels;
                        channels.resize(buffer_desc.channel_count);
                        for (size_t channel_index = 0; channel_index < buffer_desc.channel_count; ++channel_index) {
                            channels[channel_index].resize(buffer_desc.length_in_sample_frames);
                            for (size_t i = 0; i < channels[channel_index].size(); ++i)
                                channels[channel_index][i] = 0.0f;
                            auto channel_data_or_exception = buffer->get_channel_data(channel_index);
                            if (channel_data_or_exception.is_exception())
                                continue;
                            auto typed_array = channel_data_or_exception.release_value();
                            auto span = typed_array->data();
                            size_t const copy_count = min(span.size(), channels[channel_index].size());
                            for (size_t i = 0; i < copy_count; ++i)
                                channels[channel_index][i] = span[i];
                        }

                        auto shared = Render::SharedAudioBuffer::create(buffer_desc.sample_rate, buffer_desc.channel_count, buffer_desc.length_in_sample_frames, move(channels));
                        resources_out->set_audio_buffer(buffer_id, move(shared));
                    }

                    buffer_desc.buffer_id = buffer_id;
                }
            }

            node_description = buffer_desc;
        } else if (is<MediaElementAudioSourceNode>(*node)) {
            MediaElementAudioSourceNode const& source_node = static_cast<MediaElementAudioSourceNode const&>(*node);

            // https://webaudio.github.io/web-audio-api/#mediaelementaudiosourcenode
            // The output of this node is the audio from the associated HTMLMediaElement.
            // Best-effort: snapshot the provider's current channel count. The render node clamps
            // this to a preallocated capacity and keeps at least one output channel.
            auto const provider_channels = source_node.provider()->channel_count();
            size_t const channel_count = max<size_t>(1, provider_channels);

            auto provider = source_node.provider();
            MediaElementAudioSourceGraphNode source_desc;
            source_desc.provider_id = provider->provider_id();
            source_desc.channel_count = channel_count;
            node_description = source_desc;

            if (resources_out)
                resources_out->set_media_element_audio_source(provider->provider_id(), move(provider));
        } else if (is<ConstantSourceNode>(*node)) {
            ConstantSourceNode const& constant_source = static_cast<ConstantSourceNode const&>(*node);

            ConstantSourceGraphNode constant_desc;
            AudioScheduledSourceNode const& scheduled = static_cast<AudioScheduledSourceNode const&>(*node);
            constant_desc.start_frame = seconds_to_context_frames(scheduled.start_when_for_rendering(), context_sample_rate);
            constant_desc.stop_frame = seconds_to_context_frames(scheduled.stop_when_for_rendering(), context_sample_rate);
            constant_desc.offset = constant_source.offset()->value();
            node_description = constant_desc;
        } else if (is<BiquadFilterNode>(*node)) {
            BiquadFilterNode const& filter_node = static_cast<BiquadFilterNode const&>(*node);

            BiquadFilterGraphNode filter_desc;
            filter_desc.type = to_render_biquad_filter_type(filter_node.type());
            filter_desc.frequency_hz = filter_node.frequency()->value();
            filter_desc.detune_cents = filter_node.detune()->value();
            filter_desc.q = filter_node.q()->value();
            filter_desc.gain_db = filter_node.gain()->value();
            filter_desc.channel_count = static_cast<size_t>(filter_node.channel_count());
            filter_desc.channel_count_mode = to_render_channel_count_mode(filter_node.channel_count_mode());
            filter_desc.channel_interpretation = to_render_channel_interpretation(filter_node.channel_interpretation());
            node_description = filter_desc;
        } else if (is<StereoPannerNode>(*node)) {
            StereoPannerNode const& panner_node = static_cast<StereoPannerNode const&>(*node);

            StereoPannerGraphNode panner_desc;
            panner_desc.pan = static_cast<f32>(panner_node.pan()->value());
            panner_desc.channel_count = static_cast<size_t>(panner_node.channel_count());
            panner_desc.channel_count_mode = to_render_channel_count_mode(panner_node.channel_count_mode());
            panner_desc.channel_interpretation = to_render_channel_interpretation(panner_node.channel_interpretation());
            node_description = panner_desc;
        } else if (is<AnalyserNode>(*node)) {
            AnalyserNode& analyser_node = static_cast<AnalyserNode&>(*node);
            AnalyserGraphNode analyser_desc;
            analyser_desc.channel_count = static_cast<size_t>(analyser_node.channel_count());
            analyser_desc.channel_count_mode = to_render_channel_count_mode(analyser_node.channel_count_mode());
            analyser_desc.channel_interpretation = to_render_channel_interpretation(analyser_node.channel_interpretation());
            analyser_desc.fft_size = static_cast<size_t>(analyser_node.fft_size());
            analyser_desc.smoothing_time_constant = static_cast<f32>(analyser_node.smoothing_time_constant());
            node_description = analyser_desc;
            if (analyser_nodes_out)
                analyser_nodes_out->set(node_id, analyser_node);
        } else if (is<AudioWorkletNode>(*node)) {
            AudioWorkletNode& worklet_node = static_cast<AudioWorkletNode&>(*node);

            AudioWorkletGraphNode worklet_desc;
            worklet_desc.number_of_inputs = static_cast<size_t>(worklet_node.number_of_inputs());
            worklet_desc.number_of_outputs = static_cast<size_t>(worklet_node.number_of_outputs());
            worklet_desc.output_channel_count = worklet_node.output_channel_count();
            worklet_desc.channel_count = static_cast<size_t>(worklet_node.channel_count());
            worklet_desc.channel_count_mode = to_render_channel_count_mode(worklet_node.channel_count_mode());
            worklet_desc.channel_interpretation = to_render_channel_interpretation(worklet_node.channel_interpretation());
            worklet_desc.processor_name = worklet_node.name(); // Added line for processor name

            // Snapshot AudioWorkletNode parameters in a stable order.
            // We use this ordering as the destination_param_index namespace for automation.
            Vector<String> parameter_names;
            Vector<GC::Ref<AudioParam>> parameters_in_order;

            auto parameters = worklet_node.parameters();
            auto collect_result = parameters->for_each([&](auto const& key, GC::Ref<AudioParam> param) -> JS::ThrowCompletionOr<void> {
                parameter_names.append(key.to_string());
                parameters_in_order.append(param);
                return {};
            });

            if (!collect_result.is_error()) {
                // Sort by name to keep deterministic indices across snapshots.
                Vector<size_t> indices;
                indices.ensure_capacity(parameter_names.size());
                for (size_t i = 0; i < parameter_names.size(); ++i)
                    indices.append(i);
                quick_sort(indices, [&](size_t a, size_t b) {
                    return parameter_names[a] < parameter_names[b];
                });

                worklet_desc.parameter_names.ensure_capacity(indices.size());

                for (size_t sorted_index = 0; sorted_index < indices.size(); ++sorted_index) {
                    size_t const original_index = indices[sorted_index];
                    worklet_desc.parameter_names.append(parameter_names[original_index]);

                    auto const& param = parameters_in_order[original_index];

                    // Worklet parameters use destination_param_index as an index into parameter_names.
                    append_param_automation(node_id, sorted_index, param);
                }
            }

            node_description = move(worklet_desc);

            if (audio_worklet_nodes_out)
                audio_worklet_nodes_out->set(node_id, worklet_node);
        } else if (is<ScriptProcessorNode>(*node)) {
            ScriptProcessorNode& script_processor_node = static_cast<ScriptProcessorNode&>(*node);

            ScriptProcessorGraphNode script_processor_desc;
            script_processor_desc.buffer_size = static_cast<size_t>(script_processor_node.buffer_size());
            script_processor_desc.input_channel_count = static_cast<size_t>(script_processor_node.number_of_input_channels());
            script_processor_desc.output_channel_count = static_cast<size_t>(script_processor_node.number_of_output_channels());
            node_description = script_processor_desc;

            if (script_processor_nodes_out)
                script_processor_nodes_out->set(node_id, script_processor_node);
        } else if (is<OhNoesNode>(*node)) {
            OhNoesNode const& oh_noes_node = static_cast<OhNoesNode const&>(*node);

            OhNoesGraphNode oh_noes_desc;
            oh_noes_desc.base_path = oh_noes_node.base_path_for_rendering();
            oh_noes_desc.emit_enabled = oh_noes_node.emit_enabled_for_rendering();
            oh_noes_desc.strip_zero_buffers = oh_noes_node.strip_zero_buffers_for_rendering();
            node_description = move(oh_noes_desc);
        } else {
            VERIFY_NOT_REACHED();
        }

        graph.nodes.set(node_id, move(node_description));

        for (AudioNodeConnection const& connection : node->input_connections()) {
            NodeID const source_id = connection.destination_node->node_id();
            GraphConnection rc;
            rc.source = source_id;
            rc.destination = node_id;
            rc.source_output_index = static_cast<size_t>(connection.output);
            rc.destination_input_index = static_cast<size_t>(connection.input);
            graph.connections.append(rc);
        }
    }

    // Snapshot audio-rate AudioParam connections as typed param edges.
    // https://webaudio.github.io/web-audio-api/#dom-audionode-connect-destinationparam-output
    // https://webaudio.github.io/web-audio-api/#rendering-loop
    // An AudioParam mixes its intrinsic/timeline value with the summed/downmixed output
    //            of any AudioNodes connected to it (see "rendering a render quantum", step 4.4.1).
    // NOTE: The realtime render graph uses a dedicated implicit automation bus per param to model
    // the intrinsic/timeline portion. This snapshot collects both param connections and a minimal
    // automation timeline subset to be applied on the render thread.
    struct ParamEndpoint {
        NodeID node_id;
        GraphNodeType node_type;
        size_t param_index { 0 };
    };

    HashMap<FlatPtr, ParamEndpoint> param_endpoints;
    for (auto const& node : visited_nodes) {
        auto register_param = [&](GC::Ref<AudioParam const> param, GraphNodeType node_type, size_t param_index) {
            VERIFY(param_index < RenderParamLayout::param_count(node_type));
            param_endpoints.set(reinterpret_cast<FlatPtr>(param.ptr()), ParamEndpoint { node->node_id(), node_type, param_index });

            append_param_automation(node->node_id(), param_index, param);
        };

        if (is<AudioWorkletNode>(*node)) {
            auto const& worklet_node = static_cast<AudioWorkletNode const&>(*node);

            Vector<String> parameter_names;
            Vector<GC::Ref<AudioParam>> parameters_in_order;

            auto collect_result = worklet_node.parameters()->for_each([&](auto const& key, GC::Ref<AudioParam> param) -> JS::ThrowCompletionOr<void> {
                parameter_names.append(key.to_string());
                parameters_in_order.append(param);
                return {};
            });

            if (!collect_result.is_error()) {
                Vector<size_t> indices;
                indices.ensure_capacity(parameter_names.size());
                for (size_t i = 0; i < parameter_names.size(); ++i)
                    indices.append(i);

                quick_sort(indices, [&](size_t a, size_t b) {
                    return parameter_names[a] < parameter_names[b];
                });

                for (size_t sorted_index = 0; sorted_index < indices.size(); ++sorted_index) {
                    size_t const original_index = indices[sorted_index];
                    auto const& param = parameters_in_order[original_index];
                    param_endpoints.set(reinterpret_cast<FlatPtr>(param.ptr()), ParamEndpoint { node->node_id(), GraphNodeType::AudioWorklet, sorted_index });
                }
            }

            continue;
        }

        if (is<GainNode>(*node)) {
            auto const& gain_node = static_cast<GainNode const&>(*node);
            register_param(gain_node.gain(), GraphNodeType::Gain, GainParamIndex::gain);
        } else if (is<BiquadFilterNode>(*node)) {
            auto const& filter_node = static_cast<BiquadFilterNode const&>(*node);
            register_param(filter_node.frequency(), GraphNodeType::BiquadFilter, BiquadFilterParamIndex::frequency);
            register_param(filter_node.detune(), GraphNodeType::BiquadFilter, BiquadFilterParamIndex::detune);
            register_param(filter_node.q(), GraphNodeType::BiquadFilter, BiquadFilterParamIndex::q);
            register_param(filter_node.gain(), GraphNodeType::BiquadFilter, BiquadFilterParamIndex::gain);
        } else if (is<DelayNode>(*node)) {
            auto const& delay_node = static_cast<DelayNode const&>(*node);
            register_param(delay_node.delay_time(), GraphNodeType::Delay, DelayParamIndex::delay_time);
        } else if (is<DynamicsCompressorNode>(*node)) {
            auto const& compressor_node = static_cast<DynamicsCompressorNode const&>(*node);
            register_param(compressor_node.threshold(), GraphNodeType::DynamicsCompressor, DynamicsCompressorParamIndex::threshold);
            register_param(compressor_node.knee(), GraphNodeType::DynamicsCompressor, DynamicsCompressorParamIndex::knee);
            register_param(compressor_node.ratio(), GraphNodeType::DynamicsCompressor, DynamicsCompressorParamIndex::ratio);
            register_param(compressor_node.attack(), GraphNodeType::DynamicsCompressor, DynamicsCompressorParamIndex::attack);
            register_param(compressor_node.release(), GraphNodeType::DynamicsCompressor, DynamicsCompressorParamIndex::release);
        } else if (is<StereoPannerNode>(*node)) {
            auto const& panner_node = static_cast<StereoPannerNode const&>(*node);
            register_param(panner_node.pan(), GraphNodeType::StereoPanner, StereoPannerParamIndex::pan);
        } else if (is<OscillatorNode>(*node)) {
            auto const& oscillator_node = static_cast<OscillatorNode const&>(*node);
            register_param(oscillator_node.frequency(), GraphNodeType::Oscillator, OscillatorParamIndex::frequency);
            register_param(oscillator_node.detune(), GraphNodeType::Oscillator, OscillatorParamIndex::detune);
        } else if (is<AudioBufferSourceNode>(*node)) {
            auto const& buffer_source_node = static_cast<AudioBufferSourceNode const&>(*node);
            register_param(buffer_source_node.playback_rate(), GraphNodeType::BufferSource, BufferSourceParamIndex::playback_rate);
            register_param(buffer_source_node.detune(), GraphNodeType::BufferSource, BufferSourceParamIndex::detune);
        } else if (is<ConstantSourceNode>(*node)) {
            auto const& constant_source_node = static_cast<ConstantSourceNode const&>(*node);
            register_param(constant_source_node.offset(), GraphNodeType::ConstantSource, ConstantSourceParamIndex::offset);
        }
    }

    for (auto const& node : visited_nodes) {
        for (AudioParamConnection const& connection : node->param_connections()) {
            auto endpoint_it = param_endpoints.find(reinterpret_cast<FlatPtr>(connection.destination_param.ptr()));
            if (endpoint_it == param_endpoints.end())
                continue;

            GraphParamConnection pc;
            pc.source = node->node_id();
            pc.destination = endpoint_it->value.node_id;
            pc.source_output_index = static_cast<size_t>(connection.output);
            pc.destination_param_index = endpoint_it->value.param_index;

            if (endpoint_it->value.node_type != GraphNodeType::AudioWorklet)
                VERIFY(pc.destination_param_index < RenderParamLayout::param_count(endpoint_it->value.node_type));
            graph.param_connections.append(pc);
        }
    }

    // Ensure deterministic ordering so realtime RenderGraph updates can be classified reliably.
    quick_sort(graph.connections, [](auto const& a, auto const& b) {
        if (a.source != b.source)
            return a.source < b.source;
        if (a.destination != b.destination)
            return a.destination < b.destination;
        if (a.source_output_index != b.source_output_index)
            return a.source_output_index < b.source_output_index;
        return a.destination_input_index < b.destination_input_index;
    });

    quick_sort(graph.param_connections, [](auto const& a, auto const& b) {
        if (a.source != b.source)
            return a.source < b.source;
        if (a.destination != b.destination)
            return a.destination < b.destination;
        if (a.source_output_index != b.source_output_index)
            return a.source_output_index < b.source_output_index;
        return a.destination_param_index < b.destination_param_index;
    });

    quick_sort(graph.param_automations, [](auto const& a, auto const& b) {
        if (a.destination != b.destination)
            return a.destination < b.destination;
        return a.destination_param_index < b.destination_param_index;
    });

    return graph;
}

}
