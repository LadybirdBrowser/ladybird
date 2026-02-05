/*
 * Copyright (c) 2025, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PausedStateHandler.h"

#include <LibCore/Timer.h>
#include <LibMedia/PlaybackStates/PlayingStateHandler.h>
#include <LibMedia/PlaybackStates/SuspendedStateHandler.h>

namespace Media {

PausedStateHandler::PausedStateHandler(PlaybackManager& manager, int suspend_timeout_ms)
    : PlaybackStateHandler(manager)
    , m_suspend_timer(Core::Timer::create_single_shot(suspend_timeout_ms, [this] {
        suspend();
    }))
{
}

PausedStateHandler::~PausedStateHandler()
{
    m_suspend_timer->stop();
}

void PausedStateHandler::on_enter()
{
    m_suspend_timer->start();
}

void PausedStateHandler::on_exit()
{
    m_suspend_timer->stop();
}

void PausedStateHandler::play()
{
    manager().replace_state_handler<PlayingStateHandler>();
}

void PausedStateHandler::suspend()
{
    manager().replace_state_handler<SuspendedStateHandler>();
}

}
