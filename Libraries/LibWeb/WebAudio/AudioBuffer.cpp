/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/AudioBuffer.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioArray.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioBuffer);

WebIDL::ExceptionOr<GC::Ref<AudioBuffer>> AudioBuffer::create(JS::Realm& realm, WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    Bindings::AudioBufferOptions options {};
    options.number_of_channels = number_of_channels;
    options.length = length;
    options.sample_rate = sample_rate;
    return construct_impl(realm, options);
}

WebIDL::ExceptionOr<GC::Ref<AudioBuffer>> AudioBuffer::construct_impl(JS::Realm& realm, Bindings::AudioBufferOptions const& options)
{
    // 1. If any of the values in options lie outside its nominal range, throw a NotSupportedError exception and abort the following steps.
    TRY(BaseAudioContext::verify_audio_options_inside_nominal_range(realm, options.number_of_channels, options.length, options.sample_rate));

    // 2. Let b be a new AudioBuffer object.
    // 3. Respectively assign the values of the attributes numberOfChannels, length, sampleRate of the AudioBufferOptions passed in the
    //    constructor to the internal slots [[number of channels]], [[length]], [[sample rate]].
    auto buffer = realm.create<AudioBuffer>(realm, options);

    // 4. Set the internal slot [[internal data]] of this AudioBuffer to the result of calling CreateByteDataBlock([[length]] * [[number of channels]]).
    buffer->m_channels.ensure_capacity(options.number_of_channels);
    for (WebIDL::UnsignedLong i = 0; i < options.number_of_channels; ++i)
        buffer->m_channels.unchecked_append(TRY(JS::Float32Array::create(realm, options.length)));

    return buffer;
}

AudioBuffer::~AudioBuffer() = default;

// https://webaudio.github.io/web-audio-api/#acquire-the-content
RefPtr<Rendering::AudioBufferContents> AudioBuffer::acquire_contents()
{
    // NB: m_contents is non-null exactly while the channel arrays are detached because step 4 below has not run yet.
    //     The snapshot then still holds the buffer's content and script has had no way to modify it, so it can be
    //     handed out again as-is. This is what makes a sequence of acquisitions with no intervening getChannelData(),
    //     e.g. one AudioBuffer played by many AudioBufferSourceNodes, free of allocations and copying.
    //     https://webaudio.github.io/web-audio-api/#audio-buffer-copying
    if (m_contents)
        return m_contents;

    // 1. If any of the AudioBuffer's ArrayBuffers are detached, return true, abort these steps, and return a
    //    zero-length channel data buffer to the invoker.
    if (any_of(m_channels, [](auto const& channel) { return channel->viewed_array_buffer()->is_detached(); }))
        return nullptr;

    // 2. Detach all ArrayBuffers for arrays previously returned by getChannelData() on this AudioBuffer.
    // 3. Retain the underlying [[internal data]] from those ArrayBuffers and return references to them to the invoker.
    Vector<Vector<float>> channels;
    channels.ensure_capacity(m_channels.size());
    for (auto const& channel : m_channels) {
        channels.unchecked_append(copy_float32_array(*channel));
        MUST(JS::detach_array_buffer(vm(), *channel->viewed_array_buffer()));
    }
    m_contents = make_ref_counted<Rendering::AudioBufferContents>(move(channels), m_sample_rate);

    // 4. Attach ArrayBuffers containing copies of the data to the AudioBuffer, to be returned by the next call to
    //    getChannelData().
    // NB: Performed lazily by attach_acquired_channels().

    return m_contents;
}

