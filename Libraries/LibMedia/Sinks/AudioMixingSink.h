/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/Audio/Forward.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/Providers/MediaTimeProvider.h>
#include <LibMedia/Sinks/AudioSink.h>
#include <LibSync/Mutex.h>

namespace Media {

class MEDIA_API AudioMixingSink final : public AudioSink {
    class AudioMixingSinkWeakReference;

private:
    class OutputThreadData;

public:
    static ErrorOr<NonnullRefPtr<AudioMixingSink>> try_create();
    AudioMixingSink(AudioMixingSinkWeakReference&, NonnullRefPtr<OutputThreadData>);
    virtual ~AudioMixingSink() override;

    virtual void set_provider(Track const&, RefPtr<AudioDataProvider> const&) override;
    virtual RefPtr<AudioDataProvider> provider(Track const&) const override;

    // This section implements the pure virtuals in MediaTimeProvider.
    // AudioMixingSink cannot inherit from MediaTimeProvider, as AudioSink and MediaTimeProvider both inherit from
    // AtomicRefCounted. In order to use AudioMixingSink as a MediaTimeProvider, wrap it with WrapperTimeProvider.
    AK::Duration current_time() const;
    void resume();
    void pause();
    void set_time(AK::Duration);

    void set_volume(double);

    Function<void(Error&&)> on_audio_output_error;
    Function<void(Track const&)> on_start_buffering;

private:
    class AudioMixingSinkWeakReference : public AtomicRefCounted<AudioMixingSinkWeakReference> {
    public:
        void emplace(AudioMixingSink& sink) { m_ptr = &sink; }
        RefPtr<AudioMixingSink> take_strong() const
        {
            Sync::MutexLocker locker { m_mutex };
            return m_ptr;
        }
        void revoke()
        {
            Sync::MutexLocker locker { m_mutex };
            m_ptr = nullptr;
        }

    private:
        mutable Sync::Mutex m_mutex;
        AudioMixingSink* m_ptr { nullptr };
    };

    void create_playback_stream();

    Core::EventLoop& m_main_thread_event_loop;
    NonnullRefPtr<AudioMixingSinkWeakReference> m_weak_self;

    bool m_started_creating_playback_stream { false };
    bool m_playing { false };
    double m_volume { 1 };

    AK::Duration m_last_stream_time;
    AK::Duration m_last_media_time;
    Optional<AK::Duration> m_temporary_time;

    NonnullRefPtr<OutputThreadData> m_output_thread_data;
};

}
