/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/Audio/Forward.h>
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

class MEDIA_API AudioMixingSink : public AudioSink {
protected:
    class AudioMixingSinkWeakReference;

    struct AudioServerOutput;

public:
    static ErrorOr<NonnullRefPtr<AudioMixingSink>> try_create();
    AudioMixingSink(AudioMixingSinkWeakReference&, NonnullRefPtr<AudioServerClient::Client>);
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

protected:
    static constexpr size_t MAX_BLOCK_COUNT = 16;

    class AudioMixingSinkWeakReference : public AtomicRefCounted<AudioMixingSinkWeakReference> {
    public:
        void emplace(AudioMixingSink& sink) { m_ptr = &sink; }
        RefPtr<AudioMixingSink> take_strong() const
        {
            Threading::MutexLocker locker { m_mutex };
            return m_ptr;
        }
        void revoke()
        {
            Threading::MutexLocker locker { m_mutex };
            m_ptr = nullptr;
        }

    private:
        mutable Threading::Mutex m_mutex;
        AudioMixingSink* m_ptr { nullptr };
    };

    struct TrackMixingData {
        explicit TrackMixingData(NonnullRefPtr<AudioDataProvider> const& provider);

        NonnullRefPtr<AudioDataProvider> provider;
        AudioBlock current_block;
    };

    ReadonlySpan<float> write_audio_data_to_playback_stream(Span<float>, bool* did_mix_audio = nullptr, bool advance_cursor = true);

    Core::EventLoop& m_main_thread_event_loop;
    NonnullRefPtr<AudioMixingSinkWeakReference> m_weak_self;

    OwnPtr<AudioServerOutput> m_audio_server_output;

    Threading::Mutex m_mutex;
    Audio::SampleSpecification m_sample_specification;
    bool m_playing { false };
    double m_volume { 1 };

    HashMap<Track, TrackMixingData> m_track_mixing_datas;
    Atomic<i64, MemoryOrder::memory_order_relaxed> m_next_sample_to_write { 0 };

    AK::Duration m_last_stream_time;
    AK::Duration m_last_media_time;
    Optional<AK::Duration> m_temporary_time;
};

}
