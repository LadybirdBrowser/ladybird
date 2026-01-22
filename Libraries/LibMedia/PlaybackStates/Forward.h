/*
 * Copyright (c) 2025, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/PlaybackStates/PlaybackStateHandler.h>

#define ENUMERATE_PLAYBACK_STATE_HANDLERS(X) \
    X(BufferingStateHandler)                 \
    X(PlaybackStateHandler)                  \
    X(PlayingStateHandler)                   \
    X(PausedStateHandler)                    \
    X(ResumingStateHandler)                  \
    X(SeekingStateHandler)                   \
    X(SuspendedStateHandler)

namespace Media {

#define __ENUMERATE_PLAYBACK_STATE_HANDLER(type) \
    class type;
ENUMERATE_PLAYBACK_STATE_HANDLERS(__ENUMERATE_PLAYBACK_STATE_HANDLER)
#undef __ENUMERATE_PLAYBACK_STATE_HANDLER

}
