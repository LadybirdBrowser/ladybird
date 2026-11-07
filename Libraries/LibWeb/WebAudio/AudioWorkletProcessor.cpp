/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/WebAudio/AudioWorkletGlobalScope.h>

namespace Web::WebAudio {

extern "C" {
AudioWorkletGlobalScope* web_audio_processor_global_scope(JS::Realm*);
bool web_audio_processor_take_pending_data(AudioWorkletGlobalScope*, HTML::MessagePort**);
JS::Realm* web_audio_processor_scope_realm(AudioWorkletGlobalScope*);
}

AudioWorkletGlobalScope* web_audio_processor_global_scope(JS::Realm* realm)
{
    return Bindings::impl_from<AudioWorkletGlobalScope>(&realm->global_object());
}

bool web_audio_processor_take_pending_data(AudioWorkletGlobalScope* scope, HTML::MessagePort** port)
{
    auto pending_data = scope->take_pending_processor_construction_data();
    if (!pending_data.has_value())
        return false;
    *port = pending_data->port.ptr();
    return true;
}

JS::Realm* web_audio_processor_scope_realm(AudioWorkletGlobalScope* scope)
{
    return &scope->realm();
}

}
