/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/Forward.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Export.h>

namespace Web {

struct MediaContextMenu {
    Optional<URL::URL> media_url;
    bool is_video { false };
    bool is_playing { false };
    bool is_muted { false };
    bool has_user_agent_controls { false };
    bool is_looping { false };
    bool is_fullscreen { false };
    bool is_picture_in_picture { false };
    bool can_enter_picture_in_picture { false };
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::MediaContextMenu const&);

template<>
WEBCOMMON_API ErrorOr<Web::MediaContextMenu> decode(Decoder&);

}
