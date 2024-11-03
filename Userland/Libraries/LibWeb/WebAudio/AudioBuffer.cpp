/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/AudioBufferPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(AudioBuffer);

WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioBuffer>> AudioBuffer::create(JS::Realm& realm, WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    return construct_impl(realm, { number_of_channels, length, sample_rate });
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioBuffer>> AudioBuffer::construct_impl(JS::Realm& realm, AudioBufferOptions const& options)
{
    auto& vm = realm.vm();

    // 1. If any of the values in options lie outside its nominal range, throw a NotSupportedError exception and abort the following steps.
    TRY(BaseAudioContext::verify_audio_options_inside_nominal_range(realm, options.number_of_channels, options.length, options.sample_rate));

    // 2. Let b be a new AudioBuffer object.
    // 3. Respectively assign the values of the attributes numberOfChannels, length, sampleRate of the AudioBufferOptions passed in the
    //    constructor to the internal slots [[number of channels]], [[length]], [[sample rate]].
    auto buffer = vm.heap().allocate<AudioBuffer>(realm, realm, options);

    // 4. Set the internal slot [[internal data]] of this AudioBuffer to the result of calling CreateByteDataBlock([[length]] * [[number of channels]]).
    buffer->m_channels.ensure_capacity(options.number_of_channels);
    for (WebIDL::UnsignedLong i = 0; i < options.number_of_channels; ++i)
        buffer->m_channels.unchecked_append(TRY(JS::Float32Array::create(realm, options.length)));

    return buffer;
}

AudioBuffer::~AudioBuffer() = default;

// https://webaudio.github.io/web-audio-api/#dom-audiobuffer-samplerate
float AudioBuffer::sample_rate() const
{
    // The sample-rate for the PCM audio data in samples per second. This MUST return the value of [[sample rate]].
    return m_sample_rate;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffer-length
WebIDL::UnsignedLong AudioBuffer::length() const
{
    // Length of the PCM audio data in sample-frames. This MUST return the value of [[length]].
    return m_length;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffer-duration
double AudioBuffer::duration() const
{
    // Duration of the PCM audio data in seconds.
    // This is computed from the [[sample rate]] and the [[length]] of the AudioBuffer by performing a division between the [[length]] and the [[sample rate]].
    return m_length / static_cast<double>(m_sample_rate);
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffer-numberofchannels
WebIDL::UnsignedLong AudioBuffer::number_of_channels() const
{
    // The number of discrete audio channels. This MUST return the value of [[number of channels]].
    return m_channels.size();
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffer-getchanneldata
WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Float32Array>> AudioBuffer::get_channel_data(WebIDL::UnsignedLong channel) const
{
    if (channel >= m_channels.size())
        return WebIDL::IndexSizeError::create(realm(), "Channel index is out of range"_string);

    return m_channels[channel];
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffer-copyfromchannel
WebIDL::ExceptionOr<void> AudioBuffer::copy_from_channel(JS::Handle<WebIDL::BufferSource> const& destination, WebIDL::UnsignedLong channel_number, WebIDL::UnsignedLong buffer_offset) const
{
    // The copyFromChannel() method copies the samples from the specified channel of the AudioBuffer to the destination array.
    //
    // Let buffer be the AudioBuffer with Nb frames, let Nf be the number of elements in the destination array, and k be the value
    // of bufferOffset. Then the number of frames copied from buffer to destination is max(0,min(Nb−k,Nf)). If this is less than Nf,
    // then the remaining elements of destination are not modified.
    auto& vm = this->vm();

    if (!is<JS::Float32Array>(*destination->raw_object()))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Float32Array");
    auto& float32_array = static_cast<JS::Float32Array&>(*destination->raw_object());

    auto const channel = TRY(get_channel_data(channel_number));

    auto channel_length = channel->data().size();
    if (buffer_offset >= channel_length)
        return {};

    u32 count = min(float32_array.data().size(), channel_length - buffer_offset);
    channel->data().slice(buffer_offset, count).copy_to(float32_array.data());

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffer-copytochannel
WebIDL::ExceptionOr<void> AudioBuffer::copy_to_channel(JS::Handle<WebIDL::BufferSource> const& source, WebIDL::UnsignedLong channel_number, WebIDL::UnsignedLong buffer_offset)
{
    // The copyToChannel() method copies the samples to the specified channel of the AudioBuffer from the source array.
    //
    // A UnknownError may be thrown if source cannot be copied to the buffer.
    //
    // Let buffer be the AudioBuffer with Nb frames, let Nf be the number of elements in the source array, and k be the value
    // of bufferOffset. Then the number of frames copied from source to the buffer is max(0,min(Nb−k,Nf)). If this is less than Nf,
    // then the remaining elements of buffer are not modified.
    auto& vm = this->vm();

    if (!is<JS::Float32Array>(*source->raw_object()))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Float32Array");
    auto const& float32_array = static_cast<JS::Float32Array const&>(*source->raw_object());

    auto channel = TRY(get_channel_data(channel_number));

    auto channel_length = channel->data().size();
    if (buffer_offset >= channel_length)
        return {};

    u32 count = min(float32_array.data().size(), channel_length - buffer_offset);
    float32_array.data().slice(0, count).copy_to(channel->data().slice(buffer_offset, count));

    return {};
}

AudioBuffer::AudioBuffer(JS::Realm& realm, AudioBufferOptions const& options)
    : Bindings::PlatformObject(realm)
    , m_length(options.length)
    , m_sample_rate(options.sample_rate)
{
}

void AudioBuffer::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioBuffer);
}

void AudioBuffer::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_channels);
}

}
