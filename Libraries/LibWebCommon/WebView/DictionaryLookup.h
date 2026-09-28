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
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Forward.h>

namespace WebView {

struct WEBCOMMON_API DictionaryLookupTextStyle {
    String font_family;
    float ui_point_size { 0 };
    u16 weight { 0 };
    u8 slope { 0 };
};

struct WEBCOMMON_API DictionaryLookup {
    String text;
    Optional<DictionaryLookupTextStyle> style;
    Optional<Gfx::IntPoint> baseline_origin;
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, WebView::DictionaryLookupTextStyle const&);

template<>
WEBCOMMON_API ErrorOr<WebView::DictionaryLookupTextStyle> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, WebView::DictionaryLookup const&);

template<>
WEBCOMMON_API ErrorOr<WebView::DictionaryLookup> decode(Decoder&);

}
