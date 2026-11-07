/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

extern "C" {
bool web_rust_audio_buffer_create(JSVM*, u32 channels, u32 length, float sample_rate, void**);
float web_rust_audio_buffer_sample_rate(void*);
u32 web_rust_audio_buffer_length(void*);
u32 web_rust_audio_buffer_channel_count(void*);
bool web_rust_audio_buffer_channel(void*, u32 channel, Bytes*);
JSCompletion web_rust_audio_buffer_get_channel_data(JSVM*, void* realm, void* buffer, u32 channel);
void const* web_rust_audio_buffer_acquire_contents(JSVM*, void* buffer);
}

ErrorOr<GC::Ref<AudioBuffer>> AudioBuffer::create(WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    void* buffer = nullptr;
    if (!web_rust_audio_buffer_create(JS::HostABI::vm_to_abi(JS::VM::the()), number_of_channels, length, sample_rate, &buffer))
        return Error::from_errno(ENOMEM);
    return GC::Ref { *static_cast<AudioBuffer*>(buffer) };
}
float AudioBuffer::sample_rate() const { return web_rust_audio_buffer_sample_rate(const_cast<AudioBuffer*>(this)); }
u32 AudioBuffer::length() const { return web_rust_audio_buffer_length(const_cast<AudioBuffer*>(this)); }
u32 AudioBuffer::number_of_channels() const { return web_rust_audio_buffer_channel_count(const_cast<AudioBuffer*>(this)); }
double AudioBuffer::duration() const { return length() / static_cast<double>(sample_rate()); }
WebIDL::ExceptionOr<Bytes> AudioBuffer::channel_data(WebIDL::UnsignedLong channel)
{
    Bytes data;
    if (!web_rust_audio_buffer_channel(this, channel, &data))
        return WebIDL::IndexSizeError::create("Channel index is out of range"_utf16);
    return data;
}
WebIDL::ExceptionOr<GC::Ref<JS::Float32Array>> AudioBuffer::get_channel_data(JS::Object& relevant_global_object, WebIDL::UnsignedLong channel)
{
    auto& realm = HTML::relevant_realm(relevant_global_object);
    auto result = TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_audio_buffer_get_channel_data(JS::HostABI::vm_to_abi(realm.vm()), &realm, this, channel)));
    return GC::Ref { static_cast<JS::Float32Array&>(result.as_object()) };
}
RefPtr<Rendering::AudioBufferContents> AudioBuffer::acquire_contents()
{
    auto* contents = web_rust_audio_buffer_acquire_contents(JS::HostABI::vm_to_abi(JS::VM::the()), this);
    if (!contents)
        return nullptr;
    return make_ref_counted<Rendering::AudioBufferContents>(contents, number_of_channels(), sample_rate());
}

}
