/*
 * Copyright (c) 2025, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PausedStateHandler.h"

#include <LibMedia/PlaybackStates/PlayingStateHandler.h>

namespace Media {

void PausedStateHandler::play()
{
    manager().replace_state_handler<PlayingStateHandler>();
}

}
