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

class ResumingStateHandler : public PlaybackStateHandler {
public:
    ResumingStateHandler(PlaybackManager& manager, bool playing)
        : PlaybackStateHandler(manager)
        , m_playing(playing)
    {
    }
    virtual ~ResumingStateHandler() override = default;

    void resume()
    {
        if (m_playing)
            manager().replace_state_handler<PlayingStateHandler>();
        else
            manager().replace_state_handler<PausedStateHandler>();
    }

    virtual void on_enter() override { }
    virtual void on_exit() override { }

    virtual void play() override
    {
        m_playing = true;
    }
    virtual void pause() override
    {
        m_playing = false;
    }

    virtual bool is_playing() override
    {
        return m_playing;
    }

private:
    bool m_playing { false };
};

}
