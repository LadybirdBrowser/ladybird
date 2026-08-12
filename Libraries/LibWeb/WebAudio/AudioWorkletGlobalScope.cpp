/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/WebAudio/AudioWorklet.h>
#include <LibWeb/WebAudio/AudioWorkletGlobalScope.h>
#include <LibWeb/WebAudio/AudioWorkletNode.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioWorkletGlobalScope);

GC::Ref<AudioWorkletGlobalScope> AudioWorkletGlobalScope::create(GC::Ref<AudioWorklet> worklet, float sample_rate)
{
    return GC::Heap::the().allocate<AudioWorkletGlobalScope>(worklet, sample_rate);
}

AudioWorkletGlobalScope::AudioWorkletGlobalScope(GC::Ref<AudioWorklet> worklet, float sample_rate)
    : m_worklet(worklet)
    , m_sample_rate(sample_rate)
{
}

AudioWorkletGlobalScope::~AudioWorkletGlobalScope() = default;

// Converts one element of the `parameterDescriptors` sequence. This is the WebIDL conversion to
// AudioParamDescriptor, hand-rolled because the value is read off the constructor rather than
// arriving through a generated binding signature.
static WebIDL::ExceptionOr<AudioParamDescriptor> convert_parameter_descriptor(JS::VM& vm, JS::Value value)
{
    if (!value.is_object())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObject, value.to_utf16_string_without_side_effects());
    auto& object = value.as_object();

    AudioParamDescriptor descriptor;

    auto name_value = TRY(object.get(JS::PropertyKey { "name"_utf16_fly_string }));
    if (name_value.is_undefined())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::MissingRequiredProperty, "name"sv);
    descriptor.name = TRY(name_value.to_utf16_string(vm));

    auto read_float_member = [&](Utf16FlyString const& member_name, float fallback) -> JS::ThrowCompletionOr<float> {
        auto member_value = TRY(object.get(JS::PropertyKey { member_name }));
        if (member_value.is_undefined())
            return fallback;
        auto value = static_cast<float>(TRY(member_value.to_double(vm)));
        if (isinf(value) || isnan(value))
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::InvalidRestrictedFloatingPointParameter, member_name);
        return value;
    };
    descriptor.default_value = TRY(read_float_member("defaultValue"_utf16_fly_string, 0.0f));
    descriptor.min_value = TRY(read_float_member("minValue"_utf16_fly_string, AK::NumericLimits<float>::lowest()));
    descriptor.max_value = TRY(read_float_member("maxValue"_utf16_fly_string, AK::NumericLimits<float>::max()));

    auto rate_value = TRY(object.get(JS::PropertyKey { "automationRate"_utf16_fly_string }));
    if (!rate_value.is_undefined()) {
        auto rate_string = TRY(rate_value.to_utf16_string(vm));
        if (rate_string == "a-rate"sv)
            descriptor.automation_rate = Bindings::AutomationRate::ARate;
        else if (rate_string == "k-rate"sv)
            descriptor.automation_rate = Bindings::AutomationRate::KRate;
        else
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::InvalidEnumerationValue, rate_string, "AutomationRate"sv);
    }

    return descriptor;
}

