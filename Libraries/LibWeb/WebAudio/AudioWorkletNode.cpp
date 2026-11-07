/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/ErrorEvent.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/WebAudio/AudioWorklet.h>
#include <LibWeb/WebAudio/AudioWorkletGlobalScope.h>
#include <LibWeb/WebAudio/AudioWorkletNode.h>
#include <LibWeb/WebAudio/RustFFI.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>

namespace Web::WebAudio {

using DescriptorCallback = void (*)(void*, FlatPtr, float, float, float, RustFFI::AutomationRate);
extern "C" bool web_audio_worklet_find_definition(AudioWorklet*, FlatPtr, void*, DescriptorCallback);
extern "C" AudioWorkletGlobalScope* web_audio_worklet_global_scope(AudioWorklet*);
extern "C" JS::Realm* web_audio_worklet_scope_realm(AudioWorkletGlobalScope*);
extern "C" JSCompletion web_audio_worklet_clone_options(JSVM*, JS::Realm*, JS::Realm*, JS::Object*);
extern "C" JS::FunctionObject* web_audio_worklet_processor_constructor(AudioWorkletGlobalScope*, FlatPtr);
extern "C" void web_audio_worklet_set_pending_data(AudioWorkletGlobalScope*, AudioWorkletNode*, HTML::MessagePort*);
extern "C" void web_audio_worklet_clear_pending_data(AudioWorkletGlobalScope*);
extern "C" void web_audio_worklet_set_current_frame(AudioWorkletGlobalScope*, u64);
extern "C" void web_audio_worklet_queue_processor_error(AudioWorkletNode*, DOM::EventTarget*, DOM::EventTarget*, JSValue);

bool web_audio_worklet_find_definition(AudioWorklet* worklet, FlatPtr name, void* context, DescriptorCallback descriptor_callback)
{
    auto definition = worklet->find_definition(Utf16String::from_raw(name));
    if (!definition.has_value())
        return false;
    for (auto const& descriptor : definition->parameter_descriptors) {
        Utf16FlyString key(descriptor.name);
        descriptor_callback(context, key.raw_identity(), descriptor.default_value, descriptor.min_value, descriptor.max_value, static_cast<RustFFI::AutomationRate>(descriptor.automation_rate));
    }
    return true;
}
AudioWorkletGlobalScope* web_audio_worklet_global_scope(AudioWorklet* worklet) { return worklet->audio_worklet_global_scope().ptr(); }
JS::Realm* web_audio_worklet_scope_realm(AudioWorkletGlobalScope* scope) { return &scope->realm(); }
JSCompletion web_audio_worklet_clone_options(JSVM* vm, JS::Realm* realm, JS::Realm* error_realm, JS::Object* options)
{
    auto record = HTML::structured_serialize(JS::HostABI::vm_from_abi(vm), options, HTML::AllowSharedArrayBuffers::CrossOriginIsolatedOnly);
    if (record.is_error())
        return JS::HostABI::throw_completion_to_abi(WebIDL::exception_to_throw_completion(JS::HostABI::vm_from_abi(vm), *error_realm, record.release_error()).value());
    auto cloned = HTML::structured_deserialize(JS::HostABI::vm_from_abi(vm), record.value(), *realm);
    if (cloned.is_error())
        return JS::HostABI::throw_completion_to_abi(WebIDL::exception_to_throw_completion(JS::HostABI::vm_from_abi(vm), *error_realm, cloned.release_error()).value());
    return JS::HostABI::normal_completion_to_abi(JS::HostABI::value_to_abi(cloned.release_value()));
}
JS::FunctionObject* web_audio_worklet_processor_constructor(AudioWorkletGlobalScope* scope, FlatPtr name)
{
    auto const* definition = scope->find_definition(Utf16String::from_raw(name));
    VERIFY(definition);
    return definition->constructor.ptr();
}
void web_audio_worklet_set_pending_data(AudioWorkletGlobalScope* scope, AudioWorkletNode* node_reference, HTML::MessagePort* transferred_port)
{
    scope->set_pending_processor_construction_data({ *node_reference, *transferred_port });
}
void web_audio_worklet_clear_pending_data(AudioWorkletGlobalScope* scope) { scope->clear_pending_processor_construction_data(); }
void web_audio_worklet_set_current_frame(AudioWorkletGlobalScope* scope, u64 current_frame) { scope->set_current_frame_and_time(current_frame, static_cast<double>(current_frame) / scope->sample_rate()); }
void web_audio_worklet_queue_processor_error(AudioWorkletNode* node, DOM::EventTarget* target, DOM::EventTarget* global, JSValue error)
{
    auto& global_object = HTML::relevant_global_object(HTML::relevant_window_or_worker_global_scope(*global));
    HTML::ErrorEventInit event_init;
    event_init.message = JS::HostABI::value_from_abi(error).to_utf16_string_without_side_effects();
    HTML::queue_global_task(HTML::Task::Source::DOMManipulation, global_object,
        GC::create_function(GC::Heap::the(), GC::captures(*node, *target, *global, move(event_init)), [](AudioWorkletNode&, DOM::EventTarget& target, DOM::EventTarget& global, HTML::ErrorEventInit const& event_init) {
            auto& global_object = HTML::relevant_global_object(HTML::relevant_window_or_worker_global_scope(global));
            auto event = HTML::ErrorEvent::create(HTML::EventNames::processorerror, event_init, HighResolutionTime::current_high_resolution_time(global_object));
            target.dispatch_event(event);
        }));
}

}
