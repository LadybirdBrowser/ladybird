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
        track_data.provider->seek(manager().current_time(), SeekMode::Accurate, [display = NonnullRefPtr(*track_data.display)](AK::Duration) {
            display->resume_updates();
        });
        return;
    }

    VERIFY(track.type() == TrackType::Audio);
    auto& track_data = manager().get_audio_data_for_track(track);
    VERIFY(manager().m_audio_sink != nullptr);
    track_data.provider->seek(manager().current_time(), [track, sink = NonnullRefPtr(*manager().m_audio_sink)] {
        sink->clear_track_data(track);
    });
}

}
