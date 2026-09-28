/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonValue.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Forward.h>

namespace WebView {

struct WEBCOMMON_API DOMNodeProperties {
    enum class Type {
        AppliedStyleRules,
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
WEBCOMMON_API ErrorOr<void> encode(Encoder&, WebView::DOMNodeProperties const&);

template<>
WEBCOMMON_API ErrorOr<WebView::DOMNodeProperties> decode(Decoder&);

}
