/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebView/Attribute.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::Attribute const& attribute)
{
    TRY(encoder.encode(attribute.name));
    TRY(encoder.encode(attribute.value));
    return {};
}

template<>
ErrorOr<WebView::Attribute> IPC::decode(Decoder& decoder)
{
    auto name = TRY(decoder.decode<String>());
    auto value = TRY(decoder.decode<String>());

    return WebView::Attribute { move(name), move(value) };
}
