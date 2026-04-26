/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibCore/EventLoop.h>
#include <LibMedia/Audio/Forward.h>
#include <LibMedia/Audio/SampleSpecification.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/Providers/AudioDataProvider.h>
#include <LibMedia/Sinks/AudioSink.h>
#include <LibMedia/Track.h>
#include <LibSync/Mutex.h>

namespace Media {

class MEDIA_API AudioMixer final : public AudioSink {
public:
    static ErrorOr<NonnullRefPtr<AudioMixer>> try_create();
    AudioMixer();
    virtual ~AudioMixer() override = default;

    virtual void set_provider(Track const&, RefPtr<AudioDataProvider> const&) override;
    virtual RefPtr<AudioDataProvider> provider(Track const&) const override;

    void set_sample_specification(Audio::SampleSpecification);
    Audio::SampleSpecification sample_specification() const;

    bool mix_one_block_into(AudioBlock& out_block);

    void reset_to_sample_position(i64 sample_position);

    Function<void(Track const&)> on_start_buffering;

private:
    struct TrackMixingData {
        TrackMixingData(NonnullRefPtr<AudioDataProvider> const& provider)
            : provider(provider)
        {
        }

        NonnullRefPtr<AudioDataProvider> provider;
        AudioBlock current_block;
        bool buffering { false };
    };

    Core::EventLoop& m_main_thread_event_loop;

    mutable Sync::Mutex m_mutex;
    Audio::SampleSpecification m_sample_specification;
    HashMap<Track, TrackMixingData> m_track_mixing_datas;
    i64 m_next_sample_to_write { 0 };
};

}
