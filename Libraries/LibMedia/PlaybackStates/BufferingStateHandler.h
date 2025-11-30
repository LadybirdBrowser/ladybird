/*
 * Copyright (c) 2025, Gregory Bertilson <zaggy1024@gmail.com>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/PlaybackStates/Forward.h>
#include <LibMedia/PlaybackStates/ResumingStateHandler.h>

namespace Media {

class BufferingStateHandler final : public ResumingStateHandler {
public:
    BufferingStateHandler(PlaybackManager& manager, bool playing)
        : ResumingStateHandler(manager, playing)
    {
    }

    virtual ~BufferingStateHandler() override = default;

    virtual void on_enter() override
    {
        dbgln(">ENTER BUFFERING");
        ResumingStateHandler::on_enter();
    }

    virtual void on_exit() override
    {
        dbgln(">EXIT BUFFERING");
        ResumingStateHandler::on_exit();
    }

    virtual PlaybackState state() override
    {
        return PlaybackState::Buffering;
    }

    virtual void enter_buffering() override
    {
        // VERIFY_NOT_REACHED();
    }

    virtual void exit_buffering() override
    {
        dbgln(">BufferingStateHandler::exit_buffering");
        resume();
    }
};

}
