/*
 * Copyright (c) 2023, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PulseAudioWrappers.h"

#include <AK/WeakPtr.h>
#include <LibThreading/Mutex.h>

namespace Audio {

WeakPtr<PulseAudioContext> PulseAudioContext::weak_instance()
{
    // Use a weak pointer to allow the context to be shut down if we stop outputting audio.
    static WeakPtr<PulseAudioContext> the_instance;
    return the_instance;
}

ErrorOr<NonnullRefPtr<PulseAudioContext>> PulseAudioContext::instance()
{
    static Threading::Mutex instantiation_mutex;
    // Lock and unlock the mutex to ensure that the mutex is fully unlocked at application
    // exit.
    atexit([]() {
        instantiation_mutex.lock();
        instantiation_mutex.unlock();
    });

    auto instantiation_locker = Threading::MutexLocker(instantiation_mutex);

    auto the_instance = weak_instance();
    RefPtr<PulseAudioContext> strong_instance_pointer = the_instance.strong_ref();

    if (strong_instance_pointer == nullptr) {
        auto* main_loop = pa_threaded_mainloop_new();
        if (main_loop == nullptr)
            return Error::from_string_literal("Failed to create PulseAudio main loop");

        auto* api = pa_threaded_mainloop_get_api(main_loop);
        if (api == nullptr)
            return Error::from_string_literal("Failed to get PulseAudio API");

        auto* context = pa_context_new(api, "Ladybird");
        if (context == nullptr)
            return Error::from_string_literal("Failed to get PulseAudio connection context");

        strong_instance_pointer = make_ref_counted<PulseAudioContext>(main_loop, api, context);

        // Set a callback to signal ourselves to wake when the state changes, so that we can
        // synchronously wait for the connection.
        pa_context_set_state_callback(
            context, [](pa_context*, void* user_data) {
                static_cast<PulseAudioContext*>(user_data)->signal_to_wake();
            },
            strong_instance_pointer.ptr());

        if (auto error = pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr); error < 0) {
            warnln("Starting PulseAudio context connection failed with error: {}", pulse_audio_error_to_string(static_cast<PulseAudioErrorCode>(-error)));
            return Error::from_string_literal("Error while starting PulseAudio daemon connection");
        }

        if (auto error = pa_threaded_mainloop_start(main_loop); error < 0) {
            warnln("Starting PulseAudio main loop failed with error: {}", pulse_audio_error_to_string(static_cast<PulseAudioErrorCode>(-error)));
            return Error::from_string_literal("Failed to start PulseAudio main loop");
        }

        {
            auto locker = strong_instance_pointer->main_loop_locker();
            while (true) {
                bool is_ready = false;
                switch (strong_instance_pointer->get_connection_state()) {
                case PulseAudioContextState::Connecting:
                case PulseAudioContextState::Authorizing:
                case PulseAudioContextState::SettingName:
                    break;
                case PulseAudioContextState::Ready:
                    is_ready = true;
                    break;
                case PulseAudioContextState::Failed:
                    warnln("PulseAudio server connection failed with error: {}", pulse_audio_error_to_string(strong_instance_pointer->get_last_error()));
                    return Error::from_string_literal("Failed to connect to PulseAudio server");
                case PulseAudioContextState::Unconnected:
                case PulseAudioContextState::Terminated:
                    VERIFY_NOT_REACHED();
                    break;
                }

                if (is_ready)
                    break;

                strong_instance_pointer->wait_for_signal();
            }

            pa_context_set_state_callback(context, nullptr, nullptr);
        }

        the_instance = strong_instance_pointer;
    }

    return strong_instance_pointer.release_nonnull();
}

PulseAudioContext::PulseAudioContext(pa_threaded_mainloop* main_loop, pa_mainloop_api* api, pa_context* context)
    : m_main_loop(main_loop)
    , m_api(api)
    , m_context(context)
{
}

PulseAudioContext::~PulseAudioContext()
{
    {
        auto locker = main_loop_locker();
        pa_context_disconnect(m_context);
        pa_context_unref(m_context);
    }
    pa_threaded_mainloop_stop(m_main_loop);
    pa_threaded_mainloop_free(m_main_loop);
}

bool PulseAudioContext::current_thread_is_main_loop_thread()
{
    return static_cast<bool>(pa_threaded_mainloop_in_thread(m_main_loop));
}

void PulseAudioContext::lock_main_loop()
{
    if (!current_thread_is_main_loop_thread())
        pa_threaded_mainloop_lock(m_main_loop);
}

void PulseAudioContext::unlock_main_loop()
{
    if (!current_thread_is_main_loop_thread())
        pa_threaded_mainloop_unlock(m_main_loop);
}

void PulseAudioContext::wait_for_signal()
{
    pa_threaded_mainloop_wait(m_main_loop);
}

void PulseAudioContext::signal_to_wake()
{
    pa_threaded_mainloop_signal(m_main_loop, 0);
}

PulseAudioContextState PulseAudioContext::get_connection_state()
{
    return static_cast<PulseAudioContextState>(pa_context_get_state(m_context));
}

bool PulseAudioContext::connection_is_good()
{
    return PA_CONTEXT_IS_GOOD(pa_context_get_state(m_context));
}

PulseAudioErrorCode PulseAudioContext::get_last_error()
{
    return static_cast<PulseAudioErrorCode>(pa_context_errno(m_context));
}

#define STREAM_SIGNAL_CALLBACK(stream)                                          \
    [](auto*, int, void* user_data) {                                           \
        static_cast<PulseAudioStream*>(user_data)->m_context->signal_to_wake(); \
    },                                                                          \
        (stream)

ErrorOr<NonnullRefPtr<PulseAudioStream>> PulseAudioContext::create_stream(OutputState initial_state, u32 sample_rate, u8 channels, u32 target_latency_ms, PulseAudioDataRequestCallback write_callback)
{
    auto locker = main_loop_locker();

    VERIFY(get_connection_state() == PulseAudioContextState::Ready);
    pa_sample_spec sample_specification {
        // FIXME: Support more audio sample types.
        __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ ? PA_SAMPLE_FLOAT32LE : PA_SAMPLE_FLOAT32BE,
        sample_rate,
        channels,
    };

    // Check the sample specification and channel map here. These are also checked by stream_new(),
    // but we can return a more accurate error if we check beforehand.
    if (pa_sample_spec_valid(&sample_specification) == 0)
        return Error::from_string_literal("PulseAudio sample specification is invalid");
    pa_channel_map channel_map;
    if (pa_channel_map_init_auto(&channel_map, sample_specification.channels, PA_CHANNEL_MAP_DEFAULT) == 0) {
        warnln("Getting default PulseAudio channel map failed with error: {}", pulse_audio_error_to_string(get_last_error()));
        return Error::from_string_literal("Failed to get default PulseAudio channel map");
    }

    // Create the stream object and set a callback to signal ourselves to wake when the stream changes states,
    // allowing us to wait synchronously for it to become Ready or Failed.
    auto* stream = pa_stream_new_with_proplist(m_context, "Audio Stream", &sample_specification, &channel_map, nullptr);
    if (stream == nullptr) {
        warnln("Instantiating PulseAudio stream failed with error: {}", pulse_audio_error_to_string(get_last_error()));
        return Error::from_string_literal("Failed to create PulseAudio stream");
    }
    pa_stream_set_state_callback(
        stream, [](pa_stream*, void* user_data) {
            static_cast<PulseAudioContext*>(user_data)->signal_to_wake();
        },
        this);

    auto stream_wrapper = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) PulseAudioStream(NonnullRefPtr(*this), stream)));

    stream_wrapper->m_write_callback = move(write_callback);
    pa_stream_set_write_callback(
        stream, [](pa_stream* stream, size_t bytes_to_write, void* user_data) {
            auto& stream_wrapper = *static_cast<PulseAudioStream*>(user_data);
            VERIFY(stream_wrapper.m_stream == stream);
            stream_wrapper.on_write_requested(bytes_to_write);
        },
        stream_wrapper.ptr());

    // Borrowing logic from cubeb to set reasonable buffer sizes for a target latency:
    // https://searchfox.org/mozilla-central/rev/3b707c8fd7e978eebf24279ee51ccf07895cfbcb/third_party/rust/cubeb-sys/libcubeb/src/cubeb_pulse.c#910-927
    pa_buffer_attr buffer_attributes;
    buffer_attributes.maxlength = -1;
    buffer_attributes.prebuf = -1;
    buffer_attributes.tlength = target_latency_ms * sample_rate / 1000;
    buffer_attributes.minreq = buffer_attributes.tlength / 4;
    buffer_attributes.fragsize = buffer_attributes.minreq;
    auto flags = static_cast<pa_stream_flags>(PA_STREAM_AUTO_TIMING_UPDATE | PA_STREAM_INTERPOLATE_TIMING | PA_STREAM_ADJUST_LATENCY | PA_STREAM_RELATIVE_VOLUME);

    if (initial_state == OutputState::Suspended) {
        stream_wrapper->m_suspended = true;
        flags = static_cast<pa_stream_flags>(static_cast<u32>(flags) | PA_STREAM_START_CORKED);
    }

    // This is a workaround for an issue with starting the stream corked, see PulseAudioPlaybackStream::total_time_played().
    pa_stream_set_started_callback(
        stream, [](pa_stream* stream, void* user_data) {
            static_cast<PulseAudioStream*>(user_data)->m_started_playback = true;
            pa_stream_set_started_callback(stream, nullptr, nullptr);
        },
        stream_wrapper.ptr());

    pa_stream_set_underflow_callback(
        stream, [](pa_stream*, void* user_data) {
            auto& stream = *static_cast<PulseAudioStream*>(user_data);
            if (stream.m_underrun_callback)
                stream.m_underrun_callback();
        },
        stream_wrapper.ptr());

    if (auto error = pa_stream_connect_playback(stream, nullptr, &buffer_attributes, flags, nullptr, nullptr); error != 0) {
        warnln("Failed to start PulseAudio stream connection with error: {}", pulse_audio_error_to_string(static_cast<PulseAudioErrorCode>(error)));
        return Error::from_string_literal("Error while connecting the PulseAudio stream");
    }

    while (true) {
        bool is_ready = false;
        switch (stream_wrapper->get_connection_state()) {
        case PulseAudioStreamState::Creating:
            break;
        case PulseAudioStreamState::Ready:
            is_ready = true;
            break;
        case PulseAudioStreamState::Failed:
            warnln("PulseAudio stream connection failed with error: {}", pulse_audio_error_to_string(get_last_error()));
            return Error::from_string_literal("Failed to connect to PulseAudio daemon");
        case PulseAudioStreamState::Unconnected:
        case PulseAudioStreamState::Terminated:
            VERIFY_NOT_REACHED();
            break;
        }
        if (is_ready)
            break;

        wait_for_signal();
    }

    pa_stream_set_state_callback(stream, nullptr, nullptr);

    return stream_wrapper;
}

PulseAudioStream::~PulseAudioStream()
{
    auto locker = m_context->main_loop_locker();
    pa_stream_set_write_callback(m_stream, nullptr, nullptr);
    pa_stream_set_underflow_callback(m_stream, nullptr, nullptr);
    pa_stream_set_started_callback(m_stream, nullptr, nullptr);
    pa_stream_disconnect(m_stream);
    pa_stream_unref(m_stream);
}

PulseAudioStreamState PulseAudioStream::get_connection_state()
{
    return static_cast<PulseAudioStreamState>(pa_stream_get_state(m_stream));
}

bool PulseAudioStream::connection_is_good()
{
    return PA_STREAM_IS_GOOD(pa_stream_get_state(m_stream));
}

void PulseAudioStream::set_underrun_callback(Function<void()> callback)
{
    auto locker = m_context->main_loop_locker();
    m_underrun_callback = move(callback);
}

u32 PulseAudioStream::sample_rate()
{
    return pa_stream_get_sample_spec(m_stream)->rate;
}

size_t PulseAudioStream::sample_size()
{
    return pa_sample_size(pa_stream_get_sample_spec(m_stream));
}

size_t PulseAudioStream::frame_size()
{
    return pa_frame_size(pa_stream_get_sample_spec(m_stream));
}

u8 PulseAudioStream::channel_count()
{
    return pa_stream_get_sample_spec(m_stream)->channels;
}

void PulseAudioStream::on_write_requested(size_t bytes_to_write)
{
    VERIFY(m_write_callback);
    if (m_suspended)
        return;
    while (bytes_to_write > 0) {
        auto buffer = begin_write(bytes_to_write).release_value_but_fixme_should_propagate_errors();
        auto frame_size = this->frame_size();
        VERIFY(buffer.size() % frame_size == 0);
        auto written_buffer = m_write_callback(*this, buffer, buffer.size() / frame_size);
        if (written_buffer.size() == 0) {
            cancel_write().release_value_but_fixme_should_propagate_errors();
            break;
        }
        bytes_to_write -= written_buffer.size();
        write(written_buffer).release_value_but_fixme_should_propagate_errors();
    }
}

ErrorOr<Bytes> PulseAudioStream::begin_write(size_t bytes_to_write)
{
    void* data_pointer;
    size_t data_size = bytes_to_write;
    if (pa_stream_begin_write(m_stream, &data_pointer, &data_size) != 0 || data_pointer == nullptr)
        return Error::from_string_literal("Failed to get the playback stream's write buffer from PulseAudio");
    return Bytes { data_pointer, data_size };
}

ErrorOr<void> PulseAudioStream::write(ReadonlyBytes data)
{
    if (pa_stream_write(m_stream, data.data(), data.size(), nullptr, 0, PA_SEEK_RELATIVE) != 0)
        return Error::from_string_literal("Failed to write data to PulseAudio playback stream");
    return {};
}

ErrorOr<void> PulseAudioStream::cancel_write()
{
    if (pa_stream_cancel_write(m_stream) != 0)
        return Error::from_string_literal("Failed to get the playback stream's write buffer from PulseAudio");
    return {};
}

bool PulseAudioStream::is_suspended() const
{
    return m_suspended;
}

StringView pulse_audio_error_to_string(PulseAudioErrorCode code)
{
    if (code < PulseAudioErrorCode::OK || code >= PulseAudioErrorCode::Sentinel)
        return "Unknown error code"sv;

    char const* string = pa_strerror(static_cast<int>(code));
    return StringView { string, strlen(string) };
}

ErrorOr<void> PulseAudioStream::wait_for_operation(pa_operation* operation, StringView error_message)
{
    while (pa_operation_get_state(operation) == PA_OPERATION_RUNNING)
        m_context->wait_for_signal();
    if (!m_context->connection_is_good() || !this->connection_is_good()) {
        auto pulse_audio_error_name = pulse_audio_error_to_string(m_context->get_last_error());
        warnln("Encountered stream error: {}", pulse_audio_error_name);
        return Error::from_string_view(error_message);
    }
    pa_operation_unref(operation);
    return {};
}

ErrorOr<void> PulseAudioStream::drain_and_suspend()
{
    auto locker = m_context->main_loop_locker();

    if (m_suspended)
        return {};
    m_suspended = true;

    if (pa_stream_is_corked(m_stream) > 0)
        return {};

    TRY(wait_for_operation(pa_stream_drain(m_stream, STREAM_SIGNAL_CALLBACK(this)), "Draining PulseAudio stream failed"sv));
    TRY(wait_for_operation(pa_stream_cork(m_stream, 1, STREAM_SIGNAL_CALLBACK(this)), "Corking PulseAudio stream after drain failed"sv));
    return {};
}

ErrorOr<void> PulseAudioStream::flush_and_suspend()
{
    auto locker = m_context->main_loop_locker();

    if (m_suspended)
        return {};
    m_suspended = true;

    if (pa_stream_is_corked(m_stream) > 0)
        return {};

    TRY(wait_for_operation(pa_stream_flush(m_stream, STREAM_SIGNAL_CALLBACK(this)), "Flushing PulseAudio stream failed"sv));
    TRY(wait_for_operation(pa_stream_cork(m_stream, 1, STREAM_SIGNAL_CALLBACK(this)), "Corking PulseAudio stream after flush failed"sv));
    return {};
}

ErrorOr<void> PulseAudioStream::resume()
{
    auto locker = m_context->main_loop_locker();

    if (!m_suspended)
        return {};
    m_suspended = false;

    TRY(wait_for_operation(pa_stream_cork(m_stream, 0, STREAM_SIGNAL_CALLBACK(this)), "Uncorking PulseAudio stream failed"sv));

    // Defer a write to the playback buffer on the PulseAudio main loop. Otherwise, playback will not
    // begin again, despite the fact that we uncorked.
    // NOTE: We ref here and then unref in the callback so that this stream will not be deleted until
    //       it finishes.
    ref();
    pa_mainloop_api_once(
        m_context->m_api, [](pa_mainloop_api*, void* user_data) {
            auto& stream = *static_cast<PulseAudioStream*>(user_data);
            // NOTE: writable_size() returns -1 in case of an error. However, the value is still safe
            //       since begin_write() will interpret -1 as a default parameter and choose a good size.
            auto bytes_to_write = pa_stream_writable_size(stream.m_stream);
            stream.on_write_requested(bytes_to_write);
            stream.unref();
        },
        this);
    return {};
}

ErrorOr<AK::Duration> PulseAudioStream::total_time_played()
{
    auto locker = m_context->main_loop_locker();

    // NOTE: This is a workaround for a PulseAudio issue. When a stream is started corked,
    //       the time smoother doesn't seem to be aware of it, so it will return the time
    //       since the stream was connected. Once the playback actually starts, the time
    //       resets back to zero. However, since we request monotonically-increasing time,
    //       this means that the smoother will register that it had a larger time before,
    //       and return that time instead, until we reach a timestamp greater than the
    //       last-returned time. If we never call pa_stream_get_time() until after giving
    //       the stream its first samples, the issue never occurs.
    if (!m_started_playback)
        return AK::Duration::zero();

    pa_usec_t time = 0;
    auto error = pa_stream_get_time(m_stream, &time);
    if (error == -PA_ERR_NODATA)
        return AK::Duration::zero();
    if (error != 0)
        return Error::from_string_literal("Failed to get time from PulseAudio stream");
    if (time > NumericLimits<i64>::max()) {
        warnln("WARNING: Audio time is too large!");
        time -= NumericLimits<i64>::max();
    }
    return AK::Duration::from_microseconds(static_cast<i64>(time));
}

ErrorOr<void> PulseAudioStream::set_volume(double volume)
{
    auto locker = m_context->main_loop_locker();

    auto index = pa_stream_get_index(m_stream);
    if (index == PA_INVALID_INDEX)
        return Error::from_string_literal("Failed to get PulseAudio stream index while setting volume");

    auto pulse_volume = pa_sw_volume_from_linear(volume);
    pa_cvolume per_channel_volumes;
    pa_cvolume_set(&per_channel_volumes, channel_count(), pulse_volume);

    auto* operation = pa_context_set_sink_input_volume(m_context->m_context, index, &per_channel_volumes, STREAM_SIGNAL_CALLBACK(this));
    return wait_for_operation(operation, "Failed to set PulseAudio stream volume"sv);
}

}
