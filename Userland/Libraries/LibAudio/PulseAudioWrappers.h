/*
 * Copyright (c) 2023, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Time.h>
#include <LibAudio/Forward.h>
#include <LibAudio/PlaybackStream.h>
#include <LibAudio/SampleFormats.h>
#include <LibThreading/Thread.h>
#include <pulse/pulseaudio.h>

namespace Audio {

class PulseAudioStream;

enum class PulseAudioContextState {
    Unconnected = PA_CONTEXT_UNCONNECTED,
    Connecting = PA_CONTEXT_CONNECTING,
    Authorizing = PA_CONTEXT_AUTHORIZING,
    SettingName = PA_CONTEXT_SETTING_NAME,
    Ready = PA_CONTEXT_READY,
    Failed = PA_CONTEXT_FAILED,
    Terminated = PA_CONTEXT_TERMINATED,
};

enum class PulseAudioErrorCode;

using PulseAudioDataRequestCallback = Function<ReadonlyBytes(PulseAudioStream&, Bytes buffer, size_t sample_count)>;

// A wrapper around the PulseAudio main loop and context structs.
// Generally, only one instance of this should be needed for a single process.
class PulseAudioContext
    : public AtomicRefCounted<PulseAudioContext>
    , public Weakable<PulseAudioContext> {
public:
    static AK::WeakPtr<PulseAudioContext> weak_instance();
    static ErrorOr<NonnullRefPtr<PulseAudioContext>> instance();

    explicit PulseAudioContext(pa_threaded_mainloop*, pa_mainloop_api*, pa_context*);
    PulseAudioContext(PulseAudioContext const& other) = delete;
    ~PulseAudioContext();

    bool current_thread_is_main_loop_thread();
    void lock_main_loop();
    void unlock_main_loop();
    [[nodiscard]] auto main_loop_locker()
    {
        lock_main_loop();
        return ScopeGuard([this]() { unlock_main_loop(); });
    }
    // Waits for signal_to_wake() to be called.
    // This must be called with the main loop locked.
    void wait_for_signal();
    // Signals to wake all threads from calls to signal_to_wake()
    void signal_to_wake();

    PulseAudioContextState get_connection_state();
    bool connection_is_good();
    PulseAudioErrorCode get_last_error();

    ErrorOr<NonnullRefPtr<PulseAudioStream>> create_stream(OutputState initial_state, u32 sample_rate, u8 channels, u32 target_latency_ms, PulseAudioDataRequestCallback write_callback);

private:
    friend class PulseAudioStream;

    pa_threaded_mainloop* m_main_loop { nullptr };
    pa_mainloop_api* m_api { nullptr };
    pa_context* m_context;
};

enum class PulseAudioStreamState {
    Unconnected = PA_STREAM_UNCONNECTED,
    Creating = PA_STREAM_CREATING,
    Ready = PA_STREAM_READY,
    Failed = PA_STREAM_FAILED,
    Terminated = PA_STREAM_TERMINATED,
};

class PulseAudioStream : public AtomicRefCounted<PulseAudioStream> {
public:
    static constexpr bool start_corked = true;

    ~PulseAudioStream();

    PulseAudioStreamState get_connection_state();
    bool connection_is_good();

    // Sets the callback to be run when the server consumes more of the buffer than
    // has been written yet.
    void set_underrun_callback(Function<void()>);

    u32 sample_rate();
    size_t sample_size();
    size_t frame_size();
    u8 channel_count();
    // Gets a data buffer that can be written to and then passed back to PulseAudio through
    // the write() function. This avoids a copy vs directly calling write().
    ErrorOr<Bytes> begin_write(size_t bytes_to_write = NumericLimits<size_t>::max());
    // Writes a data buffer to the playback stream.
    ErrorOr<void> write(ReadonlyBytes data);
    // Cancels the previous begin_write() call.
    ErrorOr<void> cancel_write();

    bool is_suspended() const;
    // Plays back all buffered data and corks the stream. Until resume() is called, no data
    // will be written to the stream.
    ErrorOr<void> drain_and_suspend();
    // Drops all buffered data and corks the stream. Until resume() is called, no data will
    // be written to the stream.
    ErrorOr<void> flush_and_suspend();
    // Uncorks the stream and forces data to be written to the buffers to force playback to
    // resume as soon as possible.
    ErrorOr<void> resume();
    ErrorOr<AK::Duration> total_time_played();

    ErrorOr<void> set_volume(double volume);

    PulseAudioContext& context() { return *m_context; }

private:
    friend class PulseAudioContext;

    explicit PulseAudioStream(NonnullRefPtr<PulseAudioContext>&& context, pa_stream* stream)
        : m_context(context)
        , m_stream(stream)
    {
    }
    PulseAudioStream(PulseAudioStream const& other) = delete;

    ErrorOr<void> wait_for_operation(pa_operation*, StringView error_message);

    void on_write_requested(size_t bytes_to_write);

    NonnullRefPtr<PulseAudioContext> m_context;
    pa_stream* m_stream { nullptr };
    bool m_started_playback { false };
    PulseAudioDataRequestCallback m_write_callback { nullptr };
    // Determines whether we will allow the write callback to run. This should only be true
    // if the stream is becoming or is already corked.
    bool m_suspended { false };

    Function<void()> m_underrun_callback;
};

enum class PulseAudioErrorCode {
    OK = 0,
    AccessFailure,
    UnknownCommand,
    InvalidArgument,
    EntityExists,
    NoSuchEntity,
    ConnectionRefused,
    ProtocolError,
    Timeout,
    NoAuthenticationKey,
    InternalError,
    ConnectionTerminated,
    EntityKilled,
    InvalidServer,
    NoduleInitFailed,
    BadState,
    NoData,
    IncompatibleProtocolVersion,
    DataTooLarge,
    NotSupported,
    Unknown,
    NoExtension,
    Obsolete,
    NotImplemented,
    CalledFromFork,
    IOError,
    Busy,
    Sentinel
};

StringView pulse_audio_error_to_string(PulseAudioErrorCode code);

}
