/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>
#include <LibGfx/Size.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web::HTML {

struct WebViewHints {
    bool popup = false;
    Optional<Gfx::IntSize> picture_in_picture_video_size;
    Optional<DevicePixels> width;
    Optional<DevicePixels> height;
    Optional<DevicePixels> screen_x;
    Optional<DevicePixels> screen_y;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::WebViewHints const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::WebViewHints> decode(Decoder&);

}
