/*
 * Copyright (c) 2026, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/HTML/PrintSettings.h>

template<>
ErrorOr<void> IPC::encode(Encoder& encoder, Web::HTML::PrintSettings const& settings)
{
    TRY(encoder.encode(settings.paper_width_mm));
    TRY(encoder.encode(settings.paper_height_mm));
    TRY(encoder.encode(settings.margin_top_mm));
    TRY(encoder.encode(settings.margin_right_mm));
    TRY(encoder.encode(settings.margin_bottom_mm));
    TRY(encoder.encode(settings.margin_left_mm));
    return {};
}

template<>
ErrorOr<Web::HTML::PrintSettings> IPC::decode(Decoder& decoder)
{
    auto paper_width_mm = TRY(decoder.decode<float>());
    auto paper_height_mm = TRY(decoder.decode<float>());
    auto margin_top_mm = TRY(decoder.decode<float>());
    auto margin_right_mm = TRY(decoder.decode<float>());
    auto margin_bottom_mm = TRY(decoder.decode<float>());
    auto margin_left_mm = TRY(decoder.decode<float>());

    return Web::HTML::PrintSettings {
        paper_width_mm,
        paper_height_mm,
        margin_top_mm,
        margin_right_mm,
        margin_bottom_mm,
        margin_left_mm,
    };
}
