/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/StringView.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/AudioWorkletProcessorPrototype.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioWorkletGlobalScope.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

static JS::ThrowCompletionOr<Vector<AudioParamDescriptor>> parse_parameter_descriptors(JS::VM& vm, JS::Value processor_constructor)
{
    Vector<AudioParamDescriptor> descriptors;
    HashTable<FlyString> seen_names;

    auto& realm = *vm.current_realm();

    if (!processor_constructor.is_object())
        return vm.throw_completion<JS::TypeError>("Processor constructor must be an object"sv);

    auto& ctor_object = processor_constructor.as_object();
    auto descriptors_value = TRY(ctor_object.get("parameterDescriptors"_utf16_fly_string));
    if (descriptors_value.is_undefined()) {
        return descriptors;
    }

    auto iterator_record = TRY(JS::get_iterator(vm, descriptors_value, JS::IteratorHint::Sync));
    while (true) {
        auto next_value = TRY(JS::iterator_step_value(vm, *iterator_record));
        if (!next_value.has_value())
            break;

        auto descriptor_value = next_value.value();
        if (!descriptor_value.is_object())
            return vm.throw_completion<JS::TypeError>("parameterDescriptors items must be objects"sv);

        auto& descriptor_object = descriptor_value.as_object();

        auto name_value = TRY(descriptor_object.get("name"_utf16_fly_string));
        auto name = TRY(name_value.to_string(vm));

        AudioParamDescriptor descriptor;
        descriptor.name = name;

        if (seen_names.contains(descriptor.name))
            return JS::throw_completion(WebIDL::NotSupportedError::create(realm, "AudioParamDescriptor.name must be unique"_utf16));
        seen_names.set(descriptor.name);

        auto default_value_value = TRY(descriptor_object.get("defaultValue"_utf16_fly_string));
        if (!default_value_value.is_undefined()) {
            auto default_value_as_double = TRY(default_value_value.to_double(vm));
            descriptor.default_value = static_cast<float>(default_value_as_double);
        }

        auto min_value_value = TRY(descriptor_object.get("minValue"_utf16_fly_string));
        if (!min_value_value.is_undefined()) {
            auto min_value_as_double = TRY(min_value_value.to_double(vm));
            descriptor.min_value = static_cast<float>(min_value_as_double);
        } else {
            descriptor.min_value = -AK::NumericLimits<float>::max();
        }

        auto max_value_value = TRY(descriptor_object.get("maxValue"_utf16_fly_string));
        if (!max_value_value.is_undefined()) {
            auto max_value_as_double = TRY(max_value_value.to_double(vm));
            descriptor.max_value = static_cast<float>(max_value_as_double);
        } else {
            descriptor.max_value = AK::NumericLimits<float>::max();
        }

        if (descriptor.default_value < descriptor.min_value || descriptor.default_value > descriptor.max_value)
            return JS::throw_completion(WebIDL::InvalidStateError::create(realm, "AudioParamDescriptor.defaultValue must be within [minValue, maxValue]"_utf16));

        auto automation_rate_value = TRY(descriptor_object.get("automationRate"_utf16_fly_string));
        if (!automation_rate_value.is_undefined()) {
            auto automation_rate = TRY(automation_rate_value.to_string(vm));
            if (automation_rate == "a-rate"sv) {
                descriptor.automation_rate = Bindings::AutomationRate::ARate;
            } else if (automation_rate == "k-rate"sv) {
                descriptor.automation_rate = Bindings::AutomationRate::KRate;
            } else {
                return vm.throw_completion<JS::TypeError>("AudioParamDescriptor.automationRate must be 'a-rate' or 'k-rate'"sv);
            }
        }

        descriptors.append(move(descriptor));
    }

    return descriptors;
}

GC_DEFINE_ALLOCATOR(AudioWorkletGlobalScope);

AudioWorkletGlobalScope::AudioWorkletGlobalScope(JS::Realm& realm)
    : HTML::WorkletGlobalScope(realm)
{
}

AudioWorkletGlobalScope::~AudioWorkletGlobalScope() = default;

GC::Ref<AudioWorkletGlobalScope> AudioWorkletGlobalScope::create(JS::Realm& realm)
{
    return realm.create<AudioWorkletGlobalScope>(realm);
}

void AudioWorkletGlobalScope::initialize(JS::Realm&)
{
}

