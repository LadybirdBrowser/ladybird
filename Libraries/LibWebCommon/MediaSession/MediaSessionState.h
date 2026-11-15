/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Bindings/MediaSession.h>
#include <LibWebCommon/Export.h>

namespace Web::MediaSession {

inline constexpr int artwork_maximum_side = 512;

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
    RefPtr<Gfx::Bitmap const> artwork;
    Bindings::MediaSessionPlaybackState playback_state { Bindings::MediaSessionPlaybackState::Paused };
    Optional<MediaSessionPositionState> position;
    Vector<Bindings::MediaSessionAction> supported_actions;

    bool operator==(MediaSessionState const&) const = default;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::MediaSession::MediaSessionPositionState const&);

template<>
WEBCOMMON_API ErrorOr<Web::MediaSession::MediaSessionPositionState> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::MediaSession::MediaSessionState const&);

template<>
WEBCOMMON_API ErrorOr<Web::MediaSession::MediaSessionState> decode(Decoder&);

}
