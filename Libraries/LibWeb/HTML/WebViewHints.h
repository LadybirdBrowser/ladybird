/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/TokenizedFeatures.h>
#include <LibWeb/PixelUnits.h>

namespace Web::HTML {

struct WebViewHints {
    bool popup = false;
    Optional<DevicePixels> width;
    Optional<DevicePixels> height;
    Optional<DevicePixels> screen_x;
    Optional<DevicePixels> screen_y;

    static WebViewHints from_tokenised_features(TokenizedFeature::Map const&, Page const&);
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Web::HTML::WebViewHints const&);

template<>
ErrorOr<Web::HTML::WebViewHints> decode(Decoder&);

}
