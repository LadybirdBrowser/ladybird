/*
 * Copyright (c) 2025-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/EndedStateHandler.h>
#include <LibMedia/PlaybackStates/SeekingStateHandler.h>

#include "PlaybackStateHandler.h"

namespace Media {

AK::Duration PlaybackStateHandler::current_time() const
{
    return manager().m_time_provider->current_time();
}

void PlaybackStateHandler::seek(AK::Duration timestamp, SeekMode mode)
{
    manager().replace_state_handler<SeekingStateHandler>(manager().is_playing(), timestamp, mode);
}

void PlaybackStateHandler::on_pipeline_status_changed(PipelineStatus status)
{
    if (status == PipelineStatus::EndOfStream)
        manager().replace_state_handler<EndedStateHandler>();
}

}
