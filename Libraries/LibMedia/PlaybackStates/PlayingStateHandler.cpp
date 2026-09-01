/*
 * Copyright (c) 2025-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PlayingStateHandler.h"

#include <AK/Math.h>
#include <LibCore/Timer.h>
#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/BufferingStateHandler.h>
#include <LibMedia/PlaybackStates/EndedStateHandler.h>
#include <LibMedia/PlaybackStates/PausedStateHandler.h>

namespace Media {

PlayingStateHandler::PlayingStateHandler(PlaybackManager& manager)
    : PlaybackStateHandler(manager)
{
}

PlayingStateHandler::~PlayingStateHandler() = default;

void PlayingStateHandler::pause()
{
    manager().replace_state_handler<PausedStateHandler>();
}

void PlayingStateHandler::on_pipeline_status_changed(PipelineStatus status)
{
    update_unticked_end_of_stream_timer();

    if (status == PipelineStatus::Blocked) {
        manager().replace_state_handler<BufferingStateHandler>(true);
        return;
    }

    if (status == PipelineStatus::EndOfStream)
        manager().replace_state_handler<EndedStateHandler>();
}

void PlayingStateHandler::update_unticked_end_of_stream_timer()
{
    auto now = manager().current_time();

    auto latest_verified_end_time = now;
    for (auto const& track_data : manager().m_video_track_datas) {
        if (!track_data.handle.has_value() || track_data.ticking)
            continue;
        if (is_terminal(track_data.sink_status))
            continue;
        auto verified_end_time = manager().verified_end_time_for_track(track_data.track);
        if (!verified_end_time.has_value() || *verified_end_time <= latest_verified_end_time)
            continue;
        latest_verified_end_time = *verified_end_time;
    }
    if (!manager().has_ticking_audio_sink()) {
        for (auto const& track_data : manager().m_audio_track_datas) {
            if (!track_data.enabled)
                continue;
            auto verified_end_time = manager().verified_end_time_for_track(track_data.track);
            if (!verified_end_time.has_value() || *verified_end_time <= latest_verified_end_time)
                continue;
            latest_verified_end_time = *verified_end_time;
        }
    }

    auto playback_rate = manager().m_playback_rate;
    if (latest_verified_end_time == now || playback_rate <= 0) {
        if (m_unticked_end_of_stream_timer != nullptr)
            m_unticked_end_of_stream_timer->stop();
        return;
    }

    if (m_unticked_end_of_stream_timer == nullptr) {
        m_unticked_end_of_stream_timer = Core::Timer::create_single_shot(0, [&manager = manager()] {
            manager.update_pipeline_state();
        });
    }

    auto delay_seconds = (latest_verified_end_time - now).to_seconds_f64() / playback_rate;
    m_unticked_end_of_stream_timer->restart(max(1, AK::clamp_to<int>(AK::ceil(delay_seconds * 1'000.0))));
}

}
