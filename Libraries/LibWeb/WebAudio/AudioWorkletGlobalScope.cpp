/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/WebAudio/AudioWorklet.h>
#include <LibWeb/WebAudio/AudioWorkletGlobalScope.h>
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

void AudioWorkletGlobalScope::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_worklet);
    visitor.visit(m_port);
    for (auto& definition : m_processor_definitions)
        visitor.visit(definition.value.constructor);
    if (m_pending_processor_construction_data.has_value())
        visitor.visit(m_pending_processor_construction_data->port);
}

}
