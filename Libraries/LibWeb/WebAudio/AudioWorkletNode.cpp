/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/HTML/ErrorEvent.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/AudioParamMap.h>
#include <LibWeb/WebAudio/AudioWorklet.h>
#include <LibWeb/WebAudio/AudioWorkletGlobalScope.h>
#include <LibWeb/WebAudio/AudioWorkletNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/Rendering/AudioWorkletRenderNode.h>
#include <LibWeb/WebAudio/Rendering/RealtimeAudioRenderer.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioWorkletNode);

// https://webaudio.github.io/web-audio-api/#dom-audioworkletnode-audioworkletnode
WebIDL::ExceptionOr<GC::Ref<AudioWorkletNode>> AudioWorkletNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, Utf16String const& name, AudioWorkletNodeOptions const& options)
{
    auto& vm = realm.vm();

    // 1. If nodeName does not exist as a key in the BaseAudioContext's node name to parameter descriptor
    //    map, throw an InvalidStateError exception and abort these steps.
    auto worklet = context->audio_worklet();
    auto definition = worklet->find_definition(name);
    if (!definition.has_value())
        return WebIDL::InvalidStateError::create(Utf16String::formatted("No AudioWorkletProcessor named '{}' is registered", name));

    // https://webaudio.github.io/web-audio-api/#configuring-channels-with-audioworkletnodeoptions
    // If both numberOfInputs and numberOfOutputs are zero, throw a NotSupportedError.
    if (options.number_of_inputs == 0 && options.number_of_outputs == 0)
        return WebIDL::NotSupportedError::create("AudioWorkletNode must have at least one input or output"_utf16);

    // If outputChannelCount exists: throw an IndexSizeError if its length does not equal numberOfOutputs;
    // throw a NotSupportedError if any entry is zero or greater than the supported maximum.
    if (options.output_channel_count.has_value()) {
        if (options.output_channel_count->size() != options.number_of_outputs)
            return WebIDL::IndexSizeError::create("outputChannelCount length must equal numberOfOutputs"_utf16);
        for (auto channel_count : *options.output_channel_count) {
            if (channel_count == 0 || channel_count > BaseAudioContext::MAX_NUMBER_OF_CHANNELS)
                return WebIDL::NotSupportedError::create("outputChannelCount entries must be between 1 and 32"_utf16);
        }
    }

    auto worklet_scope_ptr = worklet->audio_worklet_global_scope();
    VERIFY(worklet_scope_ptr);
    auto& worklet_scope = *worklet_scope_ptr;

    // 2. Let node be this value; initialize node's AudioNode base with context and options.
    auto parameter_map = AudioParamMap::create();
    auto node_port = HTML::MessagePort::create(context->relevant_global_event_target());
    auto node = GC::Heap::the().allocate<AudioWorkletNode>(context, name, options.number_of_inputs, options.number_of_outputs, parameter_map, node_port);

    AudioNodeDefaultOptions default_options;
    default_options.channel_count = 2;
    default_options.channel_count_mode = ChannelCountMode::Max;
    default_options.channel_interpretation = ChannelInterpretation::Speakers;
    TRY(node->initialize_audio_node_options(options, default_options));

    // 3. Let map be a new AudioParamMap; for each descriptor in the parameter descriptors, create an
    //    AudioParam and append it to map; apply parameterData overrides.
    for (auto const& descriptor : definition->parameter_descriptors) {
        auto param = AudioParam::create(context, node, descriptor.default_value, descriptor.min_value, descriptor.max_value, descriptor.automation_rate);
        if (options.parameter_data.has_value()) {
            if (auto it = options.parameter_data->find(descriptor.name); it != options.parameter_data->end())
                TRY(param->set_value(static_cast<float>(it->value)));
        }
        parameter_map->set_entry(FlyString(descriptor.name.to_utf8()), param);
    }

    // 4. Let messageChannel be a new MessageChannel (collapsed): node.port and the processor's port are
    //    entangled; node.port's message queue is enabled immediately. The processor-side port stays
    //    disabled until the processor constructor has returned, so messages posted before (or during)
    //    construction are queued and delivered to listeners the constructor registered — even ones added
    //    with addEventListener() and no explicit start().
    auto processor_port = HTML::MessagePort::create(worklet_scope);
    node_port->entangle_with(*processor_port);
    node_port->enable();

    // 5. Let serializedProcessorOptions be StructuredSerialize(optionsObject's processorOptions).
    // 6-7. (Deserialization happens into the worklet realm when building the processor's options object.)
    auto& worklet_realm = worklet_scope.realm();
    GC::Ptr<JS::Object> cloned_processor_options;
    if (options.processor_options) {
        auto record = TRY(HTML::structured_serialize(vm, options.processor_options, HTML::AllowSharedArrayBuffers::CrossOriginIsolatedOnly));
        auto cloned_value = TRY(HTML::structured_deserialize(vm, record, worklet_realm));
        VERIFY(cloned_value.is_object());
        cloned_processor_options = &cloned_value.as_object();
    }

    GC::Ref<JS::Object> options_object = JS::Object::create(worklet_realm, worklet_realm.intrinsics().object_prototype());
    MUST(options_object->create_data_property("numberOfInputs"_utf16_fly_string, JS::Value(options.number_of_inputs)));
    MUST(options_object->create_data_property("numberOfOutputs"_utf16_fly_string, JS::Value(options.number_of_outputs)));
    if (options.output_channel_count.has_value()) {
        auto array = MUST(JS::Array::create(worklet_realm, options.output_channel_count->size()));
        for (size_t i = 0; i < options.output_channel_count->size(); ++i)
            MUST(array->create_data_property(JS::PropertyKey { i }, JS::Value(options.output_channel_count->at(i))));
        MUST(options_object->create_data_property("outputChannelCount"_utf16_fly_string, array));
    }
    if (options.parameter_data.has_value()) {
        auto parameter_data_object = JS::Object::create(worklet_realm, worklet_realm.intrinsics().object_prototype());
        for (auto const& entry : *options.parameter_data)
            MUST(parameter_data_object->create_data_property(JS::PropertyKey { Utf16FlyString(entry.key) }, JS::Value(entry.value)));
        MUST(options_object->create_data_property("parameterData"_utf16_fly_string, parameter_data_object));
    }
    // processorOptions is always present; when omitted by the caller, Web Audio specifies an empty object.
    if (cloned_processor_options)
        MUST(options_object->create_data_property("processorOptions"_utf16_fly_string, cloned_processor_options));
    else
        MUST(options_object->create_data_property("processorOptions"_utf16_fly_string, JS::Object::create(worklet_realm, worklet_realm.intrinsics().object_prototype())));

    Rendering::AudioWorkletPipe::Config pipe_config;
    pipe_config.quantum_size = BaseAudioContext::render_quantum_size();
    pipe_config.sample_rate = context->sample_rate();
    pipe_config.input_count = options.number_of_inputs;
    pipe_config.output_count = options.number_of_outputs;
    // Reserve space for later input layout changes without allocating on the render thread.
    for (WebIDL::UnsignedLong i = 0; i < options.number_of_inputs; ++i)
        pipe_config.input_channel_capacity.append(BaseAudioContext::MAX_NUMBER_OF_CHANNELS);
    pipe_config.output_channel_count_matches_input = options.number_of_inputs == 1
        && options.number_of_outputs == 1 && !options.output_channel_count.has_value();
    for (WebIDL::UnsignedLong i = 0; i < options.number_of_outputs; ++i) {
        if (options.output_channel_count.has_value())
            pipe_config.output_channel_capacity.append(options.output_channel_count->at(i));
        else
            pipe_config.output_channel_capacity.append(pipe_config.output_channel_count_matches_input ? BaseAudioContext::MAX_NUMBER_OF_CHANNELS : 1u);
    }
    pipe_config.param_count = definition->parameter_descriptors.size();
    for (auto const& descriptor : definition->parameter_descriptors)
        pipe_config.param_is_a_rate.append(descriptor.automation_rate == Bindings::AutomationRate::ARate);
    auto ring_sizing = Rendering::AudioWorkletPipe::Config::ring_sizing_for_device_latency(
        Rendering::RealtimeAudioRenderer::TARGET_LATENCY_MS, context->sample_rate(), pipe_config.quantum_size);
    pipe_config.ring_capacity = ring_sizing.ring_capacity;
    pipe_config.prime_level = ring_sizing.prime_level;

    auto pipe = Rendering::AudioWorkletPipe::create(pipe_config, Core::EventLoop::current());
    pipe->prime_outputs_with_silence(pipe_config.prime_level);
    node->m_pipe = pipe;

    // The pump trampoline holds the worklet scope via GC::Root (sanctioned non-GC-memory keepalive);
    // it observes ShutDown through the pipe state and unregisters the slot.
    pipe->set_pump_callback([scope_root = GC::make_root(worklet_scope), pipe, node_id = node->node_id()] {
        scope_root->pump(node_id, *pipe);
    });

    // Create the render mirror. Channel configuration rides in the constructor (the graph drops
    // SetChannelConfig messages that precede AddNode).
    Vector<NonnullRefPtr<Rendering::RenderAudioParam>> render_params;
    for (auto const& entry : parameter_map->entries())
        render_params.append(entry.value->render_param());
    node->queue_render_node_creation(make<Rendering::AudioWorkletRenderNode>(node->node_id(),
        options.number_of_inputs, options.number_of_outputs, pipe_config.quantum_size,
        pipe_config.output_channel_capacity,
        node->channel_count(), node->channel_count_mode(), node->channel_interpretation(),
        pipe, move(render_params)));

    // FIXME: Implement the spec's actively-processing keep-alive (drop this when the processor's
    //        process() return value has been false and inputs are silent). For now a live worklet
    //        node stays registered with the context until the context goes away.
    context->add_playing_source(node);

    // 8. Queue a control message to invoke the constructor of the corresponding AudioWorkletProcessor.
    // NOTE: The worklet shares the window agent's event loop, so the control message becomes a queued
    //       global task on the worklet global; the node constructor returns before user code runs.
    HTML::queue_global_task(HTML::Task::Source::DOMManipulation, worklet_realm.global_object(),
        GC::create_function(realm.heap(), [node, scope = GC::Ref { worklet_scope }, processor_port, options_object] {
            node->invoke_processor_constructor(scope, processor_port, options_object);
        }));

    return node;
}

