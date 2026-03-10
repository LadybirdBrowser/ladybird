/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16String.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/WebAudio/Debug.h>
#include <LibWeb/WebAudio/Engine/Mixing.h>
#include <LibWeb/WebAudio/Types.h>
#include <LibWeb/WebAudio/Worklet/AudioWorkletProcessorInvoker.h>

namespace Web::WebAudio::Render {

JS::ThrowCompletionOr<bool> invoke_audio_worklet_processor_process(
    JS::Realm& worklet_realm,
    JS::Object& processor_instance,
    Vector<Vector<AudioBus const*>> const& inputs,
    Span<AudioBus*> outputs,
    Span<AudioWorkletProcessorHost::ParameterSpan const> parameters,
    size_t quantum_size)
{
    ASSERT_RENDER_THREAD();
    auto& vm = worklet_realm.vm();

    auto process_key = JS::PropertyKey { "process"_utf16_fly_string };
    auto process_value = TRY(processor_instance.get(process_key));

    bool has_own_process = TRY(processor_instance.has_own_property(process_key));
    if (!has_own_process) {
        auto* prototype = TRY(processor_instance.internal_get_prototype_of());
        if (!prototype || !TRY(prototype->has_own_property(process_key)))
            return vm.throw_completion<JS::TypeError>("AudioWorkletProcessor.process is not callable"_string);
    }

    if (!process_value.is_function())
        return vm.throw_completion<JS::TypeError>("AudioWorkletProcessor.process is not callable"_string);

    auto& process_function = process_value.as_function();

    auto inputs_array = TRY(JS::Array::create(worklet_realm, inputs.size()));
    for (size_t input_index = 0; input_index < inputs.size(); ++input_index) {
        AudioBus const* mixed_input = nullptr;
        if (!inputs[input_index].is_empty())
            mixed_input = inputs[input_index][0];

        size_t const channel_count = mixed_input ? mixed_input->channel_count() : 0;
        auto channels_array = TRY(JS::Array::create(worklet_realm, channel_count));

        for (size_t ch = 0; ch < channel_count; ++ch) {
            auto channel = TRY(JS::Float32Array::create(worklet_realm, quantum_size));
            auto span = channel->data();
            auto in = mixed_input->channel(ch);
            for (size_t i = 0; i < quantum_size; ++i)
                span[i] = i < in.size() ? in[i] : 0.0f;
            TRY(channels_array->create_data_property_or_throw(JS::PropertyKey { static_cast<u32>(ch) }, channel));
        }

        TRY(channels_array->set_integrity_level(JS::Object::IntegrityLevel::Frozen));

        TRY(inputs_array->create_data_property_or_throw(JS::PropertyKey { static_cast<u32>(input_index) }, channels_array));
    }

    auto outputs_array = TRY(JS::Array::create(worklet_realm, outputs.size()));
    Vector<Vector<GC::Ref<JS::Float32Array>>> output_typed_arrays;
    output_typed_arrays.ensure_capacity(outputs.size());

    for (size_t output_index = 0; output_index < outputs.size(); ++output_index) {
        AudioBus* out_bus = outputs[output_index];
        size_t const channel_count = out_bus ? out_bus->channel_count() : 0;
        auto channels_array = TRY(JS::Array::create(worklet_realm, channel_count));

        Vector<GC::Ref<JS::Float32Array>> per_output;
        per_output.ensure_capacity(channel_count);

        for (size_t ch = 0; ch < channel_count; ++ch) {
            auto channel = TRY(JS::Float32Array::create(worklet_realm, quantum_size));
            per_output.unchecked_append(channel);
            TRY(channels_array->create_data_property_or_throw(JS::PropertyKey { static_cast<u32>(ch) }, channel));
        }

        output_typed_arrays.unchecked_append(move(per_output));
        TRY(outputs_array->create_data_property_or_throw(JS::PropertyKey { static_cast<u32>(output_index) }, channels_array));

        TRY(channels_array->set_integrity_level(JS::Object::IntegrityLevel::Frozen));
    }

    TRY(inputs_array->set_integrity_level(JS::Object::IntegrityLevel::Frozen));
    TRY(outputs_array->set_integrity_level(JS::Object::IntegrityLevel::Frozen));

    auto parameters_object = JS::Object::create(worklet_realm, worklet_realm.intrinsics().object_prototype());
    for (auto const& param : parameters) {
        size_t const length = param.values.size();
        auto values = TRY(JS::Float32Array::create(worklet_realm, length));
        auto out = values->data();
        for (size_t i = 0; i < length; ++i)
            out[i] = param.values[i];
        TRY(parameters_object->set(JS::PropertyKey { Utf16String::from_utf8(param.name) }, values, JS::Object::ShouldThrowExceptions::Yes));
    }
    TRY(parameters_object->set_integrity_level(JS::Object::IntegrityLevel::Frozen));

    auto result_value = TRY(JS::call(vm, process_function, &processor_instance, inputs_array, outputs_array, parameters_object));
    bool keep_alive = true;
    if (!result_value.is_undefined())
        keep_alive = result_value.to_boolean();

    // Copy JS output buffers back into the render outputs.
    for (size_t output_index = 0; output_index < outputs.size(); ++output_index) {
        AudioBus* out_bus = outputs[output_index];
        if (!out_bus)
            continue;

        size_t const channel_count = out_bus->channel_count();
        for (size_t ch = 0; ch < channel_count; ++ch) {
            auto span = output_typed_arrays[output_index][ch]->data();
            auto out = out_bus->channel(ch);
            for (size_t i = 0; i < quantum_size; ++i)
                out[i] = i < span.size() ? span[i] : 0.0f;
        }
    }

    return keep_alive;
}

}
