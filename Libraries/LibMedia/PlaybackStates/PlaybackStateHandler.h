/*
 * Copyright (c) 2025, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/Forward.h>
#include <LibMedia/PlaybackStates/PlaybackState.h>

namespace Media {

class PlaybackStateHandler {
public:
    PlaybackStateHandler(PlaybackManager& manager)
        : m_manager(manager)
    {
    }
    virtual ~PlaybackStateHandler() = default;

    virtual void on_enter() = 0;
    virtual void on_exit() = 0;

    virtual void play() = 0;
    virtual void pause() = 0;

    virtual bool is_playing() = 0;
    virtual PlaybackState state() = 0;

protected:
    PlaybackManager& manager() const { return m_manager; }

private:
    PlaybackManager& m_manager;
};

}
