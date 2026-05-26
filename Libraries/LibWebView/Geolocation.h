/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibIPC/Forward.h>
#include <LibWebView/Export.h>

namespace WebView {

struct WEBVIEW_API GeolocationPositionData {
    Optional<double> latitude {};
    Optional<double> longitude {};
    Optional<double> accuracy {};
    Optional<double> altitude {};
    Optional<double> altitude_accuracy {};
    Optional<double> heading {};
    Optional<double> speed {};
};

}

namespace IPC {

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::GeolocationPositionData const&);

template<>
WEBVIEW_API ErrorOr<WebView::GeolocationPositionData> decode(Decoder&);

}
