/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG11/types.html#InterfaceSVGLength
class SVGLength : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(SVGLength, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(SVGLength);

public:
    // Same as SVGLength.idl
    static constexpr unsigned short SVG_LENGTHTYPE_UNKNOWN = 0;
    static constexpr unsigned short SVG_LENGTHTYPE_NUMBER = 1;
    static constexpr unsigned short SVG_LENGTHTYPE_PERCENTAGE = 2;
    static constexpr unsigned short SVG_LENGTHTYPE_EMS = 3;
    static constexpr unsigned short SVG_LENGTHTYPE_EXS = 4;
    static constexpr unsigned short SVG_LENGTHTYPE_PX = 5;
    static constexpr unsigned short SVG_LENGTHTYPE_CM = 6;
    static constexpr unsigned short SVG_LENGTHTYPE_MM = 7;
    static constexpr unsigned short SVG_LENGTHTYPE_IN = 8;
    static constexpr unsigned short SVG_LENGTHTYPE_PT = 9;
    static constexpr unsigned short SVG_LENGTHTYPE_PC = 10;

    enum class ReadOnly : u8 {
        Yes,
        No,
    };

    [[nodiscard]] static GC::Ref<SVGLength> create(JS::Realm&, u8 unit_type, float value, ReadOnly);
    virtual ~SVGLength() override;

    float value() const { return m_value; }
    WebIDL::ExceptionOr<void> set_value(float value);

    u8 unit_type() const { return m_unit_type; }
    ReadOnly read_only() const { return m_read_only; }

    [[nodiscard]] static GC::Ref<SVGLength> from_length_percentage(JS::Realm&, CSS::LengthPercentage const&, ReadOnly);

private:
    SVGLength(JS::Realm&, u8 unit_type, float value, ReadOnly);

    virtual void initialize(JS::Realm&) override;

    float m_value { 0 };
    u8 m_unit_type { 0 };

    // https://svgwg.org/svg2-draft/types.html#ReadOnlyLength
    ReadOnly m_read_only;
};

}
