/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <LibGC/Heap.h>
#include <LibGC/WeakInlines.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioBuffer);

struct AudioBufferChannelDataCacheEntry {
    GC::Weak<AudioBuffer> buffer;
    WebIDL::UnsignedLong channel { 0 };
    Vector<GC::Weak<JS::Float32Array>> views;
};

static Vector<AudioBufferChannelDataCacheEntry>& audio_buffer_channel_data_caches()
{
    static NeverDestroyed<Vector<AudioBufferChannelDataCacheEntry>> caches;
    return *caches;
}

static void prune_audio_buffer_channel_data_caches()
{
    audio_buffer_channel_data_caches().remove_all_matching([](auto const& entry) {
        return !entry.buffer;
    });
}

static AudioBufferChannelDataCacheEntry& cache_for(AudioBuffer& buffer, WebIDL::UnsignedLong channel)
{
    auto& caches = audio_buffer_channel_data_caches();
    prune_audio_buffer_channel_data_caches();

    for (auto& entry : caches) {
        if (entry.buffer.ptr() == &buffer && entry.channel == channel)
            return entry;
    }

    caches.append({ buffer, channel, {} });
    return caches.last();
}

static void prune_live_channel_views(Vector<GC::Weak<JS::Float32Array>>& live_channel_views)
{
    live_channel_views.remove_all_matching([](auto const& view) { return !view; });
}

static u8* audio_buffer_channel_data(void* context)
{
    return static_cast<ByteBuffer*>(context)->data();
}

static size_t audio_buffer_channel_size(void* context)
{
    return static_cast<ByteBuffer*>(context)->size();
}

WebIDL::ExceptionOr<GC::Ref<AudioBuffer>> AudioBuffer::construct_impl(JS::Realm& realm, AudioBufferOptions const& options)
{
    auto& vm = realm.vm();

    // 1. If any of the values in options lie outside its nominal range, throw a NotSupportedError exception and abort the following steps.
    TRY(BaseAudioContext::verify_audio_options_inside_nominal_range(options.number_of_channels, options.length, options.sample_rate));

    if (options.length > NumericLimits<i32>::max() / sizeof(float))
        return vm.throw_completion<JS::RangeError>(JS::ErrorType::InvalidLength, "typed array");

    // 2. Let b be a new AudioBuffer object.
    // 3. Respectively assign the values of the attributes numberOfChannels, length, sampleRate of the AudioBufferOptions passed in the
    //    constructor to the internal slots [[number of channels]], [[length]], [[sample rate]].
    // 4. Set the internal slot [[internal data]] of this AudioBuffer to the result of calling CreateByteDataBlock([[length]] * [[number of channels]]).
    return TRY_OR_THROW_OOM(vm, create(options.number_of_channels, options.length, options.sample_rate));
}

