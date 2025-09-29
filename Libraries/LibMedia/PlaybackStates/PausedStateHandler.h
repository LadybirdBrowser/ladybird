/*
 * Copyright (c) 2025, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/Forward.h>
#include <LibMedia/PlaybackStates/PlayingStateHandler.h>

namespace Media {

class PausedStateHandler final : public PlaybackStateHandler {
public:
    PausedStateHandler(PlaybackManager& manager)
        : PlaybackStateHandler(manager)
    {
    }
    virtual ~PausedStateHandler() override = default;

    virtual void on_enter() override
    {
    }
    virtual void on_exit() override
    {
    }

    virtual void play() override
    {
        manager().replace_state_handler<PlayingStateHandler>();
    }
    virtual void pause() override { }

    virtual bool is_playing() override
    {
        return false;
    }
};

}
