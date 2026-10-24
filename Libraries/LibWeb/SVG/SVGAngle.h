/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Variant.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/CSS/Units.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGAngle
class SVGAngle final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(SVGAngle, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(SVGAngle);

public:
    static constexpr u16 SVG_ANGLETYPE_UNKNOWN = 0;
    static constexpr u16 SVG_ANGLETYPE_UNSPECIFIED = 1;
    static constexpr u16 SVG_ANGLETYPE_DEG = 2;
    static constexpr u16 SVG_ANGLETYPE_RAD = 3;
    static constexpr u16 SVG_ANGLETYPE_GRAD = 4;

    enum class ReadOnly : u8 {
        Yes,
        No,
    };

    // An <angle> or <number> value. A number has no unit.
    struct Value {
        float factor { 0 };
        Optional<CSS::AngleUnit> unit;
    };

    static Optional<Value> parse(Utf16View);
    static Utf16String serialize(Value);

    [[nodiscard]] static GC::Ref<SVGAngle> create_detached(Value, ReadOnly);
    [[nodiscard]] static GC::Ref<SVGAngle> create_reflected_attribute(GC::Ref<SVGElement>, Utf16FlyString name, ReadOnly);

    virtual ~SVGAngle() override;

    u16 unit_type() const;

    float value() const;
    WebIDL::ExceptionOr<void> set_value(float);

    float value_in_specified_units() const;
    WebIDL::ExceptionOr<void> set_value_in_specified_units(float);

    Utf16String value_as_string() const;
    WebIDL::ExceptionOr<void> set_value_as_string(Utf16String const&);

    WebIDL::ExceptionOr<void> new_value_specified_units(u16 unit_type, float value_in_specified_units);
    WebIDL::ExceptionOr<void> convert_to_specified_units(u16 unit_type);

    Value internal_value() const;

private:
    // Every SVGAngle object operates in one of two modes. It can:
    // 1. reflect the base value of a reflected animatable attribute (being exposed through the baseVal member of an
    //    SVGAnimatedAngle),
    struct ReflectedAttributeSource {
        GC::Ref<SVGElement> element;
        Utf16FlyString name;
    };

    // 2. be detached, which is the case for SVGAngle objects created with createSVGAngle.
    struct DetachedSource {
        Value value;
    };

    using Source = Variant<ReflectedAttributeSource, DetachedSource>;

    SVGAngle(Source, ReadOnly);

    virtual void visit_edges(Cell::Visitor&) override;

    WebIDL::ExceptionOr<void> ensure_not_read_only() const;
    void set_internal_value(Value);

    Source m_source;

    // https://svgwg.org/svg2-draft/types.html#ReadOnlyAngle
    ReadOnly m_read_only;
};

}
