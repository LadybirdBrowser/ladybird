/*
 * Copyright (c) 2020, Srimanta Barua <srimanta.barua1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/AntiAliasingPainter.h>
#include <LibGfx/DeprecatedPainter.h>
#include <LibGfx/Font/OpenType/Glyf.h>
#include <LibGfx/Path.h>
#include <LibGfx/Point.h>

namespace OpenType {

extern u16 be_u16(u8 const* ptr);
extern u32 be_u32(u8 const* ptr);
extern i16 be_i16(u8 const* ptr);
extern float be_fword(u8 const* ptr);

enum class SimpleGlyfFlags {
    // From spec.
    OnCurve = 0x01,
    XShortVector = 0x02,
    YShortVector = 0x04,
    RepeatFlag = 0x08,
    XIsSameOrPositiveXShortVector = 0x10,
    YIsSameOrPositiveYShortVector = 0x20,
    // Combinations
    XMask = 0x12,
    YMask = 0x24,
    XLongVector = 0x00,
    YLongVector = 0x00,
    XNegativeShortVector = 0x02,
    YNegativeShortVector = 0x04,
    XPositiveShortVector = 0x12,
    YPositiveShortVector = 0x24,
};

class PointIterator {
public:
    struct Item {
        bool on_curve;
        Gfx::FloatPoint point;
    };

    PointIterator(ReadonlyBytes slice, u16 num_points, u32 flags_offset, u32 x_offset, u32 y_offset, Gfx::AffineTransform affine)
        : m_slice(slice)
        , m_points_remaining(num_points)
        , m_flags_offset(flags_offset)
        , m_x_offset(x_offset)
        , m_y_offset(y_offset)
        , m_affine(affine)
    {
    }

    Optional<Item> next()
    {
        if (m_points_remaining == 0) {
            return {};
        }
        if (m_flags_remaining > 0) {
            m_flags_remaining--;
        } else {
            m_flag = m_slice[m_flags_offset++];
            if (m_flag & (u8)SimpleGlyfFlags::RepeatFlag) {
                m_flags_remaining = m_slice[m_flags_offset++];
            }
        }
        switch (m_flag & (u8)SimpleGlyfFlags::XMask) {
        case (u8)SimpleGlyfFlags::XLongVector:
            m_last_point.set_x(m_last_point.x() + be_i16(m_slice.offset(m_x_offset)));
            m_x_offset += 2;
            break;
        case (u8)SimpleGlyfFlags::XNegativeShortVector:
            m_last_point.set_x(m_last_point.x() - m_slice[m_x_offset++]);
            break;
        case (u8)SimpleGlyfFlags::XPositiveShortVector:
            m_last_point.set_x(m_last_point.x() + m_slice[m_x_offset++]);
            break;
        default:
            break;
        }
        switch (m_flag & (u8)SimpleGlyfFlags::YMask) {
        case (u8)SimpleGlyfFlags::YLongVector:
            m_last_point.set_y(m_last_point.y() + be_i16(m_slice.offset(m_y_offset)));
            m_y_offset += 2;
            break;
        case (u8)SimpleGlyfFlags::YNegativeShortVector:
            m_last_point.set_y(m_last_point.y() - m_slice[m_y_offset++]);
            break;
        case (u8)SimpleGlyfFlags::YPositiveShortVector:
            m_last_point.set_y(m_last_point.y() + m_slice[m_y_offset++]);
            break;
        default:
            break;
        }
        m_points_remaining--;
        Item ret = {
            .on_curve = (m_flag & (u8)SimpleGlyfFlags::OnCurve) != 0,
            .point = m_affine.map(m_last_point),
        };
        return ret;
    }

private:
    ReadonlyBytes m_slice;
    u16 m_points_remaining;
    u8 m_flag { 0 };
    Gfx::FloatPoint m_last_point = { 0.0f, 0.0f };
    u32 m_flags_remaining = { 0 };
    u32 m_flags_offset;
    u32 m_x_offset;
    u32 m_y_offset;
    Gfx::AffineTransform m_affine;
};

Optional<Glyf::Glyph::ComponentIterator::Item> Glyf::Glyph::ComponentIterator::next()
{
    if (!m_has_more) {
        return {};
    }
    u16 flags = be_u16(m_slice.offset(m_offset));
    m_offset += 2;
    u16 glyph_id = be_u16(m_slice.offset(m_offset));
    m_offset += 2;
    i16 arg1 = 0, arg2 = 0;
    if (flags & (u16)CompositeFlags::Arg1AndArg2AreWords) {
        arg1 = be_i16(m_slice.offset(m_offset));
        m_offset += 2;
        arg2 = be_i16(m_slice.offset(m_offset));
        m_offset += 2;
    } else {
        arg1 = (i8)m_slice[m_offset++];
        arg2 = (i8)m_slice[m_offset++];
    }
    float a = 1.0, b = 0.0, c = 0.0, d = 1.0, e = 0.0, f = 0.0;
    if (flags & (u16)CompositeFlags::WeHaveATwoByTwo) {
        a = be_fword(m_slice.offset(m_offset));
        m_offset += 2;
        b = be_fword(m_slice.offset(m_offset));
        m_offset += 2;
        c = be_fword(m_slice.offset(m_offset));
        m_offset += 2;
        d = be_fword(m_slice.offset(m_offset));
        m_offset += 2;
    } else if (flags & (u16)CompositeFlags::WeHaveAnXAndYScale) {
        a = be_fword(m_slice.offset(m_offset));
        m_offset += 2;
        d = be_fword(m_slice.offset(m_offset));
        m_offset += 2;
    } else if (flags & (u16)CompositeFlags::WeHaveAScale) {
        a = be_fword(m_slice.offset(m_offset));
        m_offset += 2;
        d = a;
    }
    // FIXME: Handle UseMyMetrics, ScaledComponentOffset, UnscaledComponentOffset, non-ArgsAreXYValues
    if (flags & (u16)CompositeFlags::ArgsAreXYValues) {
        e = arg1;
        f = arg2;
    } else {
        // FIXME: Implement this. There's no TODO() here since many fonts work just fine without this.
    }
    if (flags & (u16)CompositeFlags::UseMyMetrics) {
        // FIXME: Implement this. There's no TODO() here since many fonts work just fine without this.
    }
    if (flags & (u16)CompositeFlags::ScaledComponentOffset) {
        // FIXME: Implement this. There's no TODO() here since many fonts work just fine without this.
    }
    if (flags & (u16)CompositeFlags::UnscaledComponentOffset) {
        // FIXME: Implement this. There's no TODO() here since many fonts work just fine without this.
    }
    m_has_more = (flags & (u16)CompositeFlags::MoreComponents);
    return Item {
        .glyph_id = glyph_id,
        .affine = Gfx::AffineTransform(a, b, c, d, e, f),
    };
}

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

static void get_ttglyph_offsets(ReadonlyBytes slice, u32 num_points, u32 flags_offset, u32* x_offset, u32* y_offset)
{
    u32 flags_size = 0;
    u32 x_size = 0;
    u32 repeat_count;
    while (num_points > 0) {
        u8 flag = slice[flags_offset + flags_size];
        if (flag & (u8)SimpleGlyfFlags::RepeatFlag) {
            flags_size++;
            repeat_count = slice[flags_offset + flags_size] + 1;
        } else {
            repeat_count = 1;
        }
        flags_size++;
        switch (flag & (u8)SimpleGlyfFlags::XMask) {
        case (u8)SimpleGlyfFlags::XLongVector:
            x_size += repeat_count * 2;
            break;
        case (u8)SimpleGlyfFlags::XNegativeShortVector:
        case (u8)SimpleGlyfFlags::XPositiveShortVector:
            x_size += repeat_count;
            break;
        default:
            break;
        }
        num_points -= repeat_count;
    }
    *x_offset = flags_offset + flags_size;
    *y_offset = *x_offset + x_size;
}

ReadonlyBytes Glyf::Glyph::program() const
{
    if (m_num_contours == 0)
        return {};

    auto instructions_start = m_num_contours * 2;
    u16 num_instructions = be_u16(m_slice.offset(instructions_start));
    return m_slice.slice(instructions_start + 2, num_instructions);
}

void Glyf::Glyph::append_path_impl(Gfx::Path& path, Gfx::AffineTransform const& transform) const
{
    if (m_num_contours == 0)
        return;

    // Get offset for flags, x, and y.
    u16 num_points = be_u16(m_slice.offset((m_num_contours - 1) * 2)) + 1;
    u16 num_instructions = be_u16(m_slice.offset(m_num_contours * 2));
    u32 flags_offset = m_num_contours * 2 + 2 + num_instructions;
    u32 x_offset = 0;
    u32 y_offset = 0;
    get_ttglyph_offsets(m_slice, num_points, flags_offset, &x_offset, &y_offset);

    // Prepare to render glyph.
    PointIterator point_iterator(m_slice, num_points, flags_offset, x_offset, y_offset, transform);

    u32 current_point_index = 0;
    for (u16 contour_index = 0; contour_index < m_num_contours; contour_index++) {
        u32 current_contour_last_point_index = be_u16(m_slice.offset(contour_index * 2));

        Vector<PointIterator::Item> points;
        while (current_point_index <= current_contour_last_point_index) {
            points.append(*point_iterator.next());
            current_point_index++;
        }

        if (points.is_empty())
            continue;

        auto current = points.last();
        auto next = points.first();

        if (current.on_curve) {
            path.move_to(current.point);
        } else if (next.on_curve) {
            path.move_to(next.point);
        } else {
            auto implied_point = (current.point + next.point) * 0.5f;
            path.move_to(implied_point);
        }

        for (size_t i = 0; i < points.size(); i++) {
            current = next;
            next = points[(i + 1) % points.size()];
            if (current.on_curve) {
                path.line_to(current.point);
            } else if (next.on_curve) {
                path.quadratic_bezier_curve_to(current.point, next.point);
            } else {
                auto implied_point = (current.point + next.point) * 0.5f;
                path.quadratic_bezier_curve_to(current.point, implied_point);
            }
        }
    }
}

bool Glyf::Glyph::append_simple_path(Gfx::Path& path, i16 font_ascender, i16 font_descender, float x_scale, float y_scale) const
{
    if (m_xmin > m_xmax) [[unlikely]] {
        dbgln("OpenType: Glyph has invalid xMin ({}) > xMax ({})", m_xmin, m_xmax);
        return false;
    }
    if (font_descender > font_ascender) [[unlikely]] {
        dbgln("OpenType: Glyph has invalid ascender ({}) > descender ({})", font_ascender, font_descender);
        return false;
    }
    auto affine = Gfx::AffineTransform()
                      .translate(path.last_point())
                      .scale(x_scale, -y_scale)
                      .translate(-m_xmin, -font_ascender);
    append_path_impl(path, affine);
    return true;
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
