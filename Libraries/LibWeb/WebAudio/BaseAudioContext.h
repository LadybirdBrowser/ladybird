/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/BaseAudioContext.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Forward.h>
namespace Web::WebAudio {

class BaseAudioContext : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(BaseAudioContext, base_audio_context);

public:
    float sample_rate() const;
    GC::Ref<DOM::EventTarget> relevant_global_event_target() const;
    JS::Object& relevant_global_object() const;
};

}
