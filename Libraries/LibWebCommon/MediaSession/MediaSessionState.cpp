/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ShareableBitmap.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/MediaSession/MediaSessionState.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::MediaSession::MediaSessionPositionState const& state)
{
    TRY(encoder.encode(state.duration));
    TRY(encoder.encode(state.playback_rate));
    TRY(encoder.encode(state.position));
    // Every process on the machine reads the same monotonic clock, so the timestamp means the same to the receiver.
    TRY(encoder.encode(state.last_position_updated_time.nanoseconds()));
    return {};
}

template<>
ErrorOr<Web::MediaSession::MediaSessionPositionState> decode(Decoder& decoder)
{
    auto duration = TRY(decoder.decode<double>());
    auto playback_rate = TRY(decoder.decode<double>());
    auto position = TRY(decoder.decode<double>());
    auto last_position_updated_time = TRY(decoder.decode<i64>());
    return Web::MediaSession::MediaSessionPositionState {
        duration, playback_rate, position, MonotonicTime {} + AK::Duration::from_nanoseconds(last_position_updated_time)
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::MediaSession::MediaSessionState const& state)
{
    TRY(encoder.encode(state.activation_counter));
    TRY(encoder.encode(state.title));
    TRY(encoder.encode(state.artist));
    TRY(encoder.encode(state.album));
    TRY(encoder.encode(state.playback_state));
    TRY(encoder.encode(state.position));
    TRY(encoder.encode(state.supported_actions));
    Optional<Gfx::ShareableBitmap> artwork;
    if (state.artwork)
        artwork = state.artwork->to_shareable_bitmap();
    TRY(encoder.encode(artwork));
    return {};
}

template<>
ErrorOr<Web::MediaSession::MediaSessionState> decode(Decoder& decoder)
{
    Web::MediaSession::MediaSessionState state;
    state.activation_counter = TRY(decoder.decode<u64>());
    state.title = TRY(decoder.decode<String>());
    state.artist = TRY(decoder.decode<String>());
    state.album = TRY(decoder.decode<String>());
    state.playback_state = TRY(decoder.decode<Web::Bindings::MediaSessionPlaybackState>());
    if (state.playback_state != Web::Bindings::MediaSessionPlaybackState::None
        && state.playback_state != Web::Bindings::MediaSessionPlaybackState::Paused
        && state.playback_state != Web::Bindings::MediaSessionPlaybackState::Playing)
        return Error::from_string_literal("Invalid media session playback state");
    state.position = TRY(decoder.decode<Optional<Web::MediaSession::MediaSessionPositionState>>());
    state.supported_actions = TRY(decoder.decode<Vector<Web::Bindings::MediaSessionAction>>());
    for (auto action : state.supported_actions) {
        if (to_underlying(action) > to_underlying(Web::Bindings::MediaSessionAction::Voiceactivity))
            return Error::from_string_literal("Invalid media session action");
    }
    auto artwork = TRY(decoder.decode<Optional<Gfx::ShareableBitmap>>());
    if (artwork.has_value())
        state.artwork = artwork->bitmap();
    if (state.artwork && (state.artwork->width() > Web::MediaSession::artwork_maximum_side || state.artwork->height() > Web::MediaSession::artwork_maximum_side))
        return Error::from_string_literal("Media session artwork exceeds the maximum dimensions");
    return state;
}

}
