/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGLength.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/SVG/SVGLength.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGLength);

GC::Ref<SVGLength> SVGLength::create(JS::Realm& realm, u8 unit_type, float value, ReadOnly read_only)
{
    return realm.create<SVGLength>(realm, unit_type, value, read_only);
}

SVGLength::ParsedValue SVGLength::parsed_value_from_style_value(CSS::StyleValue const& style_value)
{
    if (style_value.is_number())
        return { SVGLength::SVG_LENGTHTYPE_NUMBER, static_cast<float>(style_value.as_number().number()) };

    if (style_value.is_percentage())
        return { SVGLength::SVG_LENGTHTYPE_PERCENTAGE, static_cast<float>(style_value.as_percentage().percentage().value()) };

    if (style_value.is_length()) {
        auto length = style_value.as_length().length();
        auto unit_type = [&] {
            switch (length.unit()) {
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
        }();
        return { static_cast<u8>(unit_type), static_cast<float>(length.raw_value()) };
    }

    // FIXME: Implement the proper spec algorithms for SVGLength getters so that we support non-scalar values
    if (style_value.is_tree_counting_function())
        return { SVG_LENGTHTYPE_UNKNOWN, 0 };

    if (style_value.is_calculated())
        return { SVG_LENGTHTYPE_UNKNOWN, 0 };

    VERIFY_NOT_REACHED();
}

SVGLength::SVGLength(JS::Realm& realm, u8 unit_type, float value, ReadOnly read_only)
    : PlatformObject(realm)
    , m_value(value)
    , m_unit_type(unit_type)
    , m_read_only(read_only)
{
}

void SVGLength::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGLength);
    Base::initialize(realm);
}

SVGLength::~SVGLength() = default;

// https://svgwg.org/svg2-draft/types.html#__svg__SVGLength__value
WebIDL::ExceptionOr<void> SVGLength::set_value(float value)
{
    // 1. If the SVGLength object is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnly::Yes)
        return WebIDL::NoModificationAllowedError::create(realm(), "Cannot modify value of read-only SVGLength"_utf16);

    // 2. Let value be the value being assigned to value.
    // 3. Set the SVGLength's value to a <number> whose value is value.
    m_value = value;
    m_unit_type = SVG_LENGTHTYPE_NUMBER;

    // FIXME: 4. If the SVGLength reflects the base value of a reflected attribute, reflects a presentation attribute, or
    //    reflects an element of the base value of a reflected attribute, then reserialize the reflected attribute.

    return {};
}

}
