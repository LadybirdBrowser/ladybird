/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2025, Tim Flynn <trflynn89@ladybird.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/ScopeGuard.h>
#include <AudioServer/InputStream.h>
#include <AudioServer/Server.h>
#include <pulse/pulseaudio.h>

namespace AudioServer {

static ByteString resolve_pulse_source_name(DeviceHandle device_handle)
{
    if (device_handle == 0)
        return {};

    ByteString source_name;

    pa_mainloop* mainloop = pa_mainloop_new();
    if (!mainloop)
        return {};

    auto* api = pa_mainloop_get_api(mainloop);
    pa_context* context = pa_context_new(api, "Ladybird AudioServer");
    if (!context) {
        pa_mainloop_free(mainloop);
        return {};
    }

    if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
        pa_context_unref(context);
        pa_mainloop_free(mainloop);
        return {};
    }

    while (true) {
        pa_mainloop_iterate(mainloop, 1, nullptr);
        auto state = pa_context_get_state(context);
        if (state == PA_CONTEXT_READY)
            break;
        if (!PA_CONTEXT_IS_GOOD(state)) {
            pa_context_disconnect(context);
            pa_context_unref(context);
            pa_mainloop_free(mainloop);
            return {};
        }
    }

    auto* operation = pa_context_get_source_info_by_index(
        context,
        static_cast<u32>(device_handle),
        [](pa_context*, pa_source_info const* info, int eol, void* userdata) {
            if (eol != 0 || !info || !info->name)
                return;
            auto& result = *static_cast<ByteString*>(userdata);
            result = ByteString(info->name);
        },
        &source_name);

    if (operation) {
        while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING)
            pa_mainloop_iterate(mainloop, 1, nullptr);
        pa_operation_unref(operation);
    }

    pa_context_disconnect(context);
    pa_context_unref(context);
    pa_mainloop_free(mainloop);

    return source_name;
}

class PulseAudioInputStream final : public InputStream {
public:
    static ErrorOr<NonnullRefPtr<InputStream>> create(ByteString device_name, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames)
    {
        auto* stream = new (nothrow) PulseAudioInputStream(move(device_name));
        if (!stream)
            return Error::from_errno(ENOMEM);

        ArmedScopeGuard rollback = [stream] { delete stream; };
        TRY(stream->initialize_shared_ring_storage(sample_rate_hz, channel_count, capacity_frames));
        TRY(stream->initialize_stream(sample_rate_hz, channel_count));

        rollback.disarm();
        return TRY(adopt_nonnull_ref_or_enomem(static_cast<InputStream*>(stream)));
    }

    ~PulseAudioInputStream() override
    {
        shutdown();
    }

private:
    explicit PulseAudioInputStream(ByteString device_name)
        : m_device_name(move(device_name))
    {
    }

