/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/NowPlayingInfo.h>
#include <math.h>

namespace WebView {

NowPlayingInfo now_playing_info_for(Optional<NowPlayingState> const& state, MonotonicTime now)
{
    NowPlayingInfo info;
    if (!state.has_value())
        return info;

    switch (state->playback_state) {
    case Web::Bindings::MediaSessionPlaybackState::Playing:
        info.playback_status = NowPlayingPlaybackStatus::Playing;
        break;
    case Web::Bindings::MediaSessionPlaybackState::Paused:
        info.playback_status = NowPlayingPlaybackStatus::Paused;
        break;
    case Web::Bindings::MediaSessionPlaybackState::None:
        break;
    }
    info.title = state->title;
    info.artist = state->artist;
    info.album = state->album;

    bool playing = info.playback_status == NowPlayingPlaybackStatus::Playing;
    info.playback_rate = playing ? 1.0 : 0.0;
    if (state->position.has_value()) {
        auto const& position = *state->position;
        if (isfinite(position.duration) && position.duration >= 0)
            info.duration = position.duration;
        if (playing) {
            info.playback_rate.clear();
            if (isfinite(position.playback_rate))
                info.playback_rate = position.playback_rate;
        }
        double elapsed = position.position;
        if (playing)
            elapsed += (now - position.last_position_updated_time).to_seconds_f64() * position.playback_rate;
        if (isfinite(elapsed)) {
            elapsed = max(0.0, elapsed);
            if (info.duration.has_value())
                elapsed = min(elapsed, *info.duration);
            info.elapsed_time = elapsed;
        }
    }

    using Web::Bindings::MediaSessionAction;
    auto const& actions = state->supported_actions;
    info.commands.play = actions.contains_slow(MediaSessionAction::Play);
    info.commands.pause = actions.contains_slow(MediaSessionAction::Pause);
    info.commands.toggle_play_pause = info.commands.play && info.commands.pause;
    info.commands.stop = actions.contains_slow(MediaSessionAction::Stop);
    info.commands.next_track = actions.contains_slow(MediaSessionAction::Nexttrack);
    info.commands.previous_track = actions.contains_slow(MediaSessionAction::Previoustrack);
    info.commands.change_playback_position = info.duration.has_value() && actions.contains_slow(MediaSessionAction::Seekto);
    return info;
}

}
