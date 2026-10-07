/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Utf16View.h>
#include <LibGfx/Font/FaceDescription.h>
#include <LibGfx/Font/FontTable.h>

#include <harfbuzz/hb-ot.h>
#include <math.h>

namespace Gfx {

static constexpr u8 upright_slope = 0;
static constexpr u8 italic_slope = 1;
static constexpr u8 oblique_slope = 2;

// https://learn.microsoft.com/en-us/typography/opentype/spec/name#platform-ids
// HarfBuzz looks names up by their language, and a record of the Unicode platform has no language, so read such a
// record here. Its string is UTF-16BE.
static Optional<String> unicode_platform_name(hb_face_t* face, u16 name_id)
{
    FontTable table { face, FourCC { "name" } };
    if (!table.contains(0, 6))
        return {};
    size_t record_count = table.u16_at(2);
    size_t storage_offset = table.u16_at(4);
    for (size_t index = 0; index < record_count && table.contains(6 + 12 * index, 12); ++index) {
        auto record = 6 + 12 * index;
        if (table.u16_at(record) != 0 || table.u16_at(record + 6) != name_id)
            continue;
        size_t length = table.u16_at(record + 8);
        size_t offset = storage_offset + table.u16_at(record + 10);
        if (length == 0 || !table.contains(offset, length))
            continue;
        Vector<char16_t> code_units;
        code_units.ensure_capacity(length / 2);
        // Some fonts end a name with U+0000.
        for (size_t unit = 0; unit + 1 < length && table.u16_at(offset + unit) != 0; unit += 2)
            code_units.unchecked_append(static_cast<char16_t>(table.u16_at(offset + unit)));
        auto name = String::from_utf16_with_replacement_character(Utf16View { code_units.data(), code_units.size() });
        if (!name.is_error())
            return name.release_value();
    }
    return {};
}

// https://learn.microsoft.com/en-us/typography/opentype/spec/name#name-ids
static Optional<String> read_family(hb_face_t* face)
{
    // A face has a WWS family name only if its typographic family also differs in styles other than weight, width and
    // slope, such as optical size. CSS tells the faces of a family apart only by weight, width and slope, so such a
    // face is in a family of its own.
    for (auto name_id : { HB_OT_NAME_ID_WWS_FAMILY, HB_OT_NAME_ID_TYPOGRAPHIC_FAMILY, HB_OT_NAME_ID_FONT_FAMILY }) {
        auto name = face_name(face, name_id);
        if (!name.has_value())
            name = unicode_platform_name(face, name_id);
        if (name.has_value() && !name->is_empty())
            return name;
    }
    return {};
}

// https://learn.microsoft.com/en-us/typography/opentype/spec/os2#fsselection
static constexpr u16 italic_selection_bit = 1 << 0;
static constexpr u16 oblique_selection_bit = 1 << 9;

// https://learn.microsoft.com/en-us/typography/opentype/spec/head
static constexpr u16 bold_mac_style_bit = 1 << 0;
static constexpr u16 italic_mac_style_bit = 1 << 1;

static FaceStyle read_declared_style(hb_face_t* face)
{
    FaceStyle style;

    u16 mac_style = 0;
    if (FontTable head { face, FourCC { "head" } }; head.contains(44, 2))
        mac_style = head.u16_at(44);

    // https://learn.microsoft.com/en-us/typography/opentype/spec/os2
    FontTable os2 { face, FourCC { "OS/2" } };
    if (os2.contains(0, 64)) {
        style.weight = os2.u16_at(4);
        style.width = os2.u16_at(6);
        auto selection = os2.u16_at(62);
        if (selection & oblique_selection_bit)
            style.slope = oblique_slope;
        else if (selection & italic_selection_bit)
            style.slope = italic_slope;
    } else if (mac_style & bold_mac_style_bit) {
        style.weight = 700;
    }
    if (style.slope == upright_slope && (mac_style & italic_mac_style_bit))
        style.slope = italic_slope;
    return style;
}

// The ascender, the descender and the x-height at the default coordinates. A font adds the deltas of its variations.
static FaceVerticalMetrics read_vertical_metrics(hb_face_t* face)
{
    // A new font has the scale of the face, so positions are in font units.
    auto* font = hb_font_create(face);
    hb_font_set_var_coords_normalized(font, nullptr, 0);

    FaceVerticalMetrics metrics;
    hb_position_t position = 0;
    hb_ot_metrics_get_position_with_fallback(font, HB_OT_METRICS_TAG_HORIZONTAL_ASCENDER, &position);
    metrics.ascender = position;
    hb_ot_metrics_get_position_with_fallback(font, HB_OT_METRICS_TAG_HORIZONTAL_DESCENDER, &position);
    metrics.descender = position;
    if (hb_ot_metrics_get_position(font, HB_OT_METRICS_TAG_X_HEIGHT, &position))
        metrics.x_height = position;

    hb_font_destroy(font);
    return metrics;
}

FaceDescription FaceDescription::read(hb_face_t* face)
{
    FaceDescription description;
    if (auto family = read_family(face); family.has_value())
        description.m_family = FlyString { family.release_value() };
    description.m_declared_style = read_declared_style(face);
    description.m_vertical_metrics = read_vertical_metrics(face);

    // https://learn.microsoft.com/en-us/typography/opentype/spec/fvar
    auto axis_count = hb_ot_var_get_axis_count(face);
    if (axis_count > 0) {
        Vector<hb_ot_var_axis_info_t> axes;
        axes.resize(axis_count);
        hb_ot_var_get_axis_infos(face, 0, &axis_count, axes.data());

        // The upper 16 bits of the face index select a named instance, counting from 1, and the face starts at its
        // coordinates. An index that no instance has selects the default coordinates.
        Vector<float> instance_coordinates;
        if (auto instance = hb_face_get_index(face) >> 16; instance > 0) {
            instance_coordinates.resize(axis_count);
            auto coordinate_count = axis_count;
            if (hb_ot_var_named_instance_get_design_coords(face, instance - 1, &coordinate_count, instance_coordinates.data()) != axis_count)
                instance_coordinates.clear();
        }

        for (size_t index = 0; index < axis_count; ++index) {
            auto const& axis = axes[index];
            auto face_value = instance_coordinates.is_empty() ? axis.default_value : instance_coordinates[index];
            description.m_axes.append({
                .tag = axis.tag,
                .minimum = axis.min_value,
                .face_value = clamp(face_value, axis.min_value, axis.max_value),
                .maximum = axis.max_value,
            });
        }
    }

    description.m_style = description.style_for_variations({});
    return description;
}

// https://learn.microsoft.com/en-us/typography/opentype/spec/os2#uswidthclass
static u16 width_class_for_percentage(float percentage)
{
    static constexpr Array<float, 9> class_percentages { 50, 62.5f, 75, 87.5f, 100, 112.5f, 125, 150, 200 };
    size_t nearest = 0;
    for (size_t index = 1; index < class_percentages.size(); ++index) {
        if (fabsf(class_percentages[index] - percentage) < fabsf(class_percentages[nearest] - percentage))
            nearest = index;
    }
    return nearest + 1;
}

FaceStyle FaceDescription::style_for_variations(ReadonlySpan<FontVariationAxis> variations) const
{
    int weight = m_declared_style.weight;
    int width = m_declared_style.width;
    Optional<float> italic;
    Optional<float> slant;
    for (auto const& axis : m_axes) {
        // The last variation of an axis applies.
        auto value = axis.face_value;
        for (size_t index = variations.size(); index-- > 0;) {
            if (variations[index].tag.to_u32() == axis.tag) {
                value = clamp(variations[index].value, axis.minimum, axis.maximum);
                break;
            }
        }

        if (axis.tag == HB_OT_TAG_VAR_AXIS_WEIGHT)
            weight = static_cast<int>(roundf(value));
        else if (axis.tag == HB_OT_TAG_VAR_AXIS_WIDTH)
            width = width_class_for_percentage(value);
        else if (axis.tag == HB_OT_TAG_VAR_AXIS_ITALIC)
            italic = value;
        else if (axis.tag == HB_OT_TAG_VAR_AXIS_SLANT)
            slant = value;
    }

    // https://learn.microsoft.com/en-us/typography/opentype/spec/dvaraxisreg
    // An ital or slnt axis decides whether the face is italic or oblique, in place of the tables.
    u8 slope = upright_slope;
    if (italic.has_value() ? *italic >= 0.5f : m_declared_style.slope == italic_slope)
        slope = italic_slope;
    else if (slant.has_value() ? *slant != 0 : m_declared_style.slope == oblique_slope)
        slope = oblique_slope;

    return {
        // CSS font weights start at 1, but some fonts give 0.
        .weight = static_cast<u16>(clamp(weight, 1, 1000)),
        .width = static_cast<u16>(clamp(width, 1, 9)),
        .slope = slope,
    };
}

}
