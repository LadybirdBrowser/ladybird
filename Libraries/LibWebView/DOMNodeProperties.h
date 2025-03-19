/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonValue.h>
#include <LibIPC/Forward.h>

namespace WebView {

struct DOMNodeProperties {
    enum class Type {
        ComputedStyle,
        Layout,
        UsedFonts,
    };

    Type type { Type::ComputedStyle };
    JsonValue properties;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, WebView::DOMNodeProperties const&);

template<>
ErrorOr<WebView::DOMNodeProperties> decode(Decoder&);

}
