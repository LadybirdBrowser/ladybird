/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/NumericLimits.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/Audio/Forward.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/Providers/MediaTimeProvider.h>
#include <LibMedia/Sinks/AudioSink.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>

namespace AudioServer {

class SessionClientOfAudioServer;
class SingleSinkSessionClient;

}

namespace Media {

class MEDIA_API AudioMixingSink : public AudioSink
    , public Weakable<AudioMixingSink> {
protected:
    struct AudioServerOutput;

public:
    static ErrorOr<NonnullRefPtr<AudioMixingSink>> try_create();
    AudioMixingSink(NonnullRefPtr<AudioServer::SingleSinkSessionClient>);
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

    Audio::SampleSpecification device_sample_specification() const;
    Audio::SampleSpecification current_sample_specification() const;

protected:
    struct TrackMixingData {
        explicit TrackMixingData(NonnullRefPtr<AudioDataProvider> const& provider);

        NonnullRefPtr<AudioDataProvider> provider;
        AudioBlock current_block;
    };

    intptr_t sink_thread_main();
    ReadonlySpan<float> write_audio_data_to_playback_stream(Span<float>, bool* did_mix_audio = nullptr, bool advance_cursor = true);
    void apply_sample_specification(Audio::SampleSpecification);
    void process_control_commands();
    void apply_time_change(AK::Duration);
    void request_output_sink_if_needed();
    void wake_sink_thread();
    void will_be_destroyed();

    Core::EventLoop& m_main_thread_event_loop;
    OwnPtr<AudioServerOutput> m_audio_server_output;
    mutable Threading::Mutex m_track_mutex;
    mutable Threading::Mutex m_sample_spec_mutex;

    Audio::SampleSpecification m_sample_specification;
    Audio::SampleSpecification m_device_sample_specification;

    Atomic<bool, MemoryOrder::memory_order_relaxed> m_playing { false };
    Atomic<double, MemoryOrder::memory_order_relaxed> m_volume { 1.0 };
    Atomic<i64, MemoryOrder::memory_order_relaxed> m_set_seek_ms { NumericLimits<i64>::min() };

    HashMap<Track, TrackMixingData> m_track_mixing_datas;
    Atomic<i64, MemoryOrder::memory_order_relaxed> m_last_stream_time { 0 };
    Atomic<i64, MemoryOrder::memory_order_relaxed> m_last_media_time { 0 };
    Atomic<i64, MemoryOrder::memory_order_relaxed> m_temporary_time { NumericLimits<i64>::min() };
    Atomic<u64, MemoryOrder::memory_order_relaxed> m_output_device_played_frames { NumericLimits<u64>::max() };
    Atomic<i64, MemoryOrder::memory_order_relaxed> m_next_sample_to_write { 0 };
    Atomic<i64, MemoryOrder::memory_order_relaxed> m_current_session_start_sample { 0 };
};

}
