/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibGfx/Point.h>
#include <LibIPC/Forward.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct WEBVIEW_API DictionaryLookupTextStyle {
    String font_family;
    float ui_point_size { 0 };
    u16 weight { 0 };
    u8 slope { 0 };
};

struct WEBVIEW_API DictionaryLookup {
    String text;
    Optional<DictionaryLookupTextStyle> style;
    Optional<Gfx::IntPoint> baseline_origin;
};

}

namespace IPC {

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DictionaryLookupTextStyle const&);

template<>
WEBVIEW_API ErrorOr<WebView::DictionaryLookupTextStyle> decode(Decoder&);

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::DictionaryLookup const&);

template<>
WEBVIEW_API ErrorOr<WebView::DictionaryLookup> decode(Decoder&);

}
