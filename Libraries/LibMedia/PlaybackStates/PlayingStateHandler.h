/*
 * Copyright (c) 2025, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/Forward.h>
#include <LibMedia/PlaybackStates/PausedStateHandler.h>

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
    virtual void pause() override
    {
        manager().replace_state_handler<PausedStateHandler>();
    }

    virtual bool is_playing() override
    {
        return true;
    }
};

}
