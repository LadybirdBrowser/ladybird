/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/HTML/WebViewHints.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, ::Web::HTML::WebViewHints const& data_holder)
{
    TRY(encoder.encode(data_holder.popup));
    TRY(encoder.encode(data_holder.width));
    TRY(encoder.encode(data_holder.height));
    TRY(encoder.encode(data_holder.screen_x));
    TRY(encoder.encode(data_holder.screen_y));

    return {};
}

template<>
ErrorOr<::Web::HTML::WebViewHints> decode(Decoder& decoder)
{
    auto popup = TRY(decoder.decode<bool>());
    auto width = TRY(decoder.decode<Optional<Web::DevicePixels>>());
    auto height = TRY(decoder.decode<Optional<Web::DevicePixels>>());
    auto screen_x = TRY(decoder.decode<Optional<Web::DevicePixels>>());
    auto screen_y = TRY(decoder.decode<Optional<Web::DevicePixels>>());

    return ::Web::HTML::WebViewHints {
        .popup = popup,
        .width = width,
        .height = height,
        .screen_x = screen_x,
        .screen_y = screen_y,
    };
}

}
