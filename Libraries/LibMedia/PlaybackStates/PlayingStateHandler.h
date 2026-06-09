/*
 * Copyright (c) 2025, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/Forward.h>

namespace Media {

class PlayingStateHandler final : public PlaybackStateHandler {
public:
    PlayingStateHandler(PlaybackManager& manager)
        : PlaybackStateHandler(manager)
    {
    }
    virtual ~PlayingStateHandler() override = default;

    virtual void on_enter() override
    {
        manager().m_time_provider->resume();
    }
    virtual void on_exit() override
    {
        manager().m_time_provider->pause();
    }

    virtual void play() override { }
    virtual void pause() override;

    virtual bool is_playing() override
    {
        return true;
    }
    virtual PlaybackState state() override
    {
        return PlaybackState::Playing;
    }
    virtual AvailableData available_data() override
    {
        return AvailableData::Future;
    }

    virtual void on_pipeline_status_changed(PipelineStatus) override;
};

}