AudioWorkletNode::AudioWorkletNode(GC::Ref<BaseAudioContext> context, Utf16String name, WebIDL::UnsignedLong number_of_inputs, WebIDL::UnsignedLong number_of_outputs, GC::Ref<AudioParamMap> parameter_map, GC::Ref<HTML::MessagePort> port)
    : AudioNode(context)
    , m_name(move(name))
    , m_number_of_inputs(number_of_inputs)
    , m_number_of_outputs(number_of_outputs)
    , m_parameter_map(parameter_map)
    , m_port(port)
{
}

AudioWorkletNode::~AudioWorkletNode() = default;

// https://webaudio.github.io/web-audio-api/#instantiation-of-audioworkletprocessor
void AudioWorkletNode::invoke_processor_constructor(GC::Ref<AudioWorkletGlobalScope> scope, GC::Ref<HTML::MessagePort> processor_port, GC::Ref<JS::Object> options_object)
{
    auto& worklet_realm = scope->realm();
    auto& vm = worklet_realm.vm();
    HTML::TemporaryExecutionContext execution_context { worklet_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    auto const* definition = scope->find_definition(m_name);
    VERIFY(definition);

    // 1. Set the pending processor construction data: the processor-side port.
    scope->set_pending_processor_construction_data({ processor_port });

    // 2. Construct the processor: Construct(processorCtor, « optionsObject »).
    auto processor_or_error = JS::construct(vm, *definition->constructor, options_object);

    // 3. Empty the pending processor construction data slot (a constructor that never chained to
    //    super() leaves it populated).
    (void)scope->take_pending_processor_construction_data();

    if (processor_or_error.is_error()) {
        m_processor_errored = true;
        m_pipe->set_state(Rendering::AudioWorkletPipe::State::Failed);
        fire_processor_error(processor_or_error.release_error().value());
        return;
    }

    m_processor = processor_or_error.release_value();

    Vector<Utf16String> parameter_names;
    if (auto synced = context()->audio_worklet()->find_definition(m_name); synced.has_value()) {
        for (auto const& descriptor : synced->parameter_descriptors)
            parameter_names.append(descriptor.name);
    }
    scope->add_processor_slot({
        .node_id = node_id(),
        .processor = *m_processor,
        .processor_port = processor_port,
        .node = GC::Weak<AudioWorkletNode> { *this },
        .pipe = *m_pipe,
        .parameter_names = move(parameter_names),
    });

    // 4. Enable the processor-side port's message queue: pending messages posted since node construction
    //    are now delivered, after the constructor had its chance to register listeners.
    processor_port->enable();
}

void AudioWorkletNode::finalize()
{
    // Allocation-free: the base queues RemoveNode (destroying the render node on the audio thread at
    // the next message drain), and the atomic state store tells the pump to drop the slot.
    if (m_pipe) {
        m_pipe->clear_pump_callback();
        m_pipe->request_shutdown();
    }
    Base::finalize();
}

void AudioWorkletNode::fire_processor_error(JS::Value error)
{
    // https://webaudio.github.io/web-audio-api/#dom-audioworkletnode-onprocessorerror
    // "...the processor will queue a task to fire an event named processorerror using ErrorEvent at the
    //  associated AudioWorkletNode."
    auto& node_realm = HTML::relevant_realm(context()->relevant_global_object());
    HTML::ErrorEventInit event_init;
    event_init.message = error.to_utf16_string_without_side_effects();
    HTML::queue_global_task(HTML::Task::Source::DOMManipulation, context()->relevant_global_object(),
        GC::create_function(node_realm.heap(), [node = GC::Ref { *this }, event_init = move(event_init)] {
            auto event = HTML::ErrorEvent::create(HTML::EventNames::processorerror, event_init,
                HighResolutionTime::current_high_resolution_time(node->context()->relevant_global_object()));
            node->dispatch_event(event);
        }));
}

void AudioWorkletNode::set_onprocessorerror(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::processorerror, event_handler);
}

WebIDL::CallbackType* AudioWorkletNode::onprocessorerror()
{
    return event_handler_attribute(HTML::EventNames::processorerror);
}

void AudioWorkletNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_parameter_map);
    visitor.visit(m_port);
    visitor.visit(m_processor);
}

}
