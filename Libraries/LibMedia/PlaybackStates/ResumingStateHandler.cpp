/*
 * Copyright (c) 2025-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ResumingStateHandler.h"

#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/PausedStateHandler.h>
#include <LibMedia/PlaybackStates/PlayingStateHandler.h>

namespace Media {

void ResumingStateHandler::resume()
{
    if (m_playing)
        manager().replace_state_handler<PlayingStateHandler>();
    else
        manager().replace_state_handler<PausedStateHandler>(PlaybackManager::RESUMING_SUSPEND_TIMEOUT_MS);
}

}
