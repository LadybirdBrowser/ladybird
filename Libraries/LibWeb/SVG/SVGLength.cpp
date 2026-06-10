/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/SVG/SVGLength.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGLength);

GC::Ref<SVGLength> SVGLength::create(u8 unit_type, float value, ReadOnly read_only)
{
    return GC::Heap::the().allocate<SVGLength>(unit_type, value, read_only);
}

GC::Ref<SVGLength> SVGLength::from_length_percentage(CSS::LengthPercentage const& length_percentage,
    ReadOnly read_only)
{
    // FIXME: We can't tell if a CSS::LengthPercentage was a unitless length.
    (void)SVG_LENGTHTYPE_NUMBER;
    if (length_percentage.is_percentage())
        return create(SVG_LENGTHTYPE_PERCENTAGE, length_percentage.percentage().value(), read_only);
    if (length_percentage.is_length())
        return create(
            [&] {
                switch (length_percentage.length().unit()) {
                case CSS::LengthUnit::Em:
                    return SVG_LENGTHTYPE_EMS;
                case CSS::LengthUnit::Ex:
                    return SVG_LENGTHTYPE_EXS;
                case CSS::LengthUnit::Px:
                    return SVG_LENGTHTYPE_PX;
                case CSS::LengthUnit::Cm:
                    return SVG_LENGTHTYPE_CM;
                case CSS::LengthUnit::Mm:
                    return SVG_LENGTHTYPE_MM;
                case CSS::LengthUnit::In:
                    return SVG_LENGTHTYPE_IN;
                case CSS::LengthUnit::Pt:
                    return SVG_LENGTHTYPE_PT;
                case CSS::LengthUnit::Pc:
                    return SVG_LENGTHTYPE_PC;
                default:
                    return SVG_LENGTHTYPE_UNKNOWN;
                }
            }(),
            length_percentage.length().raw_value(), read_only);
    return create(SVG_LENGTHTYPE_UNKNOWN, 0, read_only);
}

SVGLength::SVGLength(u8 unit_type, float value, ReadOnly read_only)
    : m_value(value)
    , m_unit_type(unit_type)
    , m_read_only(read_only)
{
}

SVGLength::~SVGLength() = default;

// https://svgwg.org/svg2-draft/types.html#__svg__SVGLength__value
WebIDL::ExceptionOr<void> SVGLength::set_value(float value)
{
    // 1. If the SVGLength object is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnly::Yes)
        return WebIDL::NoModificationAllowedError::create("Cannot modify value of read-only SVGLength"_utf16);

    // 2. Let value be the value being assigned to value.
    // 3. Set the SVGLength's value to a <number> whose value is value.
    set_value_without_readonly_check(value);

    // FIXME: 4. If the SVGLength reflects the base value of a reflected attribute, reflects a presentation attribute, or
    //    reflects an element of the base value of a reflected attribute, then reserialize the reflected attribute.

    return {};
}

void SVGLength::set_value_without_readonly_check(float value)
{
    m_value = value;
    m_unit_type = SVG_LENGTHTYPE_NUMBER;
}

}
