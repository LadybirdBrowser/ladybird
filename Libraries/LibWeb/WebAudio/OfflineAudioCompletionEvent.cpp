/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/RustEventTarget.h>
#include <LibWeb/WebAudio/OfflineAudioCompletionEvent.h>

namespace Web::WebAudio {

extern "C" DOM::Event* web_rust_offline_audio_completion_event_create(JSVM*, void* event, void* buffer);

GC::Ref<DOM::Event> OfflineAudioCompletionEvent::create(Utf16FlyString const& event_name, OfflineAudioCompletionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = Bindings::RustEvent::create(event_name, event_init, time_stamp);
    return *web_rust_offline_audio_completion_event_create(JS::HostABI::vm_to_abi(JS::VM::the()), event.ptr(), event_init.rendered_buffer.ptr());
}

}
