/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibIPC/Forward.h>

namespace WebView {

struct Attribute {
    String name;
    String value;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, WebView::Attribute const&);

template<>
ErrorOr<WebView::Attribute> decode(Decoder&);

}
