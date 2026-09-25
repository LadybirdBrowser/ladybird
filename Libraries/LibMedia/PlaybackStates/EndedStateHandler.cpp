/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/EndedStateHandler.h>
#include <LibMedia/PlaybackStates/PausedStateHandler.h>
#include <LibMedia/PlaybackStates/PlayingStateHandler.h>
#include <LibMedia/PlaybackStates/SeekingStateHandler.h>

namespace Media {

EndedStateHandler::EndedStateHandler(PlaybackManager& manager, bool was_playing)
    : PlaybackStateHandler(manager)
    , m_was_playing(was_playing)
{
}

EndedStateHandler::~EndedStateHandler() = default;

void EndedStateHandler::on_enter()
{
    m_position_before_end = manager().m_time_reader.current_time();
    manager().m_clock->pause();
}

AK::Duration EndedStateHandler::current_time() const
{
    return manager().duration();
}

void EndedStateHandler::move_pipeline_to_end()
{
    if (m_pipeline_is_at_end)
        return;
    m_pipeline_is_at_end = true;
    if (manager().duration() > m_position_before_end)
        manager().seek_clock_and_video_sinks(manager().duration());
}

void EndedStateHandler::resume_from_position_before_end()
{
    auto was_playing = m_was_playing;
    auto position_before_end = m_position_before_end;

    if (m_pipeline_is_at_end) {
        manager().replace_state_handler<SeekingStateHandler>(was_playing, position_before_end, SeekMode::Accurate);
        return;
    }
    if (was_playing)
        manager().replace_state_handler<PlayingStateHandler>();
    else
        manager().replace_state_handler<PausedStateHandler>();
}

}
