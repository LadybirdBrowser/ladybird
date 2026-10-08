/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibCore/Forward.h>
#include <LibMedia/Audio/PlaybackStream.h>
#include <LibMedia/Audio/SharedAudioFrameRing.h>
#include <LibMedia/Export.h>

namespace Audio {

using MixerClientId = u64;

// Mixes the shared rings of one tab's client streams into one output device stream, which plays only while some
// client is playing or draining.
class MEDIA_API PlaybackStreamMixer : public AtomicRefCounted<PlaybackStreamMixer> {
public:
    using DeviceStreamFactory = Function<NonnullRefPtr<PlaybackStream::CreatePromise>(OutputState, u32 target_latency_ms, PlaybackStream::AudioDataRequestCallback)>;

    // Control thread. The device stream opens suspended through the factory.
    static NonnullRefPtr<PlaybackStreamMixer> create(Core::EventLoop& control_loop, u32 target_latency_ms, DeviceStreamFactory);
    ~PlaybackStreamMixer();

    // Control thread. Runs once the device stream is open or has failed to open; at once if that is already known.
    using DeviceReadyCallback = Function<void(ErrorOr<SampleSpecification> const&)>;
    void when_device_ready(DeviceReadyCallback);
    Optional<SampleSpecification> const& device_sample_specification() const { return m_device_sample_specification; }

    // Control thread. Rings must be in the device format.
    void add_client(MixerClientId, SharedAudioFrameRing, float gain);
    void remove_client(MixerClientId);
    void resume_client(MixerClientId);
    // on_drained runs on the control thread once the ring has played out, or at once if the drain is made moot.
    void drain_client(MixerClientId, Function<void()> on_drained);
    void discard_client(MixerClientId);
    void set_client_gain(MixerClientId, float gain);

    bool has_clients() const;
    size_t active_client_count() const { return m_active_client_count; }

private:
    enum class ClientState : u8 {
        Suspended,
        Playing,
        Draining,
        // The ring ran empty; the client stays active until the device has played what it took.
        Drained,
    };

    struct Client {
        SharedAudioFrameRing ring;
        float gain { 1 };
        ClientState state { ClientState::Suspended };
        Function<void()> on_drained;
        // When the device will have played the last frame consumed from this ring.
        i64 played_at_ns { 0 };
    };

    // Shared with the device callback, which may still be running while the mixer is torn down.
    class MixState : public AtomicRefCounted<MixState> {
    public:
        explicit MixState(Core::EventLoop& control_loop)
            : m_control_loop(control_loop)
        {
        }

        ReadonlySpan<float> mix_into(Span<float> buffer, MonotonicTime buffer_starts_playing_at);

        Core::EventLoop& m_control_loop;
        // Written on the control thread only; the drain completions it posts there check it before use.
        PlaybackStreamMixer* m_mixer { nullptr };

        Mutex m_mutex;
        HashMap<MixerClientId, Client> m_clients;
    };

    explicit PlaybackStreamMixer(NonnullRefPtr<MixState>);

    void set_device_stream(NonnullRefPtr<PlaybackStream>);
    void set_device_error(Error);
    void run_device_ready_callbacks();
    void client_became_active();
    void client_became_inactive();
    // The ring ran empty; the drain is done once the device has played what it took, at `played_at_ns`.
    void finish_drain_once_played(MixerClientId, i64 played_at_ns);
    void finish_drain(MixerClientId);

    NonnullRefPtr<MixState> m_state;
    RefPtr<PlaybackStream> m_device_stream;
    Optional<SampleSpecification> m_device_sample_specification;
    Optional<Error> m_device_error;
    Vector<DeviceReadyCallback> m_device_ready_callbacks;
    size_t m_active_client_count { 0 };
    bool m_device_is_playing { false };
    HashMap<MixerClientId, NonnullRefPtr<Core::Timer>> m_drain_timers;
};

}
