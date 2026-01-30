/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/StringView.h>
#include <AK/Try.h>
#include <AK/Utf16String.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/ErrorEvent.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/AudioParamMap.h>
#include <LibWeb/WebAudio/AudioWorklet.h>
#include <LibWeb/WebAudio/AudioWorkletGlobalScope.h>
#include <LibWeb/WebAudio/AudioWorkletNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/GraphNodes/GraphNodeTypes.h>
#include <LibWeb/WebAudio/OfflineAudioContext.h>
#include <LibWeb/WebAudio/Worklet/MessagePortTransport.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

static Render::ChannelCountMode to_render_channel_count_mode(Bindings::ChannelCountMode mode)
{
    switch (mode) {
    case Bindings::ChannelCountMode::Max:
        return Render::ChannelCountMode::Max;
    case Bindings::ChannelCountMode::ClampedMax:
        return Render::ChannelCountMode::ClampedMax;
    case Bindings::ChannelCountMode::Explicit:
        return Render::ChannelCountMode::Explicit;
    }

    return Render::ChannelCountMode::Max;
}

static Render::ChannelInterpretation to_render_channel_interpretation(Bindings::ChannelInterpretation interpretation)
{
    switch (interpretation) {
    case Bindings::ChannelInterpretation::Speakers:
        return Render::ChannelInterpretation::Speakers;
    case Bindings::ChannelInterpretation::Discrete:
        return Render::ChannelInterpretation::Discrete;
    }

    return Render::ChannelInterpretation::Speakers;
}

GC_DEFINE_ALLOCATOR(AudioWorkletNode);

AudioWorkletNode::~AudioWorkletNode()
{
    // NOTE: Avoid making control-thread graph mutations from GC finalizers.
}

// https://webaudio.github.io/web-audio-api/#dom-audioworkletnode-onprocessorerror
GC::Ptr<WebIDL::CallbackType> AudioWorkletNode::onprocessorerror()
{
    return event_handler_attribute(HTML::EventNames::processorerror);
}

void AudioWorkletNode::set_onprocessorerror(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(HTML::EventNames::processorerror, value);
}

WebIDL::ExceptionOr<GC::Ref<AudioWorkletNode>> AudioWorkletNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, String const& name, AudioWorkletNodeOptions const& options)
{
    return construct_impl(realm, context, name, options);
}

