/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/RustEventTarget.h>
#include <LibWeb/WebAudio/AudioProcessingEvent.h>

namespace Web::WebAudio {

extern "C" DOM::Event* web_rust_audio_processing_event_create(JSVM*, void* event, double playback_time, void* input, void* output);

GC::Ref<DOM::Event> AudioProcessingEvent::create(Utf16FlyString const& event_name, AudioProcessingEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = Bindings::RustEvent::create(event_name, event_init, time_stamp);
    return *web_rust_audio_processing_event_create(JS::HostABI::vm_to_abi(JS::VM::the()), event.ptr(), event_init.playback_time, event_init.input_buffer.ptr(), event_init.output_buffer.ptr());
}

}
