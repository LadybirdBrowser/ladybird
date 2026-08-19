/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <LibGfx/Path.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/SVG/AttributeParser.h>

namespace Web::Painting {

extern "C" void* ladybird_web_svg_path_from_path_data_ascii(u8 const*, size_t);
extern "C" void* ladybird_web_svg_path_from_path_data_utf16(char16_t const*, size_t);

extern "C" void* ladybird_web_svg_path_from_path_data_ascii(u8 const* bytes, size_t length)
{
    auto path_data = SVG::AttributeParser::parse_path_data(Utf16View { StringView { bytes, length } });
    return new Gfx::Path(path_data.to_gfx_path());
}

extern "C" void* ladybird_web_svg_path_from_path_data_utf16(char16_t const* units, size_t length)
{
    auto path_data = SVG::AttributeParser::parse_path_data(Utf16View { units, length });
    return new Gfx::Path(path_data.to_gfx_path());
}

}