// https://webaudio.github.io/web-audio-api/#dom-audioworkletglobalscope-registerprocessor
WebIDL::ExceptionOr<void> AudioWorkletGlobalScope::register_processor(Utf16String name, GC::Ref<WebIDL::CallbackType> processor_ctor)
{
    auto& vm = realm().vm();

    // 1. If name is an empty string, throw a NotSupportedError.
    if (name.is_empty())
        return WebIDL::NotSupportedError::create("registerProcessor name must not be empty"_utf16);

    // 2. If name already exists as a key in the node name to processor constructor map, throw a NotSupportedError.
    if (m_processor_definitions.contains(name))
        return WebIDL::NotSupportedError::create(Utf16String::formatted("A processor named '{}' is already registered", name));

    // 3. If the result of IsConstructor(argument=processorCtor) is false, throw a TypeError.
    auto& constructor_object = *processor_ctor->callback;
    if (!JS::Value(&constructor_object).is_constructor())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAConstructor, "processorCtor"sv);
    auto& constructor = static_cast<JS::FunctionObject&>(constructor_object);

    // 4. Let prototype be the result of Get(O=processorCtor, P="prototype"). If prototype is not an Object, throw a TypeError.
    auto prototype = TRY(constructor.get(vm.names.prototype));
    if (!prototype.is_object())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObject, "processorCtor.prototype"sv);

    // 5. Let parameterDescriptorsValue be the result of Get(O=processorCtor, P="parameterDescriptors").
    auto parameter_descriptors_value = TRY(constructor.get(JS::PropertyKey { "parameterDescriptors"_utf16_fly_string }));

    Vector<AudioParamDescriptor> descriptors;
    // 6. If parameterDescriptorsValue is not undefined:
    if (!parameter_descriptors_value.is_undefined()) {
        // 6.1. Let parameterDescriptorSequence be the result of the conversion from parameterDescriptorsValue
        //      to an IDL value of type sequence<AudioParamDescriptor>.
        auto iterator = TRY(JS::get_iterator(vm, parameter_descriptors_value, JS::IteratorHint::Sync));
        auto values = TRY(JS::iterator_to_list(vm, iterator));
        descriptors.ensure_capacity(values.size());
        for (auto value : values)
            descriptors.unchecked_append(TRY(convert_parameter_descriptor(vm, value)));

        // 6.2/6.3. Let paramNames be an empty Vector; for each descriptor: if paramNames contains its name,
        //          throw a NotSupportedError; validate that defaultValue is within [minValue, maxValue],
        //          else throw an InvalidStateError.
        HashTable<Utf16String> parameter_names;
        for (auto const& descriptor : descriptors) {
            if (parameter_names.contains(descriptor.name))
                return WebIDL::NotSupportedError::create(Utf16String::formatted("Duplicate parameter descriptor name '{}'", descriptor.name));
            parameter_names.set(descriptor.name);

            if (!(descriptor.min_value <= descriptor.default_value && descriptor.default_value <= descriptor.max_value))
                return WebIDL::InvalidStateError::create(Utf16String::formatted("Parameter '{}' defaultValue is outside [minValue, maxValue]", descriptor.name));
        }
    }

    // 7. Append the key-value pair name → processorCtor to the node name to processor constructor map of
    //    the associated AudioWorkletGlobalScope.
    m_processor_definitions.set(name, ProcessorDefinition { constructor, descriptors });

    // 8. Queue a media element task to append the key-value pair name → parameterDescriptorSequence to the
    //    node name to parameter descriptor map of the associated BaseAudioContext.
    // NOTE: The spec ships the descriptors to the control thread via a control message. Our worklet global
    //       scope runs on the control thread, so this collapses to a direct call.
    m_worklet->add_synced_definition({ name, move(descriptors) });

    return {};
}

AudioWorkletGlobalScope::ProcessorDefinition const* AudioWorkletGlobalScope::find_definition(Utf16String const& name) const
{
    auto it = m_processor_definitions.find(name);
    if (it == m_processor_definitions.end())
        return nullptr;
    return &it->value;
}

Optional<AudioWorkletGlobalScope::PendingProcessorConstructionData> AudioWorkletGlobalScope::take_pending_processor_construction_data()
{
    if (!m_pending_processor_construction_data.has_value())
        return {};
    return m_pending_processor_construction_data.release_value();
}

void AudioWorkletGlobalScope::set_pending_processor_construction_data(PendingProcessorConstructionData data)
{
    m_pending_processor_construction_data = move(data);
}

void AudioWorkletGlobalScope::add_processor_slot(ProcessorSlot slot)
{
    m_processor_slots.set(slot.node_id, move(slot));
}

