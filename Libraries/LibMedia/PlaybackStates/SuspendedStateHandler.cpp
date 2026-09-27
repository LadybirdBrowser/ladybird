/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SuspendedStateHandler.h"

#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/EndedStateHandler.h>
#include <LibMedia/PlaybackStates/PausedStateHandler.h>
#include <LibMedia/PlaybackStates/SeekingStateHandler.h>

namespace Media {

void SuspendedStateHandler::play()
{
    manager().replace_state_handler<SeekingStateHandler>(true, current_time(), SeekMode::Accurate);
}

void SuspendedStateHandler::on_pipeline_status_changed(PipelineStatus status)
{
    if (status == PipelineStatus::Suspended)
        return;
    if (status == PipelineStatus::EndOfStream) {
        manager().replace_state_handler<EndedStateHandler>(false);
        return;
    }
    manager().replace_state_handler<PausedStateHandler>();
}

}
