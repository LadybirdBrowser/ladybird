/*
 * Copyright (c) 2025, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PausedStateHandler.h"

#include <LibMedia/PlaybackStates/PlayingStateHandler.h>
#include <LibMedia/PlaybackStates/SuspendedStateHandler.h>

namespace Media {

void PausedStateHandler::play()
{
    manager().replace_state_handler<PlayingStateHandler>();
}

void PausedStateHandler::on_pipeline_status_changed(PipelineStatus status)
{
    if (status == PipelineStatus::Suspended) {
        manager().replace_state_handler<SuspendedStateHandler>();
        return;
    }
    PlaybackStateHandler::on_pipeline_status_changed(status);
}

}