void AudioWorkletGlobalScope::pump(NodeID node_id, Rendering::AudioWorkletPipe& pipe)
{
    // Every invocation consumes its scheduled wakeup, including stale callbacks for removed slots.
    pipe.clear_wakeup_flag();
    auto it = m_processor_slots.find(node_id);
    if (it == m_processor_slots.end())
        return;

    if (it->value.pipe->state() == Rendering::AudioWorkletPipe::State::ShutDown) {
        m_processor_slots.remove(node_id);
        return;
    }
    if (it->value.failed)
        return;

    auto worklet_pipe = it->value.pipe;
    auto processor = it->value.processor;
    auto node = it->value.node;
    auto parameter_names = it->value.parameter_names;

    auto& realm = this->realm();
    auto& vm = realm.vm();
    // Keep one execution context for the batch so its microtask checkpoint runs after all available quanta.
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    auto const& config = worklet_pipe->config();

    while (true) {
        if (worklet_pipe->output_occupancy() == config.ring_capacity)
            break;
        bool processed_one = false;
        bool pump_failed = false;
        bool output_full = false;

        (void)worklet_pipe->try_pop_input([&](Rendering::AudioWorkletPipe::InputSlotReader const& input_slot) {
            processed_one = true;

            // https://webaudio.github.io/web-audio-api/#rendering-loop — currentFrame/currentTime
            // reflect the block being processed.
            m_current_frame = input_slot.start_frame();
            m_current_time = static_cast<double>(input_slot.start_frame()) / m_sample_rate;

            // The processor may detach channel arrays, so they cannot be reused without checking their buffers.
            // FIXME: Reuse arrays that remain attached.
            auto build_channel_array = [&](size_t channel_count, auto&& fill_channel) -> GC::Ref<JS::Array> {
                auto channels = MUST(JS::Array::create(realm, channel_count));
                for (size_t channel = 0; channel < channel_count; ++channel) {
                    auto float32_array = MUST(JS::Float32Array::create(realm, config.quantum_size));
                    fill_channel(channel, float32_array);
                    MUST(channels->create_data_property(JS::PropertyKey { channel }, float32_array));
                }
                return channels;
            };

            auto inputs = MUST(JS::Array::create(realm, config.input_count));
            for (size_t input = 0; input < config.input_count; ++input) {
                auto channels = build_channel_array(input_slot.actual_channel_count(input), [&](size_t channel, auto& float32_array) {
                    auto source = input_slot.input_channel(input, channel);
                    float32_array->viewed_array_buffer()->overwrite(0, source.data(), source.size() * sizeof(float));
                });
                MUST(inputs->create_data_property(JS::PropertyKey { input }, channels));
            }

            auto output_channel_count = [&](size_t output) -> size_t {
                if (config.output_channel_count_matches_input)
                    return max(input_slot.actual_channel_count(0), 1u);
                return config.output_channel_capacity[output];
            };
            auto outputs = MUST(JS::Array::create(realm, config.output_count));
            Vector<GC::Ref<JS::Array>> output_channel_arrays;
            output_channel_arrays.ensure_capacity(config.output_count);
            for (size_t output = 0; output < config.output_count; ++output) {
                auto channels = build_channel_array(output_channel_count(output), [&](size_t, auto&) { });
                output_channel_arrays.unchecked_append(channels);
                MUST(outputs->create_data_property(JS::PropertyKey { output }, channels));
            }

            auto parameters = JS::Object::create(realm, realm.intrinsics().object_prototype());
            for (size_t param_index = 0; param_index < config.param_count; ++param_index) {
                auto block = input_slot.param_block(param_index);
                auto float32_array = MUST(JS::Float32Array::create(realm, block.size()));
                float32_array->viewed_array_buffer()->overwrite(0, block.data(), block.size() * sizeof(float));
                MUST(parameters->create_data_property(JS::PropertyKey { Utf16FlyString(parameter_names[param_index]) }, float32_array));
            }

            // Look up process on the instance each call (spec); a non-callable value fails the node.
            auto process_value_or_error = processor->get("process"_utf16_fly_string);
            if (process_value_or_error.is_error() || !process_value_or_error.value().is_function()) {
                pump_failed = true;
                if (auto node_ptr = node.ptr()) {
                    auto error = process_value_or_error.is_error()
                        ? process_value_or_error.release_error().value()
                        : JS::Value { JS::TypeError::create(realm, "AudioWorkletProcessor.process must be a function"_utf16).ptr() };
                    node_ptr->fire_processor_error(error);
                }
                return;
            }

            auto process_value = process_value_or_error.release_value();
            AK::Array<JS::Value, 3> process_arguments { JS::Value { inputs.ptr() }, JS::Value { outputs.ptr() }, JS::Value { parameters.ptr() } };
            auto result = JS::call(vm, process_value.as_function(), JS::Value { processor.ptr() },
                ReadonlySpan<JS::Value> { process_arguments });
            if (result.is_error()) {
                pump_failed = true;
                if (auto node_ptr = node.ptr())
                    node_ptr->fire_processor_error(result.release_error().value());
                return;
            }

            if (worklet_pipe->state() == Rendering::AudioWorkletPipe::State::ShutDown)
                return;

            bool processor_active = result.value().to_boolean();

            // Push the processed quantum back; read outputs defensively (detached → silence).
            if (!worklet_pipe->try_push_output([&](Rendering::AudioWorkletPipe::OutputSlotWriter& output_slot) {
                    output_slot.start_frame() = input_slot.start_frame();
                    output_slot.set_processor_active(processor_active);
                    for (size_t output = 0; output < config.output_count; ++output) {
                        auto channel_count = output_channel_count(output);
                        output_slot.actual_channel_count(output) = channel_count;
                        for (size_t channel = 0; channel < channel_count; ++channel) {
                            auto destination = output_slot.output_channel(output, channel);
                            auto channel_value = MUST(output_channel_arrays[output]->get(JS::PropertyKey { channel }));
                            auto* float32_array = channel_value.is_object() ? as_if<JS::Float32Array>(channel_value.as_object()) : nullptr;
                            if (!float32_array || float32_array->viewed_array_buffer()->is_detached()
                                || float32_array->array_length().length() < config.quantum_size) {
                                __builtin_memset(destination.data(), 0, destination.size() * sizeof(float));
                                continue;
                            }
                            float32_array->viewed_array_buffer()->copy_to(float32_array->byte_offset(),
                                Bytes { reinterpret_cast<u8*>(destination.data()), destination.size() * sizeof(float) });
                        }
                    }
                }))
                output_full = true;
        });

        if (pump_failed) {
            if (auto slot_after_process = m_processor_slots.find(node_id); slot_after_process != m_processor_slots.end())
                slot_after_process->value.failed = true;
            worklet_pipe->set_state(Rendering::AudioWorkletPipe::State::Failed);
            return;
        }
        if (worklet_pipe->state() == Rendering::AudioWorkletPipe::State::ShutDown)
            return;
        if (output_full)
            break;
        if (!processed_one)
            break;
    }
}

void AudioWorkletGlobalScope::shutdown_all_slots()
{
    for (auto& slot : m_processor_slots) {
        slot.value.pipe->clear_pump_callback();
        slot.value.pipe->request_shutdown();
    }
    m_processor_slots.clear();
}

void AudioWorkletGlobalScope::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_worklet);
    visitor.visit(m_port);
    for (auto& definition : m_processor_definitions)
        visitor.visit(definition.value.constructor);
    if (m_pending_processor_construction_data.has_value())
        visitor.visit(m_pending_processor_construction_data->port);
    for (auto& slot : m_processor_slots) {
        visitor.visit(slot.value.processor);
        visitor.visit(slot.value.processor_port);
    }
}

}
