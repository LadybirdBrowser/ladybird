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

    if (m_data_available)
        resume();
}

void StartingStateHandler::on_pipeline_status_changed(PipelineStatus status)
{
    m_data_available = resolves_seek(status);

    if (m_started && m_data_available)
        resume();
}

}
