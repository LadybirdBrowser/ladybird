/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibMediaClient/Client.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/WebAudio/AudioWorklet.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio {

extern "C" float web_rust_audio_context_sample_rate(void const*);
extern "C" DOM::EventTarget* web_rust_audio_context_global(void const*);
extern "C" JS::Object* web_rust_audio_context_global_wrapper(void const*);
extern "C" void web_rust_audio_context_document_inactive(void*);
extern "C" void web_rust_audio_context_decoded(JSVM*, void*, MediaClient::Client::DecodedAudioData*);
float BaseAudioContext::sample_rate() const { return web_rust_audio_context_sample_rate(this); }
GC::Ref<DOM::EventTarget> BaseAudioContext::relevant_global_event_target() const { return *web_rust_audio_context_global(this); }
JS::Object& BaseAudioContext::relevant_global_object() const { return *web_rust_audio_context_global_wrapper(this); }
extern "C" HTML::UniqueTaskSource* web_audio_context_create_task_source();
extern "C" void web_audio_context_destroy_task_source(HTML::UniqueTaskSource*);
extern "C" void web_audio_context_queue_task(HTML::UniqueTaskSource*, DOM::EventTarget*, GC::Cell*, void (*)(void*, void*));
extern "C" DOM::DocumentObserver* web_audio_context_observe_document(DOM::EventTarget*, Bindings::RustInterfaceCell*);
extern "C" JS::Realm* web_audio_context_global_realm(DOM::EventTarget*);
extern "C" JS::Object* web_audio_context_global_wrapper(DOM::EventTarget*);
extern "C" bool web_audio_context_document_is_fully_active(DOM::EventTarget*);
extern "C" bool web_audio_context_responsible_document_is_fully_active(DOM::EventTarget*);
extern "C" bool web_audio_context_navigable_is_destroyed(DOM::EventTarget*);
extern "C" AudioWorklet* web_audio_context_get_worklet(BaseAudioContext*);
extern "C" JS::Object* web_audio_context_wrap_worklet(JS::Realm*, AudioWorklet*);
extern "C" void web_audio_context_decode(ReadonlyBytes, u32, GC::Cell*);
extern "C" void web_audio_context_decoded_destroy(MediaClient::Client::DecodedAudioData*);
extern "C" void web_audio_context_decoded_metadata(MediaClient::Client::DecodedAudioData*, u32*, u64*, u32*);
extern "C" void web_audio_context_decoded_channel(MediaClient::Client::DecodedAudioData*, u32, ReadonlySpan<float>*);
extern "C" void web_audio_context_report_exception(JS::Realm*, JSValue);
void* RustFFI::web_audio_current_event_loop() { return &Core::EventLoop::current(); }
void RustFFI::web_audio_deferred_invoke(void* event_loop, void* task, void (*invoke)(void*))
{
    static_cast<Core::EventLoop*>(event_loop)->deferred_invoke([task, invoke] { invoke(task); });
}
HTML::UniqueTaskSource* web_audio_context_create_task_source() { return new HTML::UniqueTaskSource; }
void web_audio_context_destroy_task_source(HTML::UniqueTaskSource* source) { delete source; }
void web_audio_context_queue_task(HTML::UniqueTaskSource* source, DOM::EventTarget* global, GC::Cell* cell, void (*invoke)(void*, void*))
{
    auto& settings = HTML::relevant_settings_object(HTML::relevant_window_or_worker_global_scope(*global));
    auto task = HTML::Task::create(source->source, settings.responsible_document(), GC::create_function(GC::Heap::the(), GC::captures(*cell, invoke), [](GC::Cell& cell, void (*invoke)(void*, void*)) {
        invoke(JS::HostABI::vm_to_abi(JS::VM::the()), &cell);
    }));
    (void)HTML::main_thread_event_loop().task_queue().add(task);
}
DOM::DocumentObserver* web_audio_context_observe_document(DOM::EventTarget* global, Bindings::RustInterfaceCell* context)
{
    auto* window = as_if<HTML::Window>(*global);
    if (!window)
        return nullptr;
    // FIXME: Also handle the document becoming active again, e.g. when restored from the back/forward cache.
    auto observer = DOM::DocumentObserver::create(window->associated_document());
    observer->set_document_became_inactive(GC::create_function(GC::Heap::the(), GC::captures(*context), [](Bindings::RustInterfaceCell& context) { web_rust_audio_context_document_inactive(&context); }));
    return observer.ptr();
}
JS::Realm* web_audio_context_global_realm(DOM::EventTarget* global) { return &HTML::relevant_settings_object(HTML::relevant_window_or_worker_global_scope(*global)).realm(); }
JS::Object* web_audio_context_global_wrapper(DOM::EventTarget* global) { return &HTML::relevant_global_object(HTML::relevant_window_or_worker_global_scope(*global)); }
bool web_audio_context_document_is_fully_active(DOM::EventTarget* global) { return as<HTML::Window>(*global).associated_document().is_fully_active(); }
bool web_audio_context_responsible_document_is_fully_active(DOM::EventTarget* global)
{
    auto& settings = HTML::relevant_settings_object(HTML::relevant_window_or_worker_global_scope(*global));
    return settings.responsible_document() && settings.responsible_document()->is_fully_active();
}
bool web_audio_context_navigable_is_destroyed(DOM::EventTarget* global)
{
    auto navigable = as<HTML::Window>(*global).navigable();
    return navigable && navigable->has_been_destroyed();
}
AudioWorklet* web_audio_context_get_worklet(BaseAudioContext* context) { return AudioWorklet::create(*context).ptr(); }
JS::Object* web_audio_context_wrap_worklet(JS::Realm* realm, AudioWorklet* worklet) { return Bindings::wrap(Bindings::host_defined_wrapper_world(*realm), *realm, GC::Ref { *worklet }).ptr(); }
void web_audio_context_decode(ReadonlyBytes data, u32 sample_rate, GC::Cell* operation)
{
    auto media_client = MediaClient::Client::acquire();
    if (media_client.is_error()) {
        web_rust_audio_context_decoded(JS::HostABI::vm_to_abi(JS::VM::the()), operation, nullptr);
        return;
    }
    media_client.value()->decode_audio_data(data, sample_rate, [operation = GC::make_root(*operation)](Media::DecoderErrorOr<MediaClient::Client::DecodedAudioData> result) {
        auto* decoded = result.is_error() ? nullptr : new MediaClient::Client::DecodedAudioData(result.release_value());
        web_rust_audio_context_decoded(JS::HostABI::vm_to_abi(JS::VM::the()), operation.ptr(), decoded);
    });
}
void web_audio_context_decoded_destroy(MediaClient::Client::DecodedAudioData* decoded) { delete decoded; }
void web_audio_context_decoded_metadata(MediaClient::Client::DecodedAudioData* decoded, u32* channels, u64* frames, u32* sample_rate)
{
    *channels = decoded->channel_count;
    *frames = decoded->frame_count;
    *sample_rate = decoded->sample_rate;
}
void web_audio_context_decoded_channel(MediaClient::Client::DecodedAudioData* decoded, u32 channel, ReadonlySpan<float>* samples)
{
    *samples = decoded->channel(channel);
}

void web_audio_context_report_exception(JS::Realm* realm, JSValue error)
{
    HTML::report_exception(JS::throw_completion(JS::HostABI::value_from_abi(error)), *realm);
}

}
