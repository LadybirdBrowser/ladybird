/*
 * Copyright (c) 2025-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/SeekingStateHandler.h>

#include "PlaybackStateHandler.h"

namespace Media {

void PlaybackStateHandler::seek(AK::Duration timestamp, SeekMode mode)
{
    manager().replace_state_handler<SeekingStateHandler>(manager().is_playing(), timestamp, mode);
}

}
