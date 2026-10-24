/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Forward.h>
#include <LibWeb/SVG/SVGLengthValue.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::SVG {

// https://www.w3.org/TR/SVG11/types.html#InterfaceSVGLength
class SVGLength : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(SVGLength, Bindings::GCAllocatedWrappable);
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

    enum class ReflectedAttributeType : u8 {
        BaseValue,
        AnimatedValue,
    };

    enum class Directionality : u8 {
        Horizontal,
        Vertical,
        Unspecified,
    };

    [[nodiscard]] static GC::Ref<SVGLength> create_detached(JS::Realm&, SVGLengthValue value, ReadOnly);
    [[nodiscard]] static GC::Ref<SVGLength> create_reflected_attribute(JS::Realm&, GC::Ref<SVGElement> element, Utf16FlyString name, Directionality, ReflectedAttributeType, SVGLengthValue default_value, ReadOnly);

    virtual ~SVGLength() override;

    WebIDL::ExceptionOr<float> value() const;
    WebIDL::ExceptionOr<void> set_value(float value);

    u8 unit_type() const;

    float value_in_specified_units() const;
    WebIDL::ExceptionOr<void> set_value_in_specified_units(float value);

    Utf16String value_as_string() const;
    WebIDL::ExceptionOr<void> set_value_as_string(Utf16String);

    WebIDL::ExceptionOr<void> new_value_specified_units(u16 unit_type, float value_in_specified_units);
    WebIDL::ExceptionOr<void> convert_to_specified_units(u16 unit_type);

    ReadOnly read_only() const { return m_read_only; }

    // https://svgwg.org/svg2-draft/types.html#TermAttach
    void attach(GC::Ref<SVGLengthList>, GC::Ref<SVGElement>, Directionality, ReadOnly);

    // https://svgwg.org/svg2-draft/types.html#TermDetach
    void detach();

    bool is_detached() const { return !m_list && m_source.has<DetachedSource>(); }

    // Creates a detached, writable SVGLength that has the same value as this one.
    [[nodiscard]] GC::Ref<SVGLength> create_detached_copy() const;

    // Parses a value using the CSS syntax [ <number> | <length> | <percentage> ], if it parses to a scalar value.
    static Optional<SVGLengthValue> parse_scalar_value(Utf16View);

    // Sets the value when the list this length belongs to is synchronized with its reflected attribute.
    void set_value_from_reflected_attribute(SVGLengthValue);

private:
    // An SVGLength object can be associated with a particular element, as well as being designated with a
    // directionality: horizontal, vertical or unspecified.
    GC::Ptr<SVGElement> m_element;
    GC::Ref<JS::Realm> m_realm;
    Directionality m_directionality;

    // Every SVGLength object operates in one of four modes. It can:
    // 1. reflect the base value of a reflected animatable attribute (being exposed through the baseVal member of an
    //    SVGAnimatedLength),
    struct ReflectedAttributeSource {
        Utf16FlyString name;
        ReflectedAttributeType type;

        SVGLengthValue default_value;
    };

    // 2. reflect a presentation attribute value (such as by SVGRectElement.width.baseVal),
    // AD-HOC: This mode isn't used anywhere so we don't implement it - see https://github.com/w3c/svgwg/issues/1153

    // 3. reflect an element of the base value of a reflected animatable attribute (being exposed through the methods on
    //    the baseVal member of an SVGAnimatedLengthList), or
    // NB: Such a length holds its value like a detached one, and additionally has the list it is an element of.

    // 4. be detached, which is the case for SVGLength objects created with createSVGLength.
    struct DetachedSource {
        // Scalar values live here as plain data; non-scalar values (math functions, tree counting
        // functions) as their canonical serialization, reparsed on demand.
        Variant<SVGLengthValue, Utf16String> value;
    };

    using Source = Variant<ReflectedAttributeSource, DetachedSource>;

    SVGLength(JS::Realm&, GC::Ptr<SVGElement> associated_element, Directionality, Source&&, ReadOnly);

    virtual void visit_edges(Cell::Visitor&) override;

    // An SVGLength object maintains an internal <length> or <percentage> or <number> value, which is called its value.
    // NB: Non-scalar values surface as the style value they parse to.
    using InternalValue = Variant<SVGLengthValue, NonnullRefPtr<CSS::StyleValue const>>;
    InternalValue internal_value() const;

    Source m_source;

    // The list this length is an element of, if it reflects an element of the base value of a reflected attribute.
    GC::Ptr<SVGLengthList> m_list;

    // https://svgwg.org/svg2-draft/types.html#ReadOnlyLength
    ReadOnly m_read_only;
};

}
