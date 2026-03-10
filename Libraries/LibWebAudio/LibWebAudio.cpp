/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/WebAudio/AudioParamDescriptor.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>
#include <LibWebAudio/LibWebAudio.h>
#include <errno.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::WebAudio::AudioParamDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.name.to_string()));
    TRY(encoder.encode(descriptor.default_value));
    TRY(encoder.encode(descriptor.min_value));
    TRY(encoder.encode(descriptor.max_value));
    TRY(encoder.encode(static_cast<u8>(descriptor.automation_rate)));
    return {};
}

template<>
ErrorOr<Web::WebAudio::AudioParamDescriptor> decode(Decoder& decoder)
{
    Web::WebAudio::AudioParamDescriptor descriptor;
    descriptor.name = TRY(decoder.decode<String>());
    descriptor.default_value = TRY(decoder.decode<float>());
    descriptor.min_value = TRY(decoder.decode<float>());
    descriptor.max_value = TRY(decoder.decode<float>());
    descriptor.automation_rate = static_cast<Web::Bindings::AutomationRate>(TRY(decoder.decode<u8>()));
    return descriptor;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::WebAudio::Render::RingStreamFormat const& format)
{
    TRY(encoder.encode(format.sample_rate_hz));
    TRY(encoder.encode(format.channel_count));
    TRY(encoder.encode(format.channel_capacity));
    TRY(encoder.encode(format.capacity_frames));
    return {};
}

template<>
ErrorOr<Web::WebAudio::Render::RingStreamFormat> decode(Decoder& decoder)
{
    Web::WebAudio::Render::RingStreamFormat format;
    format.sample_rate_hz = TRY(decoder.decode<u32>());
    format.channel_count = TRY(decoder.decode<u32>());
    format.channel_capacity = TRY(decoder.decode<u32>());
    format.capacity_frames = TRY(decoder.decode<u64>());
    return format;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::WebAudio::Render::RingStreamDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.stream_id));
    TRY(encoder.encode(descriptor.format));
    TRY(encoder.encode(descriptor.shared_memory));
    TRY(encoder.encode(descriptor.notify_fd));
    return {};
}

template<>
ErrorOr<Web::WebAudio::Render::RingStreamDescriptor> decode(Decoder& decoder)
{
    Web::WebAudio::Render::RingStreamDescriptor descriptor;
    descriptor.stream_id = TRY(decoder.decode<Web::WebAudio::Render::StreamID>());
    descriptor.format = TRY(decoder.decode<Web::WebAudio::Render::RingStreamFormat>());
    descriptor.shared_memory = TRY(decoder.decode<Core::AnonymousBuffer>());
    descriptor.notify_fd = TRY(decoder.decode<IPC::File>());
    return descriptor;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::WebAudio::Render::AudioInputStreamMetadata const& metadata)
{
    TRY(encoder.encode(metadata.device_handle));
    TRY(encoder.encode(metadata.sample_rate_hz));
    TRY(encoder.encode(metadata.channel_count));
    TRY(encoder.encode(metadata.capacity_frames));
    return {};
}

template<>
ErrorOr<Web::WebAudio::Render::AudioInputStreamMetadata> decode(Decoder& decoder)
{
    Web::WebAudio::Render::AudioInputStreamMetadata metadata;
    metadata.device_handle = TRY(decoder.decode<AudioServer::DeviceHandle>());
    metadata.sample_rate_hz = TRY(decoder.decode<u32>());
    metadata.channel_count = TRY(decoder.decode<u32>());
    metadata.capacity_frames = TRY(decoder.decode<u64>());
    return metadata;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.provider_id));
    TRY(encoder.encode(descriptor.ring_stream));
    return {};
}

template<>
ErrorOr<Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor> decode(Decoder& decoder)
{
    Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor descriptor;
    descriptor.provider_id = TRY(decoder.decode<u64>());
    descriptor.ring_stream = TRY(decoder.decode<Web::WebAudio::Render::RingStreamDescriptor>());
    return descriptor;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.provider_id));
    TRY(encoder.encode(descriptor.metadata));
    return {};
}

template<>
ErrorOr<Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor> decode(Decoder& decoder)
{
    Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor descriptor;
    descriptor.provider_id = TRY(decoder.decode<u64>());
    descriptor.metadata = TRY(decoder.decode<Web::WebAudio::Render::AudioInputStreamMetadata>());
    return descriptor;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::WebAudio::Render::SharedBufferStreamDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.pool_buffer));
    TRY(encoder.encode(descriptor.ready_ring_buffer));
    TRY(encoder.encode(descriptor.free_ring_buffer));
    return {};
}

