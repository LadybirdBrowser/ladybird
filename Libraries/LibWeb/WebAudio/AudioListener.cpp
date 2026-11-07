/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/WebAudio/AudioListener.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>

namespace Web::WebAudio {

extern "C" {
void* web_rust_audio_listener_create(JSVM*, void* context);
void* web_rust_audio_listener_param(void*, size_t index);
}
GC::Ref<AudioListener> AudioListener::create(GC::Ref<BaseAudioContext> context)
{
    return *static_cast<AudioListener*>(web_rust_audio_listener_create(JS::HostABI::vm_to_abi(JS::VM::the()), context.ptr()));
}
GC::Ref<AudioParam> AudioListener::forward_x() const { return *static_cast<AudioParam*>(web_rust_audio_listener_param(const_cast<AudioListener*>(this), 0)); }
GC::Ref<AudioParam> AudioListener::forward_y() const { return *static_cast<AudioParam*>(web_rust_audio_listener_param(const_cast<AudioListener*>(this), 1)); }
GC::Ref<AudioParam> AudioListener::forward_z() const { return *static_cast<AudioParam*>(web_rust_audio_listener_param(const_cast<AudioListener*>(this), 2)); }
GC::Ref<AudioParam> AudioListener::position_x() const { return *static_cast<AudioParam*>(web_rust_audio_listener_param(const_cast<AudioListener*>(this), 3)); }
GC::Ref<AudioParam> AudioListener::position_y() const { return *static_cast<AudioParam*>(web_rust_audio_listener_param(const_cast<AudioListener*>(this), 4)); }
GC::Ref<AudioParam> AudioListener::position_z() const { return *static_cast<AudioParam*>(web_rust_audio_listener_param(const_cast<AudioListener*>(this), 5)); }
GC::Ref<AudioParam> AudioListener::up_x() const { return *static_cast<AudioParam*>(web_rust_audio_listener_param(const_cast<AudioListener*>(this), 6)); }
GC::Ref<AudioParam> AudioListener::up_y() const { return *static_cast<AudioParam*>(web_rust_audio_listener_param(const_cast<AudioListener*>(this), 7)); }
GC::Ref<AudioParam> AudioListener::up_z() const { return *static_cast<AudioParam*>(web_rust_audio_listener_param(const_cast<AudioListener*>(this), 8)); }

}
