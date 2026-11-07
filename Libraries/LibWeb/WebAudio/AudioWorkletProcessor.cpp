/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/WebAudio/AudioWorkletGlobalScope.h>
#include <LibWeb/WebAudio/AudioWorkletNode.h>

namespace Web::WebAudio {

extern "C" {
AudioWorkletGlobalScope* web_audio_processor_global_scope(JS::Realm*);
AudioWorkletNode* web_audio_processor_pending_node_reference(AudioWorkletGlobalScope*);
HTML::MessagePort* web_audio_processor_pending_transferred_port(AudioWorkletGlobalScope*);
JS::Realm* web_audio_processor_scope_realm(AudioWorkletGlobalScope*);
}

AudioWorkletGlobalScope* web_audio_processor_global_scope(JS::Realm* realm)
{
    return Bindings::impl_from<AudioWorkletGlobalScope>(&realm->global_object());
}

AudioWorkletNode* web_audio_processor_pending_node_reference(AudioWorkletGlobalScope* scope)
{
    auto const& pending_data = scope->pending_processor_construction_data();
    return pending_data.has_value() ? pending_data->node_reference.ptr() : nullptr;
}

HTML::MessagePort* web_audio_processor_pending_transferred_port(AudioWorkletGlobalScope* scope)
{
    return scope->pending_processor_construction_data()->transferred_port.ptr();
}

JS::Realm* web_audio_processor_scope_realm(AudioWorkletGlobalScope* scope)
{
    return &scope->realm();
}

}
