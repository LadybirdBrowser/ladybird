/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebView/DOMNodeProperties.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::DOMNodeProperties const& attribute)
{
    TRY(encoder.encode(attribute.type));
    TRY(encoder.encode(attribute.properties));
    return {};
}

template<>
ErrorOr<WebView::DOMNodeProperties> IPC::decode(Decoder& decoder)
{
    auto type = TRY(decoder.decode<WebView::DOMNodeProperties::Type>());
    auto properties = TRY(decoder.decode<JsonValue>());

    return WebView::DOMNodeProperties { type, move(properties) };
}
