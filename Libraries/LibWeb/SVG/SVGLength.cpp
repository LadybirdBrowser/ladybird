/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGLengthPrototype.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/SVG/SVGLength.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGLength);

GC::Ref<SVGLength> SVGLength::create(JS::Realm& realm, u8 unit_type, float value, ReadOnly read_only)
{
    return realm.create<SVGLength>(realm, unit_type, value, read_only);
}

GC::Ref<SVGLength> SVGLength::from_length_percentage(JS::Realm& realm, CSS::LengthPercentage const& length_percentage,
    ReadOnly read_only)
{
    // FIXME: We can't tell if a CSS::LengthPercentage was a unitless length.
    (void)SVG_LENGTHTYPE_NUMBER;
    if (length_percentage.is_percentage())
        return create(realm, SVG_LENGTHTYPE_PERCENTAGE, length_percentage.percentage().value(), read_only);
    if (length_percentage.is_length())
        return create(
            realm, [&] {
                switch (length_percentage.length().type()) {
                case CSS::Length::Type::Em:
                    return SVG_LENGTHTYPE_EMS;
                case CSS::Length::Type::Ex:
                    return SVG_LENGTHTYPE_EXS;
                case CSS::Length::Type::Px:
                    return SVG_LENGTHTYPE_PX;
                case CSS::Length::Type::Cm:
                    return SVG_LENGTHTYPE_CM;
                case CSS::Length::Type::Mm:
                    return SVG_LENGTHTYPE_MM;
                case CSS::Length::Type::In:
                    return SVG_LENGTHTYPE_IN;
                case CSS::Length::Type::Pt:
                    return SVG_LENGTHTYPE_PT;
                case CSS::Length::Type::Pc:
                    return SVG_LENGTHTYPE_PC;
                default:
                    return SVG_LENGTHTYPE_UNKNOWN;
                }
            }(),
            length_percentage.length().raw_value(), read_only);
    return create(realm, SVG_LENGTHTYPE_UNKNOWN, 0, read_only);
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