void AudioWorkletGlobalScope::initialize_web_interfaces()
{
    auto& realm = this->realm();

    define_native_accessor(realm, "currentFrame"_utf16_fly_string, current_frame_getter, {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);
    define_native_accessor(realm, "currentTime"_utf16_fly_string, current_time_getter, {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);
    define_native_accessor(realm, "sampleRate"_utf16_fly_string, sample_rate_getter, {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);
    define_native_accessor(realm, "port"_utf16_fly_string, port_getter, {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);

    // https://webaudio.github.io/web-audio-api/#dom-audioworkletglobalscope-registerprocessor
    auto register_processor_steps = [](JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
        auto& realm = *vm.current_realm();
        auto& global_object = as<AudioWorkletGlobalScope>(realm.global_object());

        auto name = TRY(vm.argument(0).to_string(vm));
        auto processor_constructor = vm.argument(1);

        // 1. If name is an empty string, throw a NotSupportedError.
        if (name.is_empty())
            return JS::throw_completion(WebIDL::NotSupportedError::create(realm, "Processor name must not be empty"_utf16));

        // 2. If name already exists as a key in the node name to processor constructor map, throw a NotSupportedError.
        if (global_object.is_processor_registered(name))
            return JS::throw_completion(WebIDL::NotSupportedError::create(realm, "Processor name is already registered"_utf16));

        // 3. If the result of IsConstructor(argument=processorCtor) is false, throw a TypeError.
        if (!processor_constructor.is_constructor())
            return vm.throw_completion<JS::TypeError>("Processor constructor must be a constructor"sv);

        // 4. Let prototype be the result of Get(O=processorCtor, P="prototype").
        // 5. If the result of Type(argument=prototype) is not Object, throw a TypeError.
        auto prototype_value = TRY(processor_constructor.as_object().get("prototype"_utf16_fly_string));
        if (!prototype_value.is_object())
            return vm.throw_completion<JS::TypeError>("Processor constructor prototype must be an object"sv);

        // 6. Let parameterDescriptorsValue be the result of Get(O=processorCtor, P="parameterDescriptors").
        auto parameter_descriptors_or_error = parse_parameter_descriptors(vm, processor_constructor);
        if (parameter_descriptors_or_error.is_error()) {
            global_object.mark_processor_registration_failed(name);
            return parameter_descriptors_or_error.release_error();
        }

        // 7. If parameterDescriptorsValue is not undefined, execute the following steps:
        //  1. Let parameterDescriptorSequence be the result of the conversion from parameterDescriptorsValue to an IDL value of type sequence<AudioParamDescriptor>.
        //  2. Let paramNames be an empty Array.
        //  3. For each descriptor of parameterDescriptorSequence:
        //   1. Let paramName be the value of the member name in descriptor. Throw a NotSupportedError if paramNames already contains paramName value.
        //   2. Append paramName to the paramNames array.
        //   3. Let defaultValue be the value of the member defaultValue in descriptor.
        //   4. Let minValue be the value of the member minValue in descriptor.
        //   5. Let maxValue be the value of the member maxValue in descriptor.
        //   6. If the expresstion minValue <= defaultValue <= maxValue is false, throw an InvalidStateError.
        auto parameter_descriptors = parameter_descriptors_or_error.release_value();

        // 8. Append the key-value pair name -> processorCtor to node name to processor constructor map of the associated AudioWorkletGlobalScope.
        auto result = global_object.register_processor(name, processor_constructor);
        if (result.is_exception())
            return Bindings::exception_to_throw_completion(vm, result.release_error());

        // 9. queue a media element task to append the key-value pair name -> parameterDescriptorSequence to the node name to parameter descriptor map of the associated BaseAudioContext.
        auto callback_descriptors = parameter_descriptors;
        global_object.set_parameter_descriptors(name, move(parameter_descriptors));
        if (global_object.m_processor_registration_callback)
            global_object.m_processor_registration_callback(name, callback_descriptors);

        return JS::js_undefined();
    };

    auto register_processor_function = JS::NativeFunction::create(realm, move(register_processor_steps), 2, "registerProcessor"_utf16_fly_string, &realm);
    define_direct_property("registerProcessor"_utf16_fly_string, register_processor_function, JS::Attribute::Writable | JS::Attribute::Enumerable | JS::Attribute::Configurable);

    auto& audio_worklet_processor_constructor = Bindings::ensure_web_constructor<Bindings::AudioWorkletProcessorPrototype>(realm, "AudioWorkletProcessor"_fly_string);
    define_direct_property("AudioWorkletProcessor"_utf16_fly_string, &audio_worklet_processor_constructor, JS::Attribute::Writable | JS::Attribute::Enumerable | JS::Attribute::Configurable);
}

JS_DEFINE_NATIVE_FUNCTION(AudioWorkletGlobalScope::current_frame_getter)
{
    auto& realm = *vm.current_realm();
    auto& global_object = as<AudioWorkletGlobalScope>(realm.global_object());
    return JS::Value(static_cast<double>(global_object.current_frame()));
}

JS_DEFINE_NATIVE_FUNCTION(AudioWorkletGlobalScope::current_time_getter)
{
    auto& realm = *vm.current_realm();
    auto& global_object = as<AudioWorkletGlobalScope>(realm.global_object());
    return JS::Value(global_object.current_time());
}

JS_DEFINE_NATIVE_FUNCTION(AudioWorkletGlobalScope::sample_rate_getter)
{
    auto& realm = *vm.current_realm();
    auto& global_object = as<AudioWorkletGlobalScope>(realm.global_object());
    return JS::Value(global_object.sample_rate());
}

JS_DEFINE_NATIVE_FUNCTION(AudioWorkletGlobalScope::port_getter)
{
    auto& realm = *vm.current_realm();
    auto& global_object = as<AudioWorkletGlobalScope>(realm.global_object());
    if (!global_object.shared_port()) {
        auto shared_port = HTML::MessagePort::create(realm);
        global_object.set_shared_port(shared_port);
    }
    return JS::Value(global_object.shared_port());
}

WebIDL::ExceptionOr<void> AudioWorkletGlobalScope::register_processor(String const& name, JS::Value processor_constructor)
{
    if (name.is_empty())
        return WebIDL::NotSupportedError::create(realm(), "Processor name must not be empty"_utf16);

    if (m_registered_processors.contains(name))
        return WebIDL::NotSupportedError::create(realm(), "Processor name is already registered"_utf16);

    m_registered_processors.set(name, processor_constructor);
    m_parameter_descriptors.set(name, {});
    m_failed_processor_registrations.remove(name);
    return {};
}

void AudioWorkletGlobalScope::mark_processor_registration_failed(String const& name)
{
    m_failed_processor_registrations.set(name);
}

bool AudioWorkletGlobalScope::is_processor_registration_failed(String const& name) const
{
    return m_failed_processor_registrations.contains(name);
}

bool AudioWorkletGlobalScope::is_processor_registered(String const& name) const
{
    return m_registered_processors.contains(name);
}

void AudioWorkletGlobalScope::register_processor_name(String const& name)
{
    if (m_registered_processors.contains(name))
        return;
    m_registered_processors.set(name, JS::js_undefined());
    if (!m_parameter_descriptors.contains(name))
        m_parameter_descriptors.set(name, {});
    m_failed_processor_registrations.remove(name);
}

Vector<String> AudioWorkletGlobalScope::take_failed_processor_registrations()
{
    Vector<String> names;
    names.ensure_capacity(m_failed_processor_registrations.size());
    for (auto const& name : m_failed_processor_registrations)
        names.append(name.to_string());
    m_failed_processor_registrations.clear();
    return names;
}

JS::Value AudioWorkletGlobalScope::processor_constructor(String const& name) const
{
    auto it = m_registered_processors.find(name);
    if (it == m_registered_processors.end())
        return JS::js_undefined();
    return it->value;
}

void AudioWorkletGlobalScope::set_pending_processor_port(GC::Ref<HTML::MessagePort> port)
{
    m_pending_processor_port = port;
}

GC::Ptr<HTML::MessagePort> AudioWorkletGlobalScope::take_pending_processor_port()
{
    auto port = m_pending_processor_port;
    m_pending_processor_port = nullptr;
    return port;
}

Vector<AudioParamDescriptor> const* AudioWorkletGlobalScope::parameter_descriptors(String const& name) const
{
    auto it = m_parameter_descriptors.find(name);
    if (it == m_parameter_descriptors.end())
        return nullptr;
    return &it->value;
}

void AudioWorkletGlobalScope::set_parameter_descriptors(String const& name, Vector<AudioParamDescriptor>&& descriptors)
{
    m_parameter_descriptors.set(name, move(descriptors));
}

void AudioWorkletGlobalScope::unregister_processor(String const& name)
{
    m_registered_processors.remove(name);
    m_parameter_descriptors.remove(name);
    m_failed_processor_registrations.remove(name);
}

void AudioWorkletGlobalScope::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto& it : m_registered_processors)
        visitor.visit(it.value);
}

}
