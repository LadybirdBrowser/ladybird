/*
 * Copyright (c) 2025-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PlayingStateHandler.h"

#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/BufferingStateHandler.h>
#include <LibMedia/PlaybackStates/EndedStateHandler.h>
#include <LibMedia/PlaybackStates/PausedStateHandler.h>

namespace Media {

void PlayingStateHandler::pause()
{
    manager().replace_state_handler<PausedStateHandler>();
}

void PlayingStateHandler::on_pipeline_status_changed(PipelineStatus status)
{
    if (status == PipelineStatus::Blocked) {
        manager().replace_state_handler<BufferingStateHandler>(true);
        return;
    }

    if (status == PipelineStatus::EndOfStream)
        manager().replace_state_handler<EndedStateHandler>();
}

}
