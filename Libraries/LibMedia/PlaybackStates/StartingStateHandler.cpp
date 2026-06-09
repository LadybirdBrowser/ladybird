/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "StartingStateHandler.h"

#include <LibMedia/PlaybackManager.h>

namespace Media {

void StartingStateHandler::start()
{
    m_started = true;

    if (!m_pipeline_blocked)
        resume();
}

void StartingStateHandler::on_pipeline_status_changed(PipelineStatus status)
{
    m_pipeline_blocked = status == PipelineStatus::Blocked;

    if (m_started && !m_pipeline_blocked)
        resume();
}

}