template<>
ErrorOr<Web::WebAudio::Render::SharedBufferStreamDescriptor> decode(Decoder& decoder)
{
    Web::WebAudio::Render::SharedBufferStreamDescriptor descriptor;
    descriptor.pool_buffer = TRY(decoder.decode<Core::AnonymousBuffer>());
    descriptor.ready_ring_buffer = TRY(decoder.decode<Core::AnonymousBuffer>());
    descriptor.free_ring_buffer = TRY(decoder.decode<Core::AnonymousBuffer>());
    return descriptor;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::WebAudio::Render::ScriptProcessorStreamDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.node_id));
    TRY(encoder.encode(descriptor.buffer_size));
    TRY(encoder.encode(descriptor.input_channel_count));
    TRY(encoder.encode(descriptor.output_channel_count));
    TRY(encoder.encode(descriptor.request_stream));
    TRY(encoder.encode(descriptor.response_stream));
    TRY(encoder.encode(descriptor.request_notify_write_fd));
    return {};
}

template<>
ErrorOr<Web::WebAudio::Render::ScriptProcessorStreamDescriptor> decode(Decoder& decoder)
{
    Web::WebAudio::Render::ScriptProcessorStreamDescriptor descriptor;
    descriptor.node_id = TRY(decoder.decode<u64>());
    descriptor.buffer_size = TRY(decoder.decode<u32>());
    descriptor.input_channel_count = TRY(decoder.decode<u32>());
    descriptor.output_channel_count = TRY(decoder.decode<u32>());
    descriptor.request_stream = TRY(decoder.decode<Web::WebAudio::Render::SharedBufferStreamDescriptor>());
    descriptor.response_stream = TRY(decoder.decode<Web::WebAudio::Render::SharedBufferStreamDescriptor>());
    descriptor.request_notify_write_fd = TRY(decoder.decode<IPC::File>());
    return descriptor;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::WebAudio::Render::WorkletNodePortDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.node_id));
    TRY(encoder.encode(descriptor.processor_port_fd));
    return {};
}

template<>
ErrorOr<Web::WebAudio::Render::WorkletNodePortDescriptor> decode(Decoder& decoder)
{
    Web::WebAudio::Render::WorkletNodePortDescriptor descriptor;
    descriptor.node_id = TRY(decoder.decode<u64>());
    descriptor.processor_port_fd = TRY(decoder.decode<IPC::File>());
    return descriptor;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::WebAudio::Render::WorkletParameterDataEntry const& entry)
{
    TRY(encoder.encode(entry.name));
    TRY(encoder.encode(entry.value));
    return {};
}

template<>
ErrorOr<Web::WebAudio::Render::WorkletParameterDataEntry> decode(Decoder& decoder)
{
    Web::WebAudio::Render::WorkletParameterDataEntry entry;
    entry.name = TRY(decoder.decode<String>());
    entry.value = TRY(decoder.decode<double>());
    return entry;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::WebAudio::Render::WorkletNodeDefinition const& definition)
{
    TRY(encoder.encode(static_cast<u64>(definition.node_id)));
    TRY(encoder.encode(definition.processor_name));
    TRY(encoder.encode(static_cast<u64>(definition.number_of_inputs)));
    TRY(encoder.encode(static_cast<u64>(definition.number_of_outputs)));

    Vector<u64> output_channel_count;
    if (definition.output_channel_count.has_value()) {
        output_channel_count.ensure_capacity(definition.output_channel_count->size());
        for (size_t value : *definition.output_channel_count)
            output_channel_count.unchecked_append(static_cast<u64>(value));
    }
    TRY(encoder.encode(output_channel_count));

    TRY(encoder.encode(definition.output_channel_count.has_value()));

    TRY(encoder.encode(static_cast<u64>(definition.channel_count)));
    TRY(encoder.encode(static_cast<u8>(definition.channel_count_mode)));
    TRY(encoder.encode(static_cast<u8>(definition.channel_interpretation)));

    TRY(encoder.encode(definition.parameter_names));

    TRY(encoder.encode(definition.parameter_data));
    TRY(encoder.encode(definition.serialized_processor_options));
    return {};
}

