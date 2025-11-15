/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGComponentTransferFunctionElementPrototype.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGComponentTransferFunctionElement.h>
#include <LibWeb/SVG/SVGNumber.h>
#include <LibWeb/SVG/SVGNumberList.h>

namespace Web::SVG {

SVGComponentTransferFunctionElement::SVGComponentTransferFunctionElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

// https://drafts.fxtf.org/filter-effects-1/#element-attrdef-fecomponenttransfer-type
static SVGComponentTransferFunctionElement::Type parse_type(Optional<String> const& value)
{
    if (value == "identity"sv)
        return SVGComponentTransferFunctionElement::Type::Identity;
    if (value == "table"sv)
        return SVGComponentTransferFunctionElement::Type::Table;
    if (value == "discrete"sv)
        return SVGComponentTransferFunctionElement::Type::Discrete;
    if (value == "linear"sv)
        return SVGComponentTransferFunctionElement::Type::Linear;
    if (value == "gamma"sv)
        return SVGComponentTransferFunctionElement::Type::Gamma;

    return SVGComponentTransferFunctionElement::Type::Unknown;
}

void SVGComponentTransferFunctionElement::attribute_changed(FlyString const& name, Optional<String> const& old_value,
    Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // FIXME: Support reflection instead of invalidating the enumeration.
    if (name == AttributeNames::type)
        m_type = {};

    // FIXME: Support reflection instead of invalidating the list.
    if (name == AttributeNames::tableValues)
        m_table_values = {};

    // Clear our cached color table on any attribute change.
    m_cached_color_table.clear();
}

void SVGComponentTransferFunctionElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGComponentTransferFunctionElement);
    Base::initialize(realm);
}

void SVGComponentTransferFunctionElement::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_type);
    visitor.visit(m_table_values);
    visitor.visit(m_slope);
    visitor.visit(m_intercept);
    visitor.visit(m_amplitude);
    visitor.visit(m_exponent);
    visitor.visit(m_offset);
}

// https://www.w3.org/TR/filter-effects-1/#dom-svgcomponenttransferfunctionelement-type
// https://drafts.fxtf.org/filter-effects-1/#element-attrdef-fecomponenttransfer-type
GC::Ref<SVGAnimatedEnumeration> SVGComponentTransferFunctionElement::type()
{
    if (!m_type)
        m_type = SVGAnimatedEnumeration::create(realm(), to_underlying(type_from_attribute()));
    return *m_type;
}

// https://www.w3.org/TR/filter-effects-1/#dom-svgcomponenttransferfunctionelement-tablevalues
// https://drafts.fxtf.org/filter-effects-1/#element-attrdef-fecomponenttransfer-tablevalues
GC::Ref<SVGAnimatedNumberList> SVGComponentTransferFunctionElement::table_values()
{
    if (!m_table_values) {
        auto numbers = AttributeParser::parse_table_values(get_attribute_value(AttributeNames::tableValues));

        Vector<GC::Ref<SVGNumber>> items;
        items.ensure_capacity(numbers.size());
        for (auto number : numbers)
            items.unchecked_append(SVGNumber::create(realm(), number, SVGNumber::ReadOnly::Yes));

        auto number_list = SVGNumberList::create(realm(), move(items), ReadOnlyList::Yes);
        m_table_values = SVGAnimatedNumberList::create(realm(), number_list);
    }
    return *m_table_values;
}

// https://www.w3.org/TR/filter-effects-1/#dom-svgcomponenttransferfunctionelement-slope
// https://drafts.fxtf.org/filter-effects-1/#element-attrdef-fecomponenttransfer-slope
GC::Ref<SVGAnimatedNumber> SVGComponentTransferFunctionElement::slope()
{
    if (!m_slope)
        m_slope = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::slope, {}, {} }, 1.f);
    return *m_slope;
}

// https://www.w3.org/TR/filter-effects-1/#dom-svgcomponenttransferfunctionelement-intercept
// https://drafts.fxtf.org/filter-effects-1/#element-attrdef-fecomponenttransfer-intercept
GC::Ref<SVGAnimatedNumber> SVGComponentTransferFunctionElement::intercept()
{
    if (!m_intercept)
        m_intercept = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::intercept, {}, {} }, 0.f);
    return *m_intercept;
}

// https://www.w3.org/TR/filter-effects-1/#dom-svgcomponenttransferfunctionelement-amplitude
// https://drafts.fxtf.org/filter-effects-1/#element-attrdef-fecomponenttransfer-amplitude
GC::Ref<SVGAnimatedNumber> SVGComponentTransferFunctionElement::amplitude()
{
    if (!m_amplitude)
        m_amplitude = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::amplitude, {}, {} }, 1.f);
    return *m_amplitude;
}

