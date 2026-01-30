/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AudioServer/AudioInputStreamManager.h>

#include <AK/Atomic.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <AudioServer/AudioInputDeviceManager.h>
#include <AudioServer/AudioInputRingStream.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/System.h>
#include <LibThreading/Thread.h>

#if defined(AK_OS_MACOS)
#    include <AudioToolbox/AudioToolbox.h>
#    include <CoreAudio/CoreAudio.h>
#endif

#if defined(HAVE_PULSEAUDIO)
#    include <pulse/error.h>
#    include <pulse/simple.h>
#endif

namespace AudioServer {

class AudioInputStream : public RefCounted<AudioInputStream> {
public:
    virtual ~AudioInputStream()
    {
        if (m_notify_write_fd >= 0)
            (void)Core::System::close(m_notify_write_fd);
    }

    AudioInputStreamDescriptor const& descriptor() const { return m_descriptor; }
    ErrorOr<AudioInputStreamDescriptor> descriptor_for_ipc() const
    {
        auto notify_fd = TRY(IPC::File::clone_fd(m_descriptor.notify_fd.fd()));
        return AudioInputStreamDescriptor {
            .stream_id = m_descriptor.stream_id,
            .format = m_descriptor.format,
            .overflow_policy = m_descriptor.overflow_policy,
            .shared_memory = m_descriptor.shared_memory,
            .notify_fd = move(notify_fd),
        };
    }

    void set_stream_id(AudioInputStreamID id) { m_descriptor.stream_id = id; }

protected:
    ErrorOr<void> create_notify_pipe()
    {
        auto pipe_fds_or_error = Core::System::pipe2(O_CLOEXEC);
        if (pipe_fds_or_error.is_error())
            return pipe_fds_or_error.release_error();

        int read_fd = pipe_fds_or_error.value()[0];
        int write_fd = pipe_fds_or_error.value()[1];
        m_descriptor.notify_fd = IPC::File::adopt_fd(read_fd);
        m_notify_write_fd = write_fd;
        return {};
    }

    AudioInputStreamDescriptor m_descriptor;
    RingStreamView m_view;
    StreamOverflowPolicy m_overflow_policy { StreamOverflowPolicy::DropOldest };
    int m_notify_write_fd { -1 };
};

#if defined(AK_OS_MACOS)
class CoreAudioInputStream final : public AudioInputStream {
public:
    static ErrorOr<NonnullRefPtr<CoreAudioInputStream>> create(AudioInputDeviceID device_id, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames, StreamOverflowPolicy overflow_policy)
    {
        auto stream = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) CoreAudioInputStream));
        stream->m_overflow_policy = overflow_policy;

        TRY(stream->initialize_ring(sample_rate_hz, channel_count, capacity_frames));
        TRY(stream->initialize_unit(device_id, sample_rate_hz, channel_count));

        return stream;
    }

    ~CoreAudioInputStream() override
    {
        if (m_unit) {
            AudioOutputUnitStop(m_unit);
            AudioUnitUninitialize(m_unit);
            AudioComponentInstanceDispose(m_unit);
        }
    }

