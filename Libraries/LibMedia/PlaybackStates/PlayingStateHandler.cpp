/*
 * Copyright (c) 2025, Gregory Bertilson <zaggy1024@gmail.com>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/PlaybackStates/BufferingStateHandler.h>
#include <LibMedia/PlaybackStates/PlayingStateHandler.h>

namespace Media {

void PlayingStateHandler::enter_buffering()
{
    manager().replace_state_handler<BufferingStateHandler>(true);
}

}
