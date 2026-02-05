/*
 * Copyright (c) 2025, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/Forward.h>
#include <LibMedia/PlaybackStates/SeekingStateHandler.h>
#include <LibMedia/Providers/AudioDataProvider.h>
#include <LibMedia/Providers/VideoDataProvider.h>

namespace Media {

class SuspendedStateHandler final : public PlaybackStateHandler {
public:
    SuspendedStateHandler(PlaybackManager& manager)
        : PlaybackStateHandler(manager)
    {
    }
    virtual ~SuspendedStateHandler() override = default;

    virtual void on_enter() override
    {
        for (auto& video_track_data : manager().m_video_track_datas)
            video_track_data.provider->suspend();
        for (auto& audio_track_data : manager().m_audio_track_datas)
            audio_track_data.provider->suspend();
    }

    virtual void on_exit() override
    {
        for (auto& video_track_data : manager().m_video_track_datas)
            video_track_data.provider->resume();
        for (auto& audio_track_data : manager().m_audio_track_datas)
            audio_track_data.provider->resume();
    }

    virtual void play() override
    {
        manager().replace_state_handler<SeekingStateHandler>(true, manager().current_time(), SeekMode::Accurate);
    }

    virtual void pause() override { }

    virtual void seek(AK::Duration timestamp, SeekMode mode) override
    {
        manager().replace_state_handler<SeekingStateHandler>(false, timestamp, mode);
    }

    virtual bool is_playing() override
    {
        return false;
    }

    virtual PlaybackState state() override
    {
        return PlaybackState::Suspended;
    }

    virtual void enter_buffering() override { }
    virtual void exit_buffering() override { }

    virtual void on_track_enabled(Track const& track) override
    {
        if (track.type() == TrackType::Video) {
            auto& track_data = manager().get_video_data_for_track(track);
            VERIFY(track_data.display != nullptr);
            track_data.display->pause_updates();
            track_data.provider->resume();
            track_data.provider->seek(manager().current_time(), SeekMode::Accurate, [provider = track_data.provider, display = track_data.display](AK::Duration) {
                display->resume_updates();
                provider->suspend();
            });
            return;
        }

        VERIFY(track.type() == TrackType::Audio);
        auto& track_data = manager().get_audio_data_for_track(track);
        VERIFY(manager().m_audio_sink != nullptr);
        track_data.provider->resume();
        track_data.provider->seek(manager().current_time(), [track, provider = track_data.provider, sink = manager().m_audio_sink] {
            sink->clear_track_data(track);
            provider->suspend();
        });
    }
};

}
