/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StartingStateHandler.h"

#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/BufferingStateHandler.h>

namespace Media {

void StartingStateHandler::start()
{
    m_started = true;

    if (!manager().m_audio_buffering && manager().m_video_tracks_buffering.is_empty())
        resume();
}

void StartingStateHandler::exit_buffering()
{
    if (m_started)
        resume();
}

}
