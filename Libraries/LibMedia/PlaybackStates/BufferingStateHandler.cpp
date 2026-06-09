/*
 * Copyright (c) 2025-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BufferingStateHandler.h"

#include <LibMedia/PlaybackManager.h>

namespace Media {

void BufferingStateHandler::on_pipeline_status_changed(PipelineStatus status)
{
    if (status != PipelineStatus::Blocked)
        resume();
}

}