template<>
ErrorOr<Web::WebAudio::Render::WorkletNodeDefinition> decode(Decoder& decoder)
{
    Web::WebAudio::Render::WorkletNodeDefinition definition;
    definition.node_id = Web::WebAudio::NodeID { TRY(decoder.decode<u64>()) };
    definition.processor_name = TRY(decoder.decode<String>());
    definition.number_of_inputs = static_cast<size_t>(TRY(decoder.decode<u64>()));
    definition.number_of_outputs = static_cast<size_t>(TRY(decoder.decode<u64>()));

    Vector<u64> output_channel_count = TRY(decoder.decode<Vector<u64>>());
    bool output_channel_count_was_provided = TRY(decoder.decode<bool>());
    if (output_channel_count_was_provided) {
        Vector<size_t> counts;
        counts.ensure_capacity(output_channel_count.size());
        for (u64 value : output_channel_count)
            counts.unchecked_append(static_cast<size_t>(value));
        definition.output_channel_count = move(counts);
    }

    definition.channel_count = static_cast<size_t>(TRY(decoder.decode<u64>()));
    definition.channel_count_mode = static_cast<Web::WebAudio::Render::ChannelCountMode>(TRY(decoder.decode<u8>()));
    definition.channel_interpretation = static_cast<Web::WebAudio::Render::ChannelInterpretation>(TRY(decoder.decode<u8>()));

    definition.parameter_names = TRY(decoder.decode<Vector<String>>());

    definition.parameter_data = TRY(decoder.decode<Optional<Vector<Web::WebAudio::Render::WorkletParameterDataEntry>>>());
    definition.serialized_processor_options = TRY(decoder.decode<Optional<Web::HTML::SerializationRecord>>());
    return definition;
}

}

namespace Web::WebAudio::Render {

void write_timing_page(TimingFeedbackPage& page, u32 sample_rate_hz, u32 channel_count, u64 rendered_frames_total, u64 underrun_frames_total, u64 graph_generation, bool is_suspended, u64 suspend_generation)
{
    u32 seq = AK::atomic_load(&page.sequence, AK::MemoryOrder::memory_order_relaxed);
    seq = seq + 1;
    if ((seq & 1u) == 0)
        seq++;

    AK::atomic_store(&page.sequence, seq, AK::MemoryOrder::memory_order_release);

    page.sample_rate_hz = sample_rate_hz;
    page.channel_count = channel_count;
    page.rendered_frames_total = rendered_frames_total;
    page.underrun_frames_total = underrun_frames_total;
    page.graph_generation = graph_generation;
    page.is_suspended = is_suspended ? 1u : 0u;
    page.suspend_generation = suspend_generation;

    AK::atomic_store(&page.sequence, seq + 1, AK::MemoryOrder::memory_order_release);
}

bool read_timing_page(TimingFeedbackPage const& page, TimingFeedbackPage& out)
{
    for (size_t attempt = 0; attempt < 4; ++attempt) {
        u32 s0 = AK::atomic_load(&page.sequence, AK::MemoryOrder::memory_order_acquire);
        if ((s0 & 1u) != 0)
            continue;

        TimingFeedbackPage snapshot;
        snapshot.sequence = s0;
        snapshot.sample_rate_hz = page.sample_rate_hz;
        snapshot.channel_count = page.channel_count;
        snapshot.rendered_frames_total = page.rendered_frames_total;
        snapshot.underrun_frames_total = page.underrun_frames_total;
        snapshot.graph_generation = page.graph_generation;
        snapshot.is_suspended = page.is_suspended;
        snapshot.suspend_generation = page.suspend_generation;

        u32 s1 = AK::atomic_load(&page.sequence, AK::MemoryOrder::memory_order_acquire);
        if (s0 != s1)
            continue;

        out = snapshot;
        return true;
    }

    return false;
}

bool try_signal_event_fd(int fd)
{
    if (fd < 0)
        return false;

    u64 one = 1;
    ssize_t nwritten = ::write(fd, &one, sizeof(one));
    if (nwritten == static_cast<ssize_t>(sizeof(one)))
        return true;

    if (nwritten < 0 && (errno == EAGAIN || (EAGAIN != EWOULDBLOCK && errno == EWOULDBLOCK)))
        return true;

    return false;
}

void drain_event_fd(int fd)
{
    if (fd < 0)
        return;

    u64 value = 0;
    while (true) {
        ssize_t nread = ::read(fd, &value, sizeof(value));
        if (nread == static_cast<ssize_t>(sizeof(value)))
            continue;
        if (nread < 0 && (errno == EAGAIN || (EAGAIN != EWOULDBLOCK && errno == EWOULDBLOCK)))
            return;

        if (nread > 0 || (nread < 0 && errno == EINVAL)) {
            u8 buffer[64];
            while (true) {
                ssize_t pipe_read = ::read(fd, buffer, sizeof(buffer));
                if (pipe_read > 0)
                    continue;
                if (pipe_read < 0 && (errno == EAGAIN || (EAGAIN != EWOULDBLOCK && errno == EWOULDBLOCK)))
                    break;
                break;
            }
        }
        return;
    }
}

}