ErrorOr<GC::Ref<AudioBuffer>> AudioBuffer::create(WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    AudioBufferOptions options {};
    options.number_of_channels = number_of_channels;
    options.length = length;
    options.sample_rate = sample_rate;

    auto buffer = GC::Heap::the().allocate<AudioBuffer>(options);

    auto channel_byte_length = static_cast<size_t>(options.length) * sizeof(float);
    TRY(buffer->m_channels.try_ensure_capacity(options.number_of_channels));
    for (WebIDL::UnsignedLong i = 0; i < options.number_of_channels; ++i)
        buffer->m_channels.unchecked_append({ TRY(ByteBuffer::create_zeroed(channel_byte_length)) });

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

WebIDL::ExceptionOr<ByteBuffer*> AudioBuffer::channel_data(WebIDL::UnsignedLong channel)
{
    if (channel >= m_channels.size())
        return WebIDL::IndexSizeError::create("Channel index is out of range"_utf16);

    return &m_channels[channel].data;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffer-getchanneldata
WebIDL::ExceptionOr<GC::Ref<JS::Float32Array>> AudioBuffer::get_channel_data(JS::Realm& realm, WebIDL::UnsignedLong channel)
{
    auto channel_data = TRY(this->channel_data(channel));
    auto& cache = cache_for(*this, channel);

    prune_live_channel_views(cache.views);
    for (auto& view : cache.views) {
        if (view && &view->shape().realm() == &realm)
            return GC::Ref { *view };
    }

    JS::DataBlock::UnownedExternalBuffer external_buffer {
        GC::Ref<GC::Cell> { *this },
        channel_data,
        audio_buffer_channel_data,
        audio_buffer_channel_size
    };
    auto array_buffer = JS::ArrayBuffer::create(realm, move(external_buffer));
    auto view = JS::Float32Array::create(realm, length(), array_buffer);
    TRY_OR_THROW_OOM(realm.vm(), cache.views.try_append(view));
    return view;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffer-copyfromchannel
WebIDL::ExceptionOr<void> AudioBuffer::copy_from_channel(GC::Root<JS::Float32Array> const& destination, WebIDL::UnsignedLong channel_number, WebIDL::UnsignedLong buffer_offset) const
{
    // The copyFromChannel() method copies the samples from the specified channel of the AudioBuffer to the destination array.
    //
    // Let buffer be the AudioBuffer with Nb frames, let Nf be the number of elements in the destination array, and k be the value
    // of bufferOffset. Then the number of frames copied from buffer to destination is max(0,min(Nb−k,Nf)). If this is less than Nf,
    // then the remaining elements of destination are not modified.
    auto& vm = JS::VM::the();

    if (destination->viewed_array_buffer()->is_shared_array_buffer())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::SharedArrayBuffer, "Float32Array");

    if (channel_number >= m_channels.size())
        return WebIDL::IndexSizeError::create("Channel index is out of range"_utf16);

    auto& channel = m_channels[channel_number];
    ReadonlySpan<float> channel_data { reinterpret_cast<float const*>(channel.data.data()), m_length };
    auto channel_length = channel_data.size();
    if (buffer_offset >= channel_length)
        return {};

    auto destination_data = destination->data();
    auto count = min(destination_data.size(), channel_length - buffer_offset);
    channel_data.slice(buffer_offset, count).copy_to(destination_data.slice(0, count));

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffer-copytochannel
WebIDL::ExceptionOr<void> AudioBuffer::copy_to_channel(GC::Root<JS::Float32Array> const& source, WebIDL::UnsignedLong channel_number, WebIDL::UnsignedLong buffer_offset)
{
    // The copyToChannel() method copies the samples to the specified channel of the AudioBuffer from the source array.
    //
    // A UnknownError may be thrown if source cannot be copied to the buffer.
    //
    // Let buffer be the AudioBuffer with Nb frames, let Nf be the number of elements in the source array, and k be the value
    // of bufferOffset. Then the number of frames copied from source to the buffer is max(0,min(Nb−k,Nf)). If this is less than Nf,
    // then the remaining elements of buffer are not modified.
    auto& vm = JS::VM::the();

    if (source->viewed_array_buffer()->is_shared_array_buffer())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::SharedArrayBuffer, "Float32Array");

    if (channel_number >= m_channels.size())
        return WebIDL::IndexSizeError::create("Channel index is out of range"_utf16);

    auto& channel = m_channels[channel_number];
    Span<float> channel_data { reinterpret_cast<float*>(channel.data.data()), m_length };
    auto channel_length = channel_data.size();
    if (buffer_offset >= channel_length)
        return {};

    auto source_data = source->data();
    auto count = min(source_data.size(), channel_length - buffer_offset);
    source_data.slice(0, count).copy_to(channel_data.slice(buffer_offset, count));

    return {};
}

AudioBuffer::AudioBuffer(AudioBufferOptions const& options)
    : m_length(options.length)
    , m_sample_rate(options.sample_rate)
{
}

size_t AudioBuffer::external_memory_size() const
{
    auto size = JS::saturating_add_external_memory_size(Base::external_memory_size(), JS::vector_external_memory_size(m_channels));
    for (auto const& channel : m_channels)
        size = JS::saturating_add_external_memory_size(size, channel.data.capacity());
    return size;
}

}
