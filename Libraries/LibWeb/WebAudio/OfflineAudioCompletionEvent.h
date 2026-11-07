/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/OfflineAudioCompletionEvent.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/WebAudio/AudioBuffer.h>

namespace Web::WebAudio {

struct OfflineAudioCompletionEventInit : DOM::EventInit {
    GC::Ref<AudioBuffer> rendered_buffer;
};

class OfflineAudioCompletionEvent final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(OfflineAudioCompletionEvent, offline_audio_completion_event)

public:
    static GC::Ref<DOM::Event> create(Utf16FlyString const& event_name, OfflineAudioCompletionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);
};

}
