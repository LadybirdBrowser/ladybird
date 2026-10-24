/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16StringBuilder.h>
#include <LibGC/Heap.h>
#include <LibWeb/CSS/Angle.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/SVG/SVGAngle.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGAngle);

GC::Ref<SVGAngle> SVGAngle::create_detached(Value value, ReadOnly read_only)
{
    return GC::Heap::the().allocate<SVGAngle>(DetachedSource { value }, read_only);
}

GC::Ref<SVGAngle> SVGAngle::create_reflected_attribute(GC::Ref<SVGElement> element, Utf16FlyString name, ReadOnly read_only)
{
    return GC::Heap::the().allocate<SVGAngle>(ReflectedAttributeSource { element, move(name) }, read_only);
}

SVGAngle::SVGAngle(Source source, ReadOnly read_only)
    : m_source(move(source))
    , m_read_only(read_only)
{
}

SVGAngle::~SVGAngle() = default;

void SVGAngle::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (auto* source = m_source.get_pointer<ReflectedAttributeSource>())
        visitor.visit(source->element);
}

// Parses a value using the CSS syntax [ <number> | <angle> ].
Optional<SVGAngle::Value> SVGAngle::parse(Utf16View input)
{
    CSS::Parser::ParsingParams parsing_params { CSS::Parser::ParsingMode::SVGPresentationAttribute };
    if (auto number = parse_css_type(parsing_params, input, CSS::ValueType::Number); number && number->is_number())
        return Value { static_cast<float>(number->as_number().number()), {} };
    // FIXME: Support math functions, which do not parse to a literal number or angle.
    if (auto angle = parse_css_type(parsing_params, input, CSS::ValueType::Angle); angle && angle->is_angle()) {
        auto value = angle->as_angle().angle();
        return Value { static_cast<float>(value.raw_value()), value.unit() };
    }
    return {};
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAngle__valueAsString
Utf16String SVGAngle::serialize(Value value)
{
    // 2. Let string be an empty string.
    Utf16StringBuilder builder;

    // 3. Let factor be value's numeric factor, if it is an <angle>, or value itself it is a <number>.
    // 4. Append to string an implementation specific string that, if parsed as a <number> using CSS syntax, would
    //    return the number value closest to factor, given the implementation's supported real number precision.
    CSS::serialize_a_number(builder, value.factor);

    // 5. If value is an <angle>, then append to string the canonical spelling of value's unit.
    if (value.unit.has_value())
        builder.append(CSS::to_string(*value.unit));

    // 6. Return string.
    return builder.to_string();
}

SVGAngle::Value SVGAngle::internal_value() const
{
    return m_source.visit(
        [](ReflectedAttributeSource const& source) {
            // NB: The value of a reflected attribute that is not present or invalid is its initial value. The only
            //     attribute reflected by an SVGAngle is orient, whose initial value is 0.
            return parse(source.element->get_attribute_value(source.name)).value_or({});
        },
        [](DetachedSource const& source) { return source.value; });
}

void SVGAngle::set_internal_value(Value value)
{
    m_source.visit(
        [&](ReflectedAttributeSource const& source) {
            // https://svgwg.org/svg2-draft/types.html#TermReserialize
            source.element->set_attribute_value(source.name, serialize(value));
        },
        [&](DetachedSource& source) { source.value = value; });
}

WebIDL::ExceptionOr<void> SVGAngle::ensure_not_read_only() const
{
    if (m_read_only == ReadOnly::Yes)
        return WebIDL::NoModificationAllowedError::create("Cannot modify a read-only SVGAngle"_utf16);
    return {};
}

static Optional<CSS::AngleUnit> angle_unit_for_unit_type(u16 unit_type)
{
    switch (unit_type) {
    case SVGAngle::SVG_ANGLETYPE_DEG:
        return CSS::AngleUnit::Deg;
    case SVGAngle::SVG_ANGLETYPE_RAD:
        return CSS::AngleUnit::Rad;
    case SVGAngle::SVG_ANGLETYPE_GRAD:
        return CSS::AngleUnit::Grad;
    default:
        return {};
    }
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAngle__unitType
u16 SVGAngle::unit_type() const
{
    // 1. If the SVGAngle's value is a unitless <number> or a <length> with a deg, rad or grad unit, then return the
    //    corresponding constant value from the angle unit type table above.
    auto value = internal_value();
    if (!value.unit.has_value())
        return SVG_ANGLETYPE_UNSPECIFIED;
    switch (*value.unit) {
    case CSS::AngleUnit::Deg:
        return SVG_ANGLETYPE_DEG;
    case CSS::AngleUnit::Rad:
        return SVG_ANGLETYPE_RAD;
    case CSS::AngleUnit::Grad:
        return SVG_ANGLETYPE_GRAD;
    default:
        // 2. Otherwise, return SVG_ANGLETYPE_UNKNOWN.
        return SVG_ANGLETYPE_UNKNOWN;
    }
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAngle__value
float SVGAngle::value() const
{
    // 1. Let value be the SVGAngle's value.
    auto value = internal_value();

    // 2. If value is a <number>, return that number.
    if (!value.unit.has_value())
        return value.factor;

    // 3. Return the result of converting value to an angle in degrees.
    return static_cast<float>(CSS::Angle { value.factor, *value.unit }.to_degrees());
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAngle__value
WebIDL::ExceptionOr<void> SVGAngle::set_value(float value)
{
    // 1. If the SVGAngle object is read only, then throw a NoModificationAllowedError.
    TRY(ensure_not_read_only());

    // 2. Let value be the value being assigned to value.
    // 3. Set the SVGAngle's value to a <number> whose value is value.
    // 4. If the SVGAngle reflects the base value of a reflected attribute, then reserialize the reflected attribute.
    set_internal_value({ value, {} });
    return {};
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAngle__valueInSpecifiedUnits
float SVGAngle::value_in_specified_units() const
{
    // 1. Let value be the SVGAngle's value.
    // 2. If value is a <number>, return that number.
    // 3. Otherwise, value is an <angle> value. Return the numeric factor before its unit.
    return internal_value().factor;
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAngle__valueInSpecifiedUnits
WebIDL::ExceptionOr<void> SVGAngle::set_value_in_specified_units(float value)
{
    // 1. If the SVGAngle object is read only, then throw a NoModificationAllowedError.
    TRY(ensure_not_read_only());

    // 2. Let value be the value being assigned to valueInSpecifiedUnits.
    // 3. If the SVGAngle's value is a <number>, then update its value to value.
    // 4. Otherwise, if the SVGAngle's value is an <angle>, then update its numeric factor to value.
    auto new_value = internal_value();
    new_value.factor = value;

    // 5. If the SVGAngle reflects the base value of a reflected attribute or reflects an element of the base value of
    //    a reflected attribute, then reserialize the reflected attribute.
    set_internal_value(new_value);
    return {};
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAngle__valueAsString
Utf16String SVGAngle::value_as_string() const
{
    // 1. Let value be the SVGAngle's value.
    return serialize(internal_value());
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAngle__valueAsString
WebIDL::ExceptionOr<void> SVGAngle::set_value_as_string(Utf16String const& value)
{
    // 1. If the SVGAngle object is read only, then throw a NoModificationAllowedError.
    TRY(ensure_not_read_only());

    // 2. Let value be the value being assigned to valueAsString.
    // 3. Parse value using the CSS syntax [ <number> | <angle> ].
    auto parsed_value = parse(value);

    // 4. If parsing failed, then throw a SyntaxError.
    if (!parsed_value.has_value())
        return WebIDL::SyntaxError::create("Failed to parse value as a <number> or <angle>"_utf16);

    // 5. Otherwise, parsing succeeded. Set SVGAngle's value to the parsed value.
    // 6. If the SVGAngle reflects the base value of a reflected attribute or reflects an element of the base value of
    //    a reflected attribute, then reserialize the reflected attribute.
    set_internal_value(*parsed_value);
    return {};
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAngle__newValueSpecifiedUnits
WebIDL::ExceptionOr<void> SVGAngle::new_value_specified_units(u16 unit_type, float value_in_specified_units)
{
    // 1. If the SVGAngle object is read only, then throw a NoModificationAllowedError.
    TRY(ensure_not_read_only());

    // 2. If unitType is SVG_ANGLETYPE_UNKNOWN or is a value that does not appear in the angle unit type table above,
    //    then throw a NotSupportedError.
    if (unit_type == SVG_ANGLETYPE_UNKNOWN || unit_type > SVG_ANGLETYPE_GRAD)
        return WebIDL::NotSupportedError::create("Unsupported angle unit type"_utf16);

    // 3. Set SVGAngle's value depending on the value of unitType:
    //    SVG_ANGLETYPE_UNSPECIFIED: a <number> whose value is valueInSpecifiedUnits
    //    anything else: an <angle> whose numeric factor is valueInSpecifiedUnits and whose unit is as indicated by the
    //    angle unit type table above
    // 4. If the SVGAngle reflects the base value of a reflected attribute or reflects an element of the base value of
    //    a reflected attribute, then reserialize the reflected attribute.
    set_internal_value({ value_in_specified_units, angle_unit_for_unit_type(unit_type) });
    return {};
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGAngle__convertToSpecifiedUnits
WebIDL::ExceptionOr<void> SVGAngle::convert_to_specified_units(u16 unit_type)
{
    // 1. If the SVGAngle object is read only, then throw a NoModificationAllowedError.
    TRY(ensure_not_read_only());

    // 2. If unitType is SVG_ANGLETYPE_UNKNOWN or is a value that does not appear in the angle unit type table above,
    //    then throw a NotSupportedError.
    if (unit_type == SVG_ANGLETYPE_UNKNOWN || unit_type > SVG_ANGLETYPE_GRAD)
        return WebIDL::NotSupportedError::create("Unsupported angle unit type"_utf16);

    // 3. Let degrees be the value that would be returned from the value member.
    auto degrees = value();

    // 4. If unitType is SVG_ANGLETYPE_UNSPECIFIED, then:
    if (unit_type == SVG_ANGLETYPE_UNSPECIFIED) {
        // 1. Set the SVGAngle's value to a <number> whose value is degrees.
        set_internal_value({ degrees, {} });
    }
    // 5. Otherwise:
    else {
        // 1. Set the SVGAngle's value to the result of converting degrees to an <angle> with the unit found by looking
        //    up unitType in the angle unit type table above.
        auto unit = *angle_unit_for_unit_type(unit_type);
        set_internal_value({ static_cast<float>(degrees * CSS::ratio_between_units(CSS::AngleUnit::Deg, unit)), unit });
    }

    // 6. If the SVGAngle reflects the base value of a reflected attribute or reflects an element of the base value of
    //    a reflected attribute, then reserialize the reflected attribute.
    // NB: This happens as part of setting the value above.
    return {};
}

}
