/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/PlaybackStates/Forward.h>

namespace Media {

class ResumingStateHandler : public PlaybackStateHandler {
public:
    ResumingStateHandler(PlaybackManager& manager, bool playing)
        : PlaybackStateHandler(manager)
        , m_playing(playing)
    {
    }
    virtual ~ResumingStateHandler() override = default;

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

protected:
    void resume();

private:
    bool m_playing { false };
};

}
