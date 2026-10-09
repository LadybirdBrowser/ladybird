/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/Page/MediaContextMenu.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::MediaContextMenu const& menu)
{
    TRY(encoder.encode(menu.media_url));
    TRY(encoder.encode(menu.is_video));
    TRY(encoder.encode(menu.is_playing));
    TRY(encoder.encode(menu.is_muted));
    TRY(encoder.encode(menu.has_user_agent_controls));
    TRY(encoder.encode(menu.is_looping));
    TRY(encoder.encode(menu.is_fullscreen));
    TRY(encoder.encode(menu.is_picture_in_picture));
    TRY(encoder.encode(menu.can_enter_picture_in_picture));
    return {};
}

template<>
ErrorOr<Web::MediaContextMenu> IPC::decode(Decoder& decoder)
{
    return Web::MediaContextMenu {
        .media_url = TRY(decoder.decode<Optional<URL::URL>>()),
        .is_video = TRY(decoder.decode<bool>()),
        .is_playing = TRY(decoder.decode<bool>()),
        .is_muted = TRY(decoder.decode<bool>()),
        .has_user_agent_controls = TRY(decoder.decode<bool>()),
        .is_looping = TRY(decoder.decode<bool>()),
        .is_fullscreen = TRY(decoder.decode<bool>()),
        .is_picture_in_picture = TRY(decoder.decode<bool>()),
        .can_enter_picture_in_picture = TRY(decoder.decode<bool>()),
    };
}
