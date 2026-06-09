/*
 * Copyright (c) 2025-2026, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

    virtual void on_exit() override { }

    virtual AK::Duration current_time() const override { return m_chosen_timestamp; }

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

    virtual void on_pipeline_status_changed(PipelineStatus status) override
    {
        if (!resolves_seek(status))
            return;

        if (status == PipelineStatus::EndOfStream) {
            PlaybackStateHandler::on_pipeline_status_changed(status);
            return;
        }

        resume();
    }

private:
    AK::Duration choose_timestamp() const
    {
        if (m_mode == SeekMode::Accurate)
            return m_target_timestamp;
        Optional<AK::Duration> latest_fast_seek_target;
        for (auto const& video_track_data : manager().m_video_track_datas) {
            if (video_track_data.display == nullptr)
                continue;
            auto fast_seek_target = video_track_data.producer->select_fast_seek_target(m_target_timestamp, m_mode);
            if (!latest_fast_seek_target.has_value() || fast_seek_target > latest_fast_seek_target.value())
                latest_fast_seek_target = fast_seek_target;
        }
        return latest_fast_seek_target.value_or(m_target_timestamp);
    }

    void begin_seek()
    {
        m_chosen_timestamp = choose_timestamp();

        for (auto& video_track_data : manager().m_video_track_datas) {
            if (video_track_data.display == nullptr)
                continue;
            video_track_data.display->seek(m_chosen_timestamp);
        }

        if (manager().m_audio_sink)
            manager().m_audio_sink->seek(m_chosen_timestamp);
        else
            manager().m_time_provider->seek(m_chosen_timestamp);
    }

    AK::Duration m_target_timestamp;
    SeekMode m_mode { SeekMode::Accurate };
    AK::Duration m_chosen_timestamp { AK::Duration::zero() };
};

}
