/*
 * Copyright (c) 2025-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Forward.h>
#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/Forward.h>

namespace Media {

class PausedStateHandler final : public PlaybackStateHandler {
public:
    PausedStateHandler(PlaybackManager& manager, int suspend_timeout_ms = PlaybackManager::DEFAULT_SUSPEND_TIMEOUT_MS);
    virtual ~PausedStateHandler() override;

    virtual void on_enter() override;
    virtual void on_exit() override;

    virtual void play() override;
    virtual void pause() override { }

    virtual bool is_playing() override
    {
        return false;
    }
    virtual PlaybackState state() override
    {
        return PlaybackState::Paused;
    }

    virtual void enter_buffering() override { }
    virtual void exit_buffering() override { }

private:
    void suspend();

    NonnullRefPtr<Core::Timer> m_suspend_timer;
};

}
