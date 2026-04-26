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
};

}