    ErrorOr<void> initialize_stream(u32 sample_rate_hz, u32 channel_count)
    {
        m_mainloop = pa_threaded_mainloop_new();
        if (m_mainloop == nullptr)
            return Error::from_string_literal("Failed to create PulseAudio main loop");

        pa_mainloop_api* api = pa_threaded_mainloop_get_api(m_mainloop);
        if (api == nullptr)
            return Error::from_string_literal("Failed to get PulseAudio API");

        m_context = pa_context_new(api, "Ladybird AudioServer Input");
        if (m_context == nullptr)
            return Error::from_string_literal("Failed to create PulseAudio context");

        pa_context_set_state_callback(m_context, [](pa_context*, void* user_data) {
            auto& stream = *static_cast<PulseAudioInputStream*>(user_data);
            stream.signal_mainloop(); }, this);

        if (pa_context_connect(m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0)
            return Error::from_string_literal("Error while starting PulseAudio context connection");

        if (pa_threaded_mainloop_start(m_mainloop) < 0)
            return Error::from_string_literal("Failed to start PulseAudio main loop");

        auto mainloop_locker = lock_mainloop();
        while (true) {
            pa_context_state_t state = pa_context_get_state(m_context);
            if (state == PA_CONTEXT_READY)
                break;
            if (!PA_CONTEXT_IS_GOOD(state))
                return Error::from_string_literal("Failed to connect to PulseAudio context");
            pa_threaded_mainloop_wait(m_mainloop);
        }
        pa_context_set_state_callback(m_context, nullptr, nullptr);

        pa_sample_spec spec {};
        spec.format = PA_SAMPLE_FLOAT32LE;
        spec.rate = sample_rate_hz;
        spec.channels = channel_count;

        m_stream = pa_stream_new(m_context, "AudioInput", &spec, nullptr);
        if (!m_stream)
            return Error::from_string_literal("Failed to create PulseAudio input stream");

        pa_stream_set_state_callback(m_stream, [](pa_stream*, void* user_data) {
            auto& stream = *static_cast<PulseAudioInputStream*>(user_data);
            stream.signal_mainloop(); }, this);

        pa_stream_set_read_callback(m_stream, [](pa_stream* stream, size_t bytes, void* user_data) {
            auto& input_stream = *static_cast<PulseAudioInputStream*>(user_data);
            input_stream.read_callback(stream, bytes); }, this);

        char const* source_name = m_device_name.is_empty() ? nullptr : m_device_name.characters();
        if (pa_stream_connect_record(m_stream, source_name, nullptr, PA_STREAM_ADJUST_LATENCY) != 0)
            return Error::from_string_literal("Error while connecting PulseAudio record stream");

        while (true) {
            pa_stream_state_t state = pa_stream_get_state(m_stream);
            if (state == PA_STREAM_READY)
                break;
            if (!PA_STREAM_IS_GOOD(state))
                return Error::from_string_literal("Failed to connect PulseAudio record stream");
            pa_threaded_mainloop_wait(m_mainloop);
        }
        pa_stream_set_state_callback(m_stream, nullptr, nullptr);

        return {};
    }

    void read_callback(pa_stream* stream, size_t)
    {
        if (m_stream != stream)
            return;

        void const* data = nullptr;
        size_t bytes = 0;
        if (pa_stream_peek(stream, &data, &bytes) != 0)
            return;

        if (data && bytes > 0) {
            size_t sample_count = bytes / sizeof(float);
            ReadonlySpan<float> interleaved { reinterpret_cast<float const*>(data), sample_count };
            size_t channels = channel_count();
            (void)try_push_interleaved(interleaved, channels);
        }

        (void)pa_stream_drop(stream);
    }

    void shutdown()
    {
        if (m_mainloop == nullptr)
            return;

        {
            auto mainloop_locker = lock_mainloop();
            if (m_stream != nullptr) {
                pa_stream_set_read_callback(m_stream, nullptr, nullptr);
                pa_stream_set_state_callback(m_stream, nullptr, nullptr);
                pa_stream_disconnect(m_stream);
                pa_stream_unref(m_stream);
                m_stream = nullptr;
            }

            if (m_context != nullptr) {
                pa_context_set_state_callback(m_context, nullptr, nullptr);
                pa_context_disconnect(m_context);
                pa_context_unref(m_context);
                m_context = nullptr;
            }
        }

        pa_threaded_mainloop_stop(m_mainloop);
        pa_threaded_mainloop_free(m_mainloop);
        m_mainloop = nullptr;
    }

    void signal_mainloop() const
    {
        if (m_mainloop != nullptr)
            pa_threaded_mainloop_signal(m_mainloop, 0);
    }

    [[nodiscard]] ScopeGuard<Function<void()>> lock_mainloop() const
    {
        pa_threaded_mainloop_lock(m_mainloop);
        return ScopeGuard<Function<void()>>(Function<void()>([this]() {
            pa_threaded_mainloop_unlock(m_mainloop);
        }));
    }

    ByteString m_device_name;
    pa_threaded_mainloop* m_mainloop { nullptr };
    pa_context* m_context { nullptr };
    pa_stream* m_stream { nullptr };
};

ErrorOr<NonnullRefPtr<InputStream>> create_platform_input_stream(DeviceHandle device_handle, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames)
{
    DeviceHandle raw_handle = static_cast<DeviceHandle>(Server::device_handle_to_os_device_id(device_handle));
    ByteString device_name = resolve_pulse_source_name(raw_handle);
    return PulseAudioInputStream::create(move(device_name), sample_rate_hz, channel_count, capacity_frames);
}

}
