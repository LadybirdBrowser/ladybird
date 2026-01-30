/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Weakable.h>
#include <AK/WeakPtr.h>
#include <AK/SPSCQueue.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/Audio/Forward.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/Providers/MediaTimeProvider.h>
#include <LibMedia/Sinks/AudioSink.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>

namespace AudioServerClient {

class Client;

}

namespace Media {

class MEDIA_API AudioMixingSink : public AudioSink, public Weakable<AudioMixingSink> {
protected:
    struct AudioServerOutput;

public:
    static ErrorOr<NonnullRefPtr<AudioMixingSink>> try_create();
    AudioMixingSink(NonnullRefPtr<AudioServerClient::Client>);
    virtual ~AudioMixingSink() override;

    virtual void set_provider(Track const&, RefPtr<AudioDataProvider> const&) override;
    virtual RefPtr<AudioDataProvider> provider(Track const&) const override;

    // This section implements the pure virtuals in MediaTimeProvider.
    // AudioMixingSink cannot inherit from MediaTimeProvider, as AudioSink and MediaTimeProvider both inherit from
    // AtomicRefCounted. In order to use AudioMixingSink as a MediaTimeProvider, wrap it with WrapperTimeProvider.
    virtual AK::Duration current_time() const;
    virtual void resume();
    virtual void pause();
    virtual void set_time(AK::Duration);
    virtual void clear_track_data(Track const&);

    virtual void set_volume(double);

    using TapCallback = Function<void(ReadonlySpan<float> interleaved_samples, Audio::SampleSpecification const&, AK::Duration start_time)>;
    void set_tap(TapCallback, Optional<Audio::SampleSpecification> override_sample_specification = {});
    void clear_tap();
    bool has_tap() const;

    Audio::SampleSpecification device_sample_specification() const;
    Audio::SampleSpecification current_sample_specification() const;

protected:
    static constexpr size_t MAX_BLOCK_COUNT = 16;

    struct TrackMixingData {
        explicit TrackMixingData(NonnullRefPtr<AudioDataProvider> const& provider);

        NonnullRefPtr<AudioDataProvider> provider;
        AudioBlock current_block;
    };

    ReadonlySpan<float> write_audio_data_to_playback_stream(Span<float>, bool* did_mix_audio = nullptr, bool advance_cursor = true);
    void apply_sample_specification(Audio::SampleSpecification);

    Core::EventLoop& m_main_thread_event_loop;

    OwnPtr<AudioServerOutput> m_audio_server_output;

    struct TapState : public RefCounted<TapState> {
        TapState(TapCallback callback, Audio::SampleSpecification sample_specification)
            : callback(move(callback))
            , sample_specification(sample_specification)
        {
        }

        TapCallback callback;
        Audio::SampleSpecification sample_specification;
    };

    struct ControlCommand {
        enum class Type : u8 {
            SetSampleSpec,
            SetTime,
            SetTap,
            ClearTap,
            SetVolume,
            Resume,
            Pause,
        };

        Type type { Type::SetSampleSpec };
        Audio::SampleSpecification sample_specification;
        AK::Duration time;
        RefPtr<TapState> tap_state;
        double volume { 1.0 };
    };

    static constexpr size_t CONTROL_QUEUE_CAPACITY = 64;

    void enqueue_control_command(ControlCommand const&);
    void process_control_commands();
    void apply_time_change(AK::Duration);

    RefPtr<TapState> m_tap_state;
    Atomic<bool, MemoryOrder::memory_order_relaxed> m_has_tap { false };
    AK::SPSCQueue<ControlCommand, CONTROL_QUEUE_CAPACITY> m_control_queue;

    mutable Threading::Mutex m_mutex;
    Audio::SampleSpecification m_sample_specification;
    Audio::SampleSpecification m_device_sample_specification;
    Atomic<bool, MemoryOrder::memory_order_relaxed> m_playing { false };
    double m_volume { 1 };

    HashMap<Track, TrackMixingData> m_track_mixing_datas;
    Atomic<i64, MemoryOrder::memory_order_relaxed> m_next_sample_to_write { 0 };

    AK::Duration m_last_stream_time;
    AK::Duration m_last_media_time;
    Optional<AK::Duration> m_temporary_time;

    void will_be_destroyed();
};

}