// https://webaudio.github.io/web-audio-api/#audio-buffer-copying
WebIDL::ExceptionOr<void> AudioBuffer::attach_acquired_channels()
{
    // The pending final step of acquiring this buffer's content: fresh arrays holding a copy of the acquired sample
    // data are attached the first time script reaches for the channel data again. Whoever acquired the content keeps
    // using the snapshot, so writes through the new arrays cannot change what is already playing.
    if (!m_contents)
        return {};

    GC::RootVector<GC::Ref<JS::Float32Array>> channels;
    channels.ensure_capacity(m_channels.size());
    for (auto const& samples : m_contents->channels) {
        auto channel = TRY(JS::Float32Array::create(realm(), m_length));
        overwrite_float32_array(channel, samples);
        channels.unchecked_append(channel);
    }

    m_channels = move(channels);
    m_contents = nullptr;
    return {};
}

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
WebIDL::ExceptionOr<GC::Ref<JS::Float32Array>> AudioBuffer::get_channel_data(WebIDL::UnsignedLong channel)
{
    if (channel >= m_channels.size())
        return WebIDL::IndexSizeError::create(realm(), "Channel index is out of range"_utf16);

    TRY(attach_acquired_channels());
    return m_channels[channel];
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffer-copyfromchannel
WebIDL::ExceptionOr<void> AudioBuffer::copy_from_channel(GC::Ref<JS::Float32Array> destination, WebIDL::UnsignedLong channel_number, WebIDL::UnsignedLong buffer_offset)
{
    // The copyFromChannel() method copies the samples from the specified channel of the AudioBuffer to the destination array.
    //
    // Let buffer be the AudioBuffer with Nb frames, let Nf be the number of elements in the destination array, and k be the value
    // of bufferOffset. Then the number of frames copied from buffer to destination is max(0,min(Nb−k,Nf)). If this is less than Nf,
    // then the remaining elements of destination are not modified.
    auto& vm = this->vm();

    if (destination->viewed_array_buffer()->is_shared_array_buffer())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::SharedArrayBuffer, "Float32Array");

    auto const channel = TRY(get_channel_data(channel_number));

    auto channel_record = JS::make_typed_array_with_buffer_witness_record(*channel, JS::ArrayBuffer::Order::SeqCst);
    auto channel_length = JS::is_typed_array_out_of_bounds(channel_record) ? 0 : JS::typed_array_length(channel_record);
    if (buffer_offset >= channel_length)
        return {};

    auto destination_record = JS::make_typed_array_with_buffer_witness_record(*destination, JS::ArrayBuffer::Order::SeqCst);
    auto destination_length = JS::is_typed_array_out_of_bounds(destination_record) ? 0 : JS::typed_array_length(destination_record);
    auto count = min(destination_length, channel_length - buffer_offset);
    if (count == 0)
        return {};

    auto byte_count = count * sizeof(float);
    auto source_byte_offset = channel->byte_offset() + buffer_offset * sizeof(float);
    auto destination_byte_offset = destination->byte_offset();
    auto& source_buffer = *channel->viewed_array_buffer();
    auto& destination_buffer = *destination->viewed_array_buffer();
    if (source_buffer.shares_storage_with(destination_buffer))
        destination_buffer.move_data(destination_byte_offset, source_byte_offset, byte_count);
    else
        source_buffer.copy_data_to(destination_buffer, source_byte_offset, destination_byte_offset, byte_count);

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffer-copytochannel
WebIDL::ExceptionOr<void> AudioBuffer::copy_to_channel(GC::Ref<JS::Float32Array> source, WebIDL::UnsignedLong channel_number, WebIDL::UnsignedLong buffer_offset)
{
    // The copyToChannel() method copies the samples to the specified channel of the AudioBuffer from the source array.
    //
    // A UnknownError may be thrown if source cannot be copied to the buffer.
    //
    // Let buffer be the AudioBuffer with Nb frames, let Nf be the number of elements in the source array, and k be the value
    // of bufferOffset. Then the number of frames copied from source to the buffer is max(0,min(Nb−k,Nf)). If this is less than Nf,
    // then the remaining elements of buffer are not modified.
    auto& vm = this->vm();

    if (source->viewed_array_buffer()->is_shared_array_buffer())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::SharedArrayBuffer, "Float32Array");

    auto channel = TRY(get_channel_data(channel_number));

    auto channel_record = JS::make_typed_array_with_buffer_witness_record(*channel, JS::ArrayBuffer::Order::SeqCst);
    auto channel_length = JS::is_typed_array_out_of_bounds(channel_record) ? 0 : JS::typed_array_length(channel_record);
    if (buffer_offset >= channel_length)
        return {};

    auto source_record = JS::make_typed_array_with_buffer_witness_record(*source, JS::ArrayBuffer::Order::SeqCst);
    auto source_length = JS::is_typed_array_out_of_bounds(source_record) ? 0 : JS::typed_array_length(source_record);
    auto count = min(source_length, channel_length - buffer_offset);
    if (count == 0)
        return {};

    auto byte_count = count * sizeof(float);
    auto source_byte_offset = source->byte_offset();
    auto destination_byte_offset = channel->byte_offset() + buffer_offset * sizeof(float);
    auto& source_buffer = *source->viewed_array_buffer();
    auto& destination_buffer = *channel->viewed_array_buffer();
    if (source_buffer.shares_storage_with(destination_buffer))
        destination_buffer.move_data(destination_byte_offset, source_byte_offset, byte_count);
    else
        source_buffer.copy_data_to(destination_buffer, source_byte_offset, destination_byte_offset, byte_count);

    return {};
}

AudioBuffer::AudioBuffer(JS::Realm& realm, Bindings::AudioBufferOptions const& options)
    : Bindings::PlatformObject(realm)
    , m_length(options.length)
    , m_sample_rate(options.sample_rate)
{
}

void AudioBuffer::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioBuffer);
    Base::initialize(realm);
}

void AudioBuffer::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_channels);
}

size_t AudioBuffer::external_memory_size() const
{
    auto size = JS::saturating_add_external_memory_size(Base::external_memory_size(), JS::vector_external_memory_size(m_channels));
    if (m_contents)
        size = JS::saturating_add_external_memory_size(size, m_contents->channels.size() * m_length * sizeof(float));
    return size;
}

}
