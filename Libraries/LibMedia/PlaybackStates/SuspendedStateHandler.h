/*
 * Copyright (c) 2025, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/Forward.h>
#include <LibMedia/PlaybackStates/SeekingStateHandler.h>
#include <LibMedia/Processors/AudioMixer.h>
#include <LibMedia/Producers/DecodedAudioProducer.h>
#include <LibMedia/Producers/DecodedVideoProducer.h>

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
            video_track_data.producer->suspend();
        for (auto& audio_track_data : manager().m_audio_track_datas)
            audio_track_data.producer->suspend();
    }

    virtual void on_exit() override
    {
        for (auto& video_track_data : manager().m_video_track_datas)
            video_track_data.producer->resume();
        for (auto& audio_track_data : manager().m_audio_track_datas)
            audio_track_data.producer->resume();
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
    virtual AvailableData available_data() override
    {
        return AvailableData::Future;
    }

    virtual void enter_buffering() override { }
    virtual void exit_buffering() override { }

    virtual void on_track_enabled(Track const& track) override
    {
        if (track.type() == TrackType::Video) {
            auto& track_data = manager().get_video_data_for_track(track);
            VERIFY(track_data.display != nullptr);
            track_data.display->pause_updates();
            track_data.producer->resume();
            track_data.producer->seek(manager().current_time(), SeekMode::Accurate, [manager = manager().weak(), producer = track_data.producer, display = track_data.display](AK::Duration) {
                if (!manager)
                    return;
                display->resume_updates();
                if (manager->state() != PlaybackState::Buffering)
                    producer->suspend();
            });
            return;
        }

        VERIFY(track.type() == TrackType::Audio);
        auto& track_data = manager().get_audio_data_for_track(track);
        if (!manager().m_audio_mixer)
            return;
        track_data.producer->resume();
        track_data.producer->seek(manager().current_time(), [manager = manager().weak(), producer = track_data.producer] {
            if (!manager)
                return;
            if (manager->state() == PlaybackState::Suspended)
                producer->suspend();
        });
    }
};

}
