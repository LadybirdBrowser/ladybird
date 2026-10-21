/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web {

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::CSSPixelPoint const& value)
{
    TRY(encoder.encode(value.x().raw_value()));
    TRY(encoder.encode(value.y().raw_value()));
    return {};
}

template<>
ErrorOr<Web::CSSPixelPoint> decode(Decoder& decoder)
{
    auto x = TRY(decoder.decode<i32>());
    auto y = TRY(decoder.decode<i32>());
    return Web::CSSPixelPoint { Web::CSSPixels::from_raw(x), Web::CSSPixels::from_raw(y) };
}

}
