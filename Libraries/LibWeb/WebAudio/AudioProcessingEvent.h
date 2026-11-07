/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/AudioProcessingEvent.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/WebAudio/AudioBuffer.h>

namespace Web::WebAudio {

struct AudioProcessingEventInit : DOM::EventInit {
    GC::Ref<AudioBuffer> input_buffer;
    GC::Ref<AudioBuffer> output_buffer;
    double playback_time;
};

class AudioProcessingEvent final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(AudioProcessingEvent, audio_processing_event)

public:
    static GC::Ref<DOM::Event> create(Utf16FlyString const& event_name, AudioProcessingEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
};

}