// https://webaudio.github.io/web-audio-api/#dom-audioworkletnode-audioworkletnode
WebIDL::ExceptionOr<GC::Ref<AudioWorkletNode>> AudioWorkletNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, String const& name, AudioWorkletNodeOptions const& options)
{
    // 1. If nodeName does not exist as a key in the BaseAudioContext's node name to parameter descriptor map, throw a InvalidStateError exception and abort these steps.
    auto worklet = context->audio_worklet();
    if (worklet->is_processor_registration_failed(name))
        return WebIDL::InvalidStateError::create(realm, "Processor registration failed"_utf16);

    if (!worklet->is_processor_registered(name)) {
        if (!worklet->has_loaded_any_module())
            return WebIDL::InvalidStateError::create(realm, "No AudioWorklet module has been loaded"_utf16);
        return WebIDL::InvalidStateError::create(realm, "Processor name is not registered"_utf16);
    }

    // 2. Let node be this value.
    // 3. Initialize the AudioNode node with context and options as arguments.

    // 4. Configure input, output and output channels of node with options. Abort the remaining steps if any exception is thrown.
    if (options.number_of_inputs == 0 && options.number_of_outputs == 0)
        return WebIDL::NotSupportedError::create(realm, "AudioWorkletNode must have at least one input or output"_utf16);

    // https://webaudio.github.io/web-audio-api/#configuring-channels-with-audioworkletnodeoptions
    if (options.output_channel_count.has_value()) {
        auto const& output_channel_count = options.output_channel_count.value();
        for (auto const channel_count : output_channel_count) {
            if (channel_count == 0 || channel_count > BaseAudioContext::MAX_NUMBER_OF_CHANNELS)
                return WebIDL::NotSupportedError::create(realm, "Invalid output channel count"_utf16);
        }

        if (output_channel_count.size() != options.number_of_outputs)
            return WebIDL::IndexSizeError::create(realm, "outputChannelCount must match numberOfOutputs"_utf16);
    }

    // OfflineAudioContext renders worklets on the control thread using the in-process worklet realm.
    // Realtime AudioContext uses a separate worklet VM (in-process render thread or AudioServer).
    bool const is_offline = is<OfflineAudioContext>(*context);

    // 5. Let messageChannel be a new MessageChannel.
    // 6. Let nodePort be the value of messageChannel's port1 attribute.
    // 7. Let processorPortOnThisSide be the value of messageChannel's port2 attribute.
    GC::Ref<HTML::MessagePort> node_port = HTML::MessagePort::create(realm);
    node_port->set_task_source(HTML::Task::Source::AudioWorklet);
    int realtime_processor_port_fd = -1;
    if (!is_offline) {
        auto pair_or_error = Render::create_message_port_transport_pair(realm);
        if (pair_or_error.is_error())
            return WebIDL::InvalidStateError::create(realm, "Failed to allocate AudioWorklet MessagePort transport"_utf16);
        auto pair = pair_or_error.release_value();
        node_port = pair.port;
        realtime_processor_port_fd = pair.peer_fd;
    }

    // 8. Let serializedProcessorPort be the result of StructuredSerializeWithTransfer(processorPortOnThisSide, << processorPortOnThisSide >>).
    // 9. Convert options dictionary to optionsObject.
    // 10. Let serializedOptions be the result of StructuredSerialize(optionsObject).
    auto parameters = AudioParamMap::create(realm);

    Optional<Vector<size_t>> output_channel_count_for_node;
    if (options.output_channel_count.has_value()) {
        Vector<size_t> counts;
        counts.ensure_capacity(options.number_of_outputs);
        for (auto const channel_count : options.output_channel_count.value())
            counts.unchecked_append(static_cast<size_t>(channel_count));
        output_channel_count_for_node = move(counts);
    }

    // 12. Let parameterDescriptors be the result of retrieval of nodeName from node name to parameter descriptor map:
    // 12.1. Let audioParamMap be a new AudioParamMap object.
    // 12.2. For each descriptor of parameterDescriptors:
    // 12.2.1. Let paramName be the value of name member in descriptor.
    // 12.2.2. Let audioParam be a new AudioParam instance with automationRate, defaultValue, minValue, and maxValue having values equal to the values of corresponding members on descriptor.
    // 12.2.3. Append a key-value pair paramName -> audioParam to audioParamMap's entries.
    if (auto const* descriptors = worklet->parameter_descriptors(name)) {
        for (auto const& descriptor : *descriptors) {
            auto param = AudioParam::create(realm, context, descriptor.default_value, descriptor.min_value, descriptor.max_value, descriptor.automation_rate);
            parameters->set(descriptor.name, param);
        }
    }

    // 12.3. If parameterData is present on options, perform the following steps:
    // 12.3.1. Let parameterData be the value of parameterData.
    // 12.3.2. For each paramName -> paramValue of parameterData:
    // 12.3.2.1. If there exists a map entry on audioParamMap with key paramName, let audioParamInMap be such entry.
    // 12.3.2.2. Set value property of audioParamInMap to paramValue.
    if (options.parameter_data.has_value()) {
        for (auto const& it : options.parameter_data.value()) {
            if (auto param = parameters->get(it.key))
                TRY(param->set_value(static_cast<float>(it.value)));
        }
    }

    if (!is_offline) {
        Optional<Vector<Render::WorkletParameterDataEntry>> parameter_data;
        if (options.parameter_data.has_value()) {
            parameter_data = Vector<Render::WorkletParameterDataEntry> {};
            auto& entries = parameter_data.value();
            entries.ensure_capacity(options.parameter_data->size());
            for (auto const& it : options.parameter_data.value())
                entries.unchecked_append(Render::WorkletParameterDataEntry { .name = it.key, .value = it.value });
        }

        Optional<HTML::SerializationRecord> serialized_processor_options;
        if (options.processor_options.has_value())
            serialized_processor_options = TRY(HTML::structured_serialize(realm.vm(), JS::Value(options.processor_options.value().ptr())));

        // 11. Set node's port to nodePort.
        // 12.4. Set node's parameters to audioParamMap.
        auto node = realm.create<AudioWorkletNode>(realm, context, name, options, output_channel_count_for_node, *node_port, parameters);

        if (realtime_processor_port_fd >= 0)
            worklet->set_realtime_processor_port(node->node_id(), realtime_processor_port_fd);

        AudioNodeDefaultOptions default_options;
        default_options.channel_count = 2;
        default_options.channel_count_mode = Bindings::ChannelCountMode::Max;
        default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;

        // Best-effort implementation of the output channel configuration algorithm.
        // LibWeb currently models channelCount as a single per-node value, so we only apply
        // outputChannelCount to the single-output case.
        if (output_channel_count_for_node.has_value() && options.number_of_outputs == 1 && !output_channel_count_for_node->is_empty()) {
            default_options.channel_count = (*output_channel_count_for_node)[0];
            default_options.channel_count_mode = Bindings::ChannelCountMode::Explicit;
        }

        // 13. Configure the AudioNode channel attributes from options.
        TRY(node->initialize_audio_node_options(options, default_options));

        // Track the node definition independently of render-graph reachability.
        // This allows AudioWorkletNode messaging tests to pass even when the
        // node is never connected into the destination graph.
        Vector<String> parameter_names;
        if (auto const* descriptors = worklet->parameter_descriptors(name)) {
            parameter_names.ensure_capacity(descriptors->size());
            for (auto const& descriptor : *descriptors)
                parameter_names.unchecked_append(descriptor.name);
            quick_sort(parameter_names, [](String const& a, String const& b) { return a < b; });
        }

        // 13. Queue a control message to invoke the constructor of the corresponding AudioWorkletProcessor with the processor construction data that consists of: nodeName, node, serializedOptions, and serializedProcessorPort.
        worklet->register_realtime_node_definition(Render::WorkletNodeDefinition {
            .node_id = node->node_id(),
            .processor_name = name,
            .number_of_inputs = static_cast<size_t>(options.number_of_inputs),
            .number_of_outputs = static_cast<size_t>(options.number_of_outputs),
            .output_channel_count = output_channel_count_for_node,
            .channel_count = static_cast<size_t>(node->channel_count()),
            .channel_count_mode = to_render_channel_count_mode(node->channel_count_mode()),
            .channel_interpretation = to_render_channel_interpretation(node->channel_interpretation()),
            .parameter_names = move(parameter_names),
            .parameter_data = move(parameter_data),
            .serialized_processor_options = move(serialized_processor_options),
        });

        context->notify_audio_graph_changed();
        return node;
    }

    auto& worklet_settings_object = worklet->worklet_environment_settings_object();
    GC::Ref<JS::Realm> worklet_realm = worklet_settings_object.realm();

    auto processor_port = HTML::MessagePort::create(*worklet_realm);
    processor_port->set_task_source(HTML::Task::Source::AudioWorklet);
    node_port->entangle_with(*processor_port);

    auto& worklet_global_scope = as<AudioWorkletGlobalScope>(worklet_settings_object.global_object());
    auto processor_constructor = worklet_global_scope.processor_constructor(name);
    if (!processor_constructor.is_function())
        return WebIDL::InvalidStateError::create(realm, "Registered processor constructor is not callable"_utf16);

    Optional<HTML::SerializationRecord> serialized_processor_options;
    if (options.processor_options.has_value())
        serialized_processor_options = TRY(HTML::structured_serialize(realm.vm(), JS::Value(options.processor_options.value().ptr())));

    HTML::TemporaryExecutionContext execution_context(*worklet_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

    auto node_options_object = JS::Object::create(*worklet_realm, worklet_realm->intrinsics().object_prototype());
    TRY(node_options_object->create_data_property_or_throw(JS::PropertyKey { "numberOfInputs"_utf16_fly_string }, JS::Value(options.number_of_inputs)));
    TRY(node_options_object->create_data_property_or_throw(JS::PropertyKey { "numberOfOutputs"_utf16_fly_string }, JS::Value(options.number_of_outputs)));

    if (options.output_channel_count.has_value()) {
        auto const& output_channel_count = options.output_channel_count.value();
        auto output_channel_count_array = TRY(JS::Array::create(*worklet_realm, output_channel_count.size()));
        for (size_t i = 0; i < output_channel_count.size(); ++i)
            TRY(output_channel_count_array->create_data_property_or_throw(JS::PropertyKey { static_cast<u32>(i) }, JS::Value(output_channel_count[i])));
        TRY(node_options_object->create_data_property_or_throw(JS::PropertyKey { "outputChannelCount"_utf16_fly_string }, output_channel_count_array));
    }

    if (options.parameter_data.has_value()) {
        auto parameter_data_object = JS::Object::create(*worklet_realm, worklet_realm->intrinsics().object_prototype());
        for (auto const& it : options.parameter_data.value())
            TRY(parameter_data_object->create_data_property_or_throw(JS::PropertyKey { Utf16String::from_utf8(it.key) }, JS::Value(it.value)));
        TRY(node_options_object->create_data_property_or_throw(JS::PropertyKey { "parameterData"_utf16_fly_string }, parameter_data_object));
    }

    Optional<JS::Value> processor_error;
    if (serialized_processor_options.has_value()) {
        auto deserialized_value_or_error = HTML::structured_deserialize(worklet_realm->vm(), serialized_processor_options.value(), *worklet_realm);
        if (deserialized_value_or_error.is_error()) {
            processor_error = JS::js_undefined();
        } else {
            TRY(node_options_object->create_data_property_or_throw(JS::PropertyKey { "processorOptions"_utf16_fly_string }, deserialized_value_or_error.release_value()));
        }
    }

    JS::Value processor_instance;
    if (!processor_error.has_value()) {
        worklet_global_scope.set_pending_processor_port(processor_port);
        auto processor_instance_or_error = JS::construct(worklet_realm->vm(), processor_constructor.as_function(), JS::Value(node_options_object));
        worklet_global_scope.take_pending_processor_port();

        if (processor_instance_or_error.is_error())
            processor_error = processor_instance_or_error.release_error().value();
        else
            processor_instance = processor_instance_or_error.release_value();
    }

    // 11. Set node's port to nodePort.
    // 12.4. Set node's parameters to audioParamMap.
    auto node = realm.create<AudioWorkletNode>(realm, context, name, options, output_channel_count_for_node, *node_port, parameters);
    if (!processor_error.has_value() && processor_instance.is_object())
        node->m_processor_instance = &processor_instance.as_object();

    AudioNodeDefaultOptions default_options;
    default_options.channel_count = 2;
    default_options.channel_count_mode = Bindings::ChannelCountMode::Max;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;

    // Best-effort implementation of the output channel configuration algorithm.
    // LibWeb currently models channelCount as a single per-node value, so we only apply
    // outputChannelCount to the single-output case.
    if (output_channel_count_for_node.has_value() && options.number_of_outputs == 1 && !output_channel_count_for_node->is_empty()) {
        default_options.channel_count = (*output_channel_count_for_node)[0];
        default_options.channel_count_mode = Bindings::ChannelCountMode::Explicit;
    }

    TRY(node->initialize_audio_node_options(options, default_options));

    if (processor_error.has_value()) {
        auto error_value = processor_error.value();
        context->queue_a_media_element_task("audio worklet processorerror fired"sv, GC::create_function(realm.heap(), [node, error_value]() {
            HTML::ErrorEventInit event_init;
            event_init.error = error_value;
            node->dispatch_event(HTML::ErrorEvent::create(node->realm(), HTML::EventNames::processorerror, event_init));
        }));
    }
    return node;
}

AudioWorkletNode::AudioWorkletNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, String const& name, AudioWorkletNodeOptions const& options, Optional<Vector<size_t>> output_channel_count, HTML::MessagePort& port, AudioParamMap& parameters)
    : AudioNode(realm, context)
    , m_name(name)
    , m_number_of_inputs(options.number_of_inputs)
    , m_number_of_outputs(options.number_of_outputs)
    , m_output_channel_count(move(output_channel_count))
    , m_port(port)
    , m_parameters(parameters)
{
}

void AudioWorkletNode::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioWorkletNode);
    Base::initialize(realm);
}

void AudioWorkletNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_port);
    visitor.visit(m_parameters);
    visitor.visit(m_processor_instance);
}

}
