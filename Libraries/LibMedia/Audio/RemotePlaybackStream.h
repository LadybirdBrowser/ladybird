/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibMedia/Audio/PlaybackStream.h>
#include <LibMedia/Audio/SharedAudioFrameRing.h>
#include <LibMedia/Export.h>

namespace Audio {

class ClientConnection;

// A PlaybackStream whose output device lives in the AudioServer: frames through a shared ring, control over IPC.
class MEDIA_API RemotePlaybackStream final : public PlaybackStream {
public:
    // Control thread, once the server has reported the device format for this stream.
    RemotePlaybackStream(ClientConnection&, u64 stream_id, SampleSpecification, u32 ring_latency_ms, OutputState, AudioDataRequestCallback);
    virtual ~RemotePlaybackStream() override;

    virtual SampleSpecification sample_specification() const override { return m_sample_specification; }
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> resume() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> drain_buffer_and_suspend() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> discard_buffer_and_suspend() override;
    virtual void notify_data_available() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> set_volume(double volume) override;

    u64 stream_id() const { return m_stream_id; }

    // Pump thread. Tops the ring up to the target latency; stops asking after an empty return until notified.
    void fill(Vector<float>& scratch);

    // Control thread. The server has played this stream's ring out, or abandoned the drain for a resume or discard.
    void drain_completed();
    // Control thread. The connection died: tell the owner.
    void connection_lost();

private:
    enum class State : u8 {
        Suspended,
        Playing,
        Draining,
        Lost,
    };

    void run_on_control_thread(Function<void()>);
    // With m_mutex held.
    void ensure_ring_locked();
    void start_playing_locked();

    // Only ever referenced and released on the control thread; see the destructor.
    NonnullRefPtr<ClientConnection> m_connection;
    Core::EventLoop& m_control_loop;
    u64 const m_stream_id { 0 };
    SampleSpecification const m_sample_specification;
    size_t const m_fill_target_frames { 0 };
    AudioDataRequestCallback m_callback;

    mutable Mutex m_mutex;
    Optional<SharedAudioFrameRing> m_ring;
    State m_state { State::Suspended };
    bool m_waiting_for_data { false };
    // Counts notify_data_available() calls, so a notification that lands while the callback is running is not lost
    // when the callback then comes back empty.
    u64 m_data_notification_count { 0 };
};

}
