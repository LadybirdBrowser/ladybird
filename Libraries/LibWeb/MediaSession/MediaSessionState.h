/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibWebCommon/Bindings/MediaSession.h>

namespace Web::MediaSession {

struct MediaSessionPositionState {
    double duration;
    double playback_rate;
    double position;
    MonotonicTime last_position_updated_time;

    bool operator==(MediaSessionPositionState const&) const = default;
};

struct MediaSessionState {
    u64 activation_counter { 0 };
    String title;
    String artist;
    String album;
    Bindings::MediaSessionPlaybackState playback_state { Bindings::MediaSessionPlaybackState::Paused };
    Optional<MediaSessionPositionState> position;
    Vector<Bindings::MediaSessionAction> supported_actions;

    bool operator==(MediaSessionState const&) const = default;
};

}