private:
    CoreAudioInputStream() = default;

    ErrorOr<void> initialize_ring(u32 sample_rate_hz, u32 channel_count, u64 capacity_frames)
    {
        if (sample_rate_hz == 0 || channel_count == 0 || capacity_frames == 0)
            return Error::from_string_literal("invalid ring stream format");

        u32 channel_capacity = channel_count;
        size_t total_bytes = ring_stream_bytes_total(channel_capacity, capacity_frames);
        m_descriptor.shared_memory = TRY(Core::AnonymousBuffer::create_with_size(total_bytes));

        auto* header = m_descriptor.shared_memory.data<RingStreamHeader>();
        if (!header)
            return Error::from_string_literal("failed to map ring stream header");

        __builtin_memset(header, 0, sizeof(RingStreamHeader));
        ring_stream_initialize_header(*header, sample_rate_hz, channel_count, channel_capacity, capacity_frames);

        m_view.header = header;
        auto* data = reinterpret_cast<float*>(header + 1);
        m_view.interleaved_frames = { data, ring_stream_bytes_for_data(channel_capacity, capacity_frames) / sizeof(float) };

        m_descriptor.stream_id = 0;
        m_descriptor.format = RingStreamFormat {
            .sample_rate_hz = sample_rate_hz,
            .channel_count = channel_count,
            .channel_capacity = channel_capacity,
            .capacity_frames = capacity_frames,
        };
        m_descriptor.overflow_policy = m_overflow_policy;
        TRY(create_notify_pipe());

        return {};
    }

    ErrorOr<void> initialize_unit(AudioInputDeviceID device_id, u32 sample_rate_hz, u32 channel_count)
    {
        AudioComponentDescription description {};
        description.componentType = kAudioUnitType_Output;
        description.componentSubType = kAudioUnitSubType_HALOutput;
        description.componentManufacturer = kAudioUnitManufacturer_Apple;

        auto* component = AudioComponentFindNext(nullptr, &description);
        if (!component)
            return Error::from_string_literal("failed to locate HAL output audio unit");

        OSStatus status = AudioComponentInstanceNew(component, &m_unit);
        if (status != noErr)
            return Error::from_errno(status);

        UInt32 enable_input = 1;
        status = AudioUnitSetProperty(m_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, 1, &enable_input, sizeof(enable_input));
        if (status != noErr)
            return Error::from_errno(status);

        UInt32 disable_output = 0;
        status = AudioUnitSetProperty(m_unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Output, 0, &disable_output, sizeof(disable_output));
        if (status != noErr)
            return Error::from_errno(status);

        AudioObjectID device = static_cast<AudioObjectID>(device_id);
        status = AudioUnitSetProperty(m_unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, sizeof(device));
        if (status != noErr)
            return Error::from_errno(status);

        AudioStreamBasicDescription format {};
        format.mSampleRate = static_cast<Float64>(sample_rate_hz);
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        format.mBitsPerChannel = 32;
        format.mChannelsPerFrame = channel_count;
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = channel_count * sizeof(float);
        format.mBytesPerPacket = format.mFramesPerPacket * format.mBytesPerFrame;

        status = AudioUnitSetProperty(m_unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 1, &format, sizeof(format));
        if (status != noErr)
            return Error::from_errno(status);

        AURenderCallbackStruct callback {};
        callback.inputProc = &CoreAudioInputStream::input_callback;
        callback.inputProcRefCon = this;
        status = AudioUnitSetProperty(m_unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, 0, &callback, sizeof(callback));
        if (status != noErr)
            return Error::from_errno(status);

        status = AudioUnitInitialize(m_unit);
        if (status != noErr)
            return Error::from_errno(status);

        status = AudioOutputUnitStart(m_unit);
        if (status != noErr)
            return Error::from_errno(status);

        return {};
    }

    static OSStatus input_callback(void* user_data, AudioUnitRenderActionFlags* flags, AudioTimeStamp const* timestamp, UInt32 bus_number, UInt32 frames, [[maybe_unused]] AudioBufferList* io_data)
    {
        auto* stream = static_cast<CoreAudioInputStream*>(user_data);
        if (!stream || !stream->m_unit)
            return noErr;

        size_t required_samples = static_cast<size_t>(frames) * stream->m_descriptor.format.channel_count;
        if (stream->m_input_buffer.size() < required_samples)
            stream->m_input_buffer.resize(required_samples);

        AudioBufferList buffer_list {};
        buffer_list.mNumberBuffers = 1;
        buffer_list.mBuffers[0].mNumberChannels = stream->m_descriptor.format.channel_count;
        buffer_list.mBuffers[0].mDataByteSize = static_cast<UInt32>(required_samples * sizeof(float));
        buffer_list.mBuffers[0].mData = stream->m_input_buffer.data();

        OSStatus status = AudioUnitRender(stream->m_unit, flags, timestamp, bus_number, frames, &buffer_list);
        if (status != noErr)
            return status;

        ReadonlySpan<float> interleaved { stream->m_input_buffer.data(), required_samples };
        (void)ring_stream_try_push_interleaved(stream->m_view, interleaved, stream->m_descriptor.format.channel_count, stream->m_overflow_policy);
        return noErr;
    }

    AudioComponentInstance m_unit { nullptr };
    Vector<float> m_input_buffer;
};
#endif

#if defined(HAVE_PULSEAUDIO)
class PulseAudioInputStream final : public AudioInputStream {
public:
    static ErrorOr<NonnullRefPtr<PulseAudioInputStream>> create(ByteString device_name, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames, StreamOverflowPolicy overflow_policy)
    {
        auto stream = adopt_nonnull_ref_or_enomem(new (nothrow) PulseAudioInputStream(move(device_name)));
        stream->m_overflow_policy = overflow_policy;
        TRY(stream->initialize_ring(sample_rate_hz, channel_count, capacity_frames));
        TRY(stream->initialize_stream(sample_rate_hz, channel_count));
        return stream;
    }

    ~PulseAudioInputStream() override
    {
        m_should_stop.store(true, AK::MemoryOrder::memory_order_release);
        if (m_thread && m_thread->needs_to_be_joined())
            (void)m_thread->join();

        if (m_stream)
            pa_simple_free(m_stream);
    }

private:
    explicit PulseAudioInputStream(ByteString device_name)
        : m_device_name(move(device_name))
    {
    }

