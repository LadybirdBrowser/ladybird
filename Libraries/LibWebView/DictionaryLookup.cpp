/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebView/DictionaryLookup.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::DictionaryLookupTextStyle const& style)
{
    TRY(encoder.encode(style.font_family));
    TRY(encoder.encode(style.ui_point_size));
    TRY(encoder.encode(style.weight));
    TRY(encoder.encode(style.slope));
    return {};
}

template<>
ErrorOr<WebView::DictionaryLookupTextStyle> IPC::decode(Decoder& decoder)
{
    auto font_family = TRY(decoder.decode<String>());
    auto ui_point_size = TRY(decoder.decode<float>());
    auto weight = TRY(decoder.decode<u16>());
    auto slope = TRY(decoder.decode<u8>());

    return WebView::DictionaryLookupTextStyle { move(font_family), ui_point_size, weight, slope };
}

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, WebView::DictionaryLookup const& lookup)
{
    TRY(encoder.encode(lookup.text));
    TRY(encoder.encode(lookup.style));
    TRY(encoder.encode(lookup.baseline_origin));
    return {};
}

template<>
ErrorOr<WebView::DictionaryLookup> IPC::decode(Decoder& decoder)
{
    auto text = TRY(decoder.decode<String>());
    auto style = TRY(decoder.decode<Optional<WebView::DictionaryLookupTextStyle>>());
    auto baseline_origin = TRY(decoder.decode<Optional<Gfx::IntPoint>>());

    return WebView::DictionaryLookup { move(text), move(style), move(baseline_origin) };
}
