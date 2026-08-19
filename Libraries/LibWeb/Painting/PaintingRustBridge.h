/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

inline Layout::RustFFI::FfiCssPixelRect to_ffi_css_pixel_rect(CSSPixelRect const& rect)
{
    return { rect.x().raw_value(), rect.y().raw_value(), rect.width().raw_value(), rect.height().raw_value() };
}

inline CSSPixelRect from_ffi_css_pixel_rect(Layout::RustFFI::FfiCssPixelRect const& rect)
{
    return { CSSPixels::from_raw(rect.x), CSSPixels::from_raw(rect.y), CSSPixels::from_raw(rect.width), CSSPixels::from_raw(rect.height) };
}

}
