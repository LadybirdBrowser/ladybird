/*
 * Copyright (c) 2025, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibMedia/PlaybackStates/PlaybackStateHandler.h>

#define ENUMERATE_PLAYBACK_STATE_HANDLERS(X) \
    X(PlaybackStateHandler)                  \
    X(PlayingStateHandler)                   \
    X(PausedStateHandler)                    \
    X(ResumingStateHandler)                  \
    X(SeekingStateHandler)

namespace Media {

#define __ENUMERATE_PLAYBACK_STATE_HANDLER(clazz) \
    class clazz;
ENUMERATE_PLAYBACK_STATE_HANDLERS(__ENUMERATE_PLAYBACK_STATE_HANDLER)
#undef __ENUMERATE_PLAYBACK_STATE_HANDLER

}
