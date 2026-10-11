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
ErrorOr<void> encode(Encoder& encoder, ::Web::HTML::PictureInPictureWindowHints const& hints)
{
    TRY(encoder.encode(hints.navigable_id));
    TRY(encoder.encode(hints.video_size));
    return {};
}

template<>
ErrorOr<::Web::HTML::PictureInPictureWindowHints> decode(Decoder& decoder)
{
    auto navigable_id = TRY(decoder.decode<Web::HTML::CrossProcessId>());
    auto video_size = TRY(decoder.decode<Gfx::IntSize>());
    return ::Web::HTML::PictureInPictureWindowHints { navigable_id, video_size };
}

template<>
ErrorOr<void> encode(Encoder& encoder, ::Web::HTML::WebViewHints const& data_holder)
{
    TRY(encoder.encode(data_holder.popup));
    TRY(encoder.encode(data_holder.picture_in_picture_window));
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
    auto picture_in_picture_window = TRY(decoder.decode<Optional<Web::HTML::PictureInPictureWindowHints>>());
    auto width = TRY(decoder.decode<Optional<Web::DevicePixels>>());
    auto height = TRY(decoder.decode<Optional<Web::DevicePixels>>());
    auto screen_x = TRY(decoder.decode<Optional<Web::DevicePixels>>());
    auto screen_y = TRY(decoder.decode<Optional<Web::DevicePixels>>());

    return ::Web::HTML::WebViewHints {
        .popup = popup,
        .picture_in_picture_window = picture_in_picture_window,
        .width = width,
        .height = height,
        .screen_x = screen_x,
        .screen_y = screen_y,
    };
}

}
