/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibIPC/Forward.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct WEBVIEW_API Attribute {
    Utf16FlyString name;
    Utf16String value;
};

}

namespace IPC {

template<>
WEBVIEW_API ErrorOr<void> encode(Encoder&, WebView::Attribute const&);

template<>
WEBVIEW_API ErrorOr<WebView::Attribute> decode(Decoder&);

}