// https://www.w3.org/TR/filter-effects-1/#dom-svgcomponenttransferfunctionelement-exponent
// https://drafts.fxtf.org/filter-effects-1/#element-attrdef-fecomponenttransfer-exponent
GC::Ref<SVGAnimatedNumber> SVGComponentTransferFunctionElement::exponent()
{
    if (!m_exponent)
        m_exponent = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::exponent, {}, {} }, 1.f);
    return *m_exponent;
}

// https://www.w3.org/TR/filter-effects-1/#dom-svgcomponenttransferfunctionelement-offset
// https://drafts.fxtf.org/filter-effects-1/#element-attrdef-fecomponenttransfer-offset
GC::Ref<SVGAnimatedNumber> SVGComponentTransferFunctionElement::offset()
{
    if (!m_offset)
        m_offset = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::offset, {}, {} }, 0.f);
    return *m_offset;
}

SVGComponentTransferFunctionElement::Type SVGComponentTransferFunctionElement::type_from_attribute() const
{
    return parse_type(get_attribute_value(AttributeNames::type));
}

Vector<float> SVGComponentTransferFunctionElement::table_float_values()
{
    Vector<float> values;
    auto table_numbers = table_values()->base_val()->items();
    values.ensure_capacity(table_numbers.size());
    for (auto& svg_number : table_numbers)
        values.unchecked_append(svg_number->value());
    return values;
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-fecomponenttransfer-type
ReadonlyBytes SVGComponentTransferFunctionElement::color_table()
{
    if (m_cached_color_table.has_value())
        return m_cached_color_table.value();

    ByteBuffer result;
    result.resize(256);

    auto set_identity = [&result] {
        for (int i = 0; i < 256; ++i)
            result.data()[i] = i;
    };
    auto to_u8 = [](float value) {
        return AK::clamp_to<u8>(value * 255.f);
    };

    switch (type_from_attribute()) {
    // https://drafts.fxtf.org/filter-effects/#attr-valuedef-type-identity
    case Type::Unknown:
    case Type::Identity:
        set_identity();
        break;

    // https://drafts.fxtf.org/filter-effects/#attr-valuedef-type-table
    case Type::Table: {
        auto table_values = table_float_values();

        // An empty list results in an identity transfer function.
        if (table_values.is_empty()) {
            set_identity();
            break;
        }

        // For a value C < 1 find k such that: k/n <= C < (k+1)/n
        // The result C' is given by: C' = vk + (C - k/n)*n * (vk+1 - vk)
        auto const segments = table_values.size() - 1.f;
        for (int i = 0; i < 256; ++i) {
            // If C = 1 then: C' = vn.
            if (i == 255 || segments == 0.f) {
                result.data()[i] = to_u8(table_values.last());
                continue;
            }

            auto offset = i / 255.f;
            auto segment_index = static_cast<size_t>(offset * segments);
            auto segment_start = segment_index / segments;
            auto offset_in_segment = offset - segment_start;
            auto segment_length = 1.f / segments;
            auto progress_in_segment = offset_in_segment / segment_length;

            auto segment_value = mix(table_values[segment_index], table_values[segment_index + 1], progress_in_segment);
            result.data()[i] = to_u8(segment_value);
        }
        break;
    }

    // https://drafts.fxtf.org/filter-effects/#attr-valuedef-type-discrete
    case Type::Discrete: {
        auto table_values = table_float_values();

        // An empty list results in an identity transfer function.
        if (table_values.is_empty()) {
            set_identity();
            break;
        }

        // For a value C < 1 find k such that: k/n <= C < (k+1)/n
        // The result C' is given by: C' = vk + (C - k/n)*n * (vk+1 - vk)
        for (int i = 0; i < 255; ++i) {
            auto index = static_cast<size_t>(i / 255.f * table_values.size());
            result.data()[i] = to_u8(table_values[index]);
        }

        // If C = 1 then: C' = vn.
        result.data()[255] = to_u8(table_values.last());
        break;
    }

    // https://drafts.fxtf.org/filter-effects/#attr-valuedef-type-linear
    case Type::Linear: {
        auto slope = this->slope()->base_val();
        auto intercept = this->intercept()->base_val();
        for (int i = 0; i < 256; ++i)
            result.data()[i] = to_u8(slope * i / 255.f + intercept);
        break;
    }

    // https://drafts.fxtf.org/filter-effects/#attr-valuedef-type-gamma
    case Type::Gamma: {
        auto amplitude = this->amplitude()->base_val();
        auto exponent = this->exponent()->base_val();
        auto offset = this->offset()->base_val();
        for (int i = 0; i < 256; ++i)
            result.data()[i] = to_u8(amplitude * pow(i / 255.f, exponent) + offset);
        break;
    }
    }

    m_cached_color_table = move(result);
    return m_cached_color_table.value();
}

}