    ErrorOr<void> initialize_ring(u32 sample_rate_hz, u32 channel_count, u64 capacity_frames)
    {
        if (sample_rate_hz == 0 || channel_count == 0 || capacity_frames == 0)
            return Error::from_string_literal("invalid ring stream format");

        u32 channel_capacity = channel_count;
        size_t total_bytes = ring_stream_bytes_total(channel_capacity, capacity_frames);
        m_descriptor.shared_memory = TRY(Core::AnonymousBuffer::create_with_size(total_bytes));

        auto* header = m_descriptor.shared_memory.data<RingStreamHeader>();
        if (!header)
            return Error::from_string_literal("failed to map ring stream header");

        __builtin_memset(header, 0, sizeof(RingStreamHeader));
        ring_stream_initialize_header(*header, sample_rate_hz, channel_count, channel_capacity, capacity_frames);

        m_view.header = header;
        auto* data = reinterpret_cast<float*>(header + 1);
        m_view.interleaved_frames = { data, ring_stream_bytes_for_data(channel_capacity, capacity_frames) / sizeof(float) };

        m_descriptor.stream_id = 0;
        m_descriptor.format = RingStreamFormat {
            .sample_rate_hz = sample_rate_hz,
            .channel_count = channel_count,
            .channel_capacity = channel_capacity,
            .capacity_frames = capacity_frames,
        };
        m_descriptor.overflow_policy = m_overflow_policy;
        TRY(create_notify_pipe());

        return {};
    }

    ErrorOr<void> initialize_stream(u32 sample_rate_hz, u32 channel_count)
    {
        pa_sample_spec spec {};
        spec.format = PA_SAMPLE_FLOAT32LE;
        spec.rate = sample_rate_hz;
        spec.channels = channel_count;

        int error = 0;
        m_stream = pa_simple_new(nullptr, "Ladybird", PA_STREAM_RECORD, m_device_name.is_empty() ? nullptr : m_device_name.characters(), "AudioInput", &spec, nullptr, nullptr, &error);
        if (!m_stream)
            return Error::from_string_view(pa_strerror(error));

        m_thread = Threading::Thread::construct("PulseAudioInput"sv, [this] {
            capture_loop();
            return 0;
        });
        m_thread->start();
        return {};
    }

    void capture_loop()
    {
        size_t channels = m_descriptor.format.channel_count;
        size_t frames_per_read = 256;
        size_t samples_per_read = frames_per_read * channels;
        Vector<float> buffer;
        buffer.resize(samples_per_read);

        while (!m_should_stop.load(AK::MemoryOrder::memory_order_acquire)) {
            int error = 0;
            if (pa_simple_read(m_stream, buffer.data(), buffer.size() * sizeof(float), &error) < 0)
                break;

            ReadonlySpan<float> interleaved { buffer.data(), buffer.size() };
            (void)ring_stream_try_push_interleaved(m_view, interleaved, channels, m_overflow_policy);
        }
    }

    ByteString m_device_name;
    pa_simple* m_stream { nullptr };
    RefPtr<Threading::Thread> m_thread;
    Atomic<bool> m_should_stop { false };
};
#endif

static Atomic<u64> s_next_stream_id { 1 };
static HashMap<AudioInputStreamID, NonnullRefPtr<AudioInputStream>> s_streams;

static Optional<AudioInputDeviceInfo> find_device_info(AudioInputDeviceID device_id)
{
    auto devices = AudioInputDeviceManager::enumerate_devices();
    for (auto const& device : devices) {
        if (device.device_id == device_id)
            return device;
    }
    return {};
}

ErrorOr<AudioInputStreamDescriptor> AudioInputStreamManager::create_stream(AudioInputDeviceID device_id, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames, StreamOverflowPolicy overflow_policy)
{
#if !defined(AK_OS_MACOS) && !defined(HAVE_PULSEAUDIO)
    (void)overflow_policy;
#endif
    if (device_id == 0)
        return Error::from_string_literal("invalid device id");

    auto device_info = find_device_info(device_id);
    if (device_info.has_value()) {
        if (sample_rate_hz == 0)
            sample_rate_hz = device_info->sample_rate_hz;
        if (channel_count == 0)
            channel_count = device_info->channel_count;
    }

    if (sample_rate_hz == 0)
        sample_rate_hz = 48000;
    if (channel_count == 0)
        channel_count = 1;

    if (capacity_frames == 0)
        capacity_frames = 4096;

    RefPtr<AudioInputStream> stream;

#if defined(AK_OS_MACOS)
    stream = TRY(CoreAudioInputStream::create(device_id, sample_rate_hz, channel_count, capacity_frames, overflow_policy));
#elif defined(HAVE_PULSEAUDIO)
    ByteString device_name;
    if (device_info.has_value())
        device_name = device_info->persistent_id;
    stream = TRY(PulseAudioInputStream::create(device_name, sample_rate_hz, channel_count, capacity_frames, overflow_policy));
#else
    return Error::from_string_literal("audio input capture backend not available");
#endif

    auto stream_id = s_next_stream_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
    stream->set_stream_id(stream_id);

    s_streams.set(stream_id, stream.release_nonnull());

    auto descriptor_or_error = s_streams.get(stream_id).value()->descriptor_for_ipc();
    if (descriptor_or_error.is_error())
        return descriptor_or_error.release_error();
    return descriptor_or_error.release_value();
}

void AudioInputStreamManager::destroy_stream(AudioInputStreamID stream_id)
{
    s_streams.remove(stream_id);
}

}
