/*
 * Copyright (c) 2020, Srimanta Barua <srimanta.barua1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/OpenType/Glyf.h>
#include <LibGfx/Point.h>

namespace OpenType {

extern u16 be_u16(u8 const* ptr);
extern u32 be_u32(u8 const* ptr);

ErrorOr<Loca> Loca::from_slice(ReadonlyBytes slice, u32 num_glyphs, IndexToLocFormat index_to_loc_format)
{
    switch (index_to_loc_format) {
    case IndexToLocFormat::Offset16:
        if (slice.size() < num_glyphs * 2)
            return Error::from_string_literal("Could not load Loca: Not enough data");
        break;
    case IndexToLocFormat::Offset32:
        if (slice.size() < num_glyphs * 4)
            return Error::from_string_literal("Could not load Loca: Not enough data");
        break;
    }
    return Loca(slice, num_glyphs, index_to_loc_format);
}

u32 Loca::get_glyph_offset(u32 glyph_id) const
{
    // NOTE: The value of n is numGlyphs + 1.
    VERIFY(glyph_id <= m_num_glyphs);
    switch (m_index_to_loc_format) {
    case IndexToLocFormat::Offset16:
        return ((u32)be_u16(m_slice.offset(glyph_id * 2))) * 2;
    case IndexToLocFormat::Offset32:
        return be_u32(m_slice.offset(glyph_id * 4));
    default:
        VERIFY_NOT_REACHED();
    }
}

Optional<Glyf::Glyph> Glyf::glyph(u32 offset) const
{
    if (offset + sizeof(GlyphHeader) > m_slice.size())
        return {};
    VERIFY(m_slice.size() >= offset + sizeof(GlyphHeader));
    auto const& glyph_header = *bit_cast<GlyphHeader const*>(m_slice.offset(offset));
    i16 num_contours = glyph_header.number_of_contours;
    i16 xmin = glyph_header.x_min;
    i16 ymin = glyph_header.y_min;
    i16 xmax = glyph_header.x_max;
    i16 ymax = glyph_header.y_max;
    auto slice = m_slice.slice(offset + sizeof(GlyphHeader));
    return Glyph(slice, xmin, ymin, xmax, ymax, num_contours);
}

}
