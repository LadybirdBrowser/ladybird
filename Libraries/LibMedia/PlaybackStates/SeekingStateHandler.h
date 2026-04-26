/*
 * Copyright (c) 2025-2026, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/Time.h>
#include <LibMedia/PipelineStatus.h>
#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/Forward.h>
#include <LibMedia/PlaybackStates/ResumingStateHandler.h>
#include <LibMedia/Producers/DecodedAudioProducer.h>
#include <LibMedia/Producers/DecodedVideoProducer.h>
#include <LibMedia/SeekMode.h>
#include <LibMedia/Sinks/AudioPlaybackSink.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>

namespace Media {

class SeekingStateHandler final : public ResumingStateHandler {
public:
    SeekingStateHandler(PlaybackManager& manager, bool playing, AK::Duration timestamp, SeekMode mode)
        : ResumingStateHandler(manager, playing)
        , m_target_timestamp(timestamp)
        , m_mode(mode)
    {
    }
    virtual ~SeekingStateHandler() override = default;

    virtual void on_enter() override
    {
        ResumingStateHandler::on_enter();
        begin_seek();
    }

    virtual void on_exit() override
    {
        VERIFY(m_video_seeks_pending.is_empty());
        VERIFY(!m_audio_seek_pending);
    }

    virtual void seek(AK::Duration timestamp, SeekMode mode) override
    {
        m_target_timestamp = timestamp;
        m_mode = mode;
        begin_seek();
    }

    virtual PlaybackState state() override
    {
        return PlaybackState::Seeking;
    }
    virtual AvailableData available_data() override
    {
        return AvailableData::Current;
    }

    virtual void enter_buffering() override { }
    virtual void exit_buffering() override { }

    virtual void on_audio_sink_state_changed(PipelineStatus status) override
    {
        if (status == PipelineStatus::Pending)
            return;
        m_audio_seek_pending = false;
        possibly_complete_seek();
    }

    virtual void on_video_sink_state_changed(Track const& track, PipelineStatus status) override
    {
        if (status == PipelineStatus::Pending)
            return;
        m_video_seeks_pending.remove(track);
        possibly_complete_seek();
    }

private:
    void begin_seek()
    {
        m_chosen_timestamp = m_target_timestamp;
        m_video_seeks_pending.clear();
        m_audio_seek_pending = false;

        for (auto& video_track_data : manager().m_video_track_datas) {
            if (video_track_data.display == nullptr)
                continue;
            m_video_seeks_pending.set(video_track_data.track);
            video_track_data.display->seek(m_chosen_timestamp);
        }

        if (manager().m_audio_sink) {
            m_audio_seek_pending = true;
            manager().m_audio_sink->seek(m_chosen_timestamp);
        } else {
            manager().m_time_provider->seek(m_chosen_timestamp);
        }

        possibly_complete_seek();
    }

    void possibly_complete_seek()
    {
        if (m_audio_seek_pending)
            return;
        if (!m_video_seeks_pending.is_empty())
            return;
        resume();
    }

    AK::Duration m_target_timestamp;
    SeekMode m_mode { SeekMode::Accurate };
    AK::Duration m_chosen_timestamp { AK::Duration::zero() };
    HashTable<Track> m_video_seeks_pending;
    bool m_audio_seek_pending { false };
};

}
