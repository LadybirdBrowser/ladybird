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

void PlaybackStateHandler::on_track_enabled(Track const& track)
{
    if (track.type() == TrackType::Video) {
        auto& track_data = manager().get_video_data_for_track(track);
        VERIFY(track_data.display != nullptr);
        track_data.display->pause_updates();
        auto weak_manager = manager().m_weak_wrapper;
        track_data.provider->seek(manager().current_time(), SeekMode::Accurate, [weak_manager, track](AK::Duration) {
            auto manager = weak_manager->take_strong();
            if (!manager)
                return;
            auto& current_track_data = manager->get_video_data_for_track(track);
            if (!current_track_data.display)
                return;
            current_track_data.display->resume_updates();
        });
        return;
    }

    VERIFY(track.type() == TrackType::Audio);
    auto& track_data = manager().get_audio_data_for_track(track);
    VERIFY(manager().m_audio_sink != nullptr);
    auto weak_manager = manager().m_weak_wrapper;
    track_data.provider->seek(manager().current_time(), [track, weak_manager] {
        auto manager = weak_manager->take_strong();
        if (!manager || !manager->m_audio_sink)
            return;
        manager->m_audio_sink->clear_track_data(track);
    });
}

}
