/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGLength.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGLength.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGLength);

GC::Ref<SVGLength> SVGLength::create_detached(JS::Realm& realm, NonnullRefPtr<CSS::StyleValue const> value, ReadOnly read_only)
{
    return realm.create<SVGLength>(realm, nullptr, Directionality::Unspecified, DetachedSource { .value = move(value) }, read_only);
}

GC::Ref<SVGLength> SVGLength::create_reflected_attribute(JS::Realm& realm, GC::Ref<SVGElement> element, Utf16FlyString name, Directionality directionality, ReflectedAttributeType type, NonnullRefPtr<CSS::StyleValue const> default_value, ReadOnly read_only)
{
    return realm.create<SVGLength>(realm, element, directionality, ReflectedAttributeSource { .name = move(name), .type = type, .default_value = move(default_value) }, read_only);
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

SVGLength::SVGLength(JS::Realm& realm, GC::Ptr<SVGElement> associated_element, Directionality directionality, Source&& source, ReadOnly read_only)
    : PlatformObject(realm)
    , m_element(associated_element)
    , m_directionality(directionality)
    , m_source(move(source))
    , m_read_only(read_only)
{
}

void SVGLength::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGLength);
    Base::initialize(realm);
}

void SVGLength::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (m_source.has<ReflectedAttributeSource>())
        visitor.visit(m_element);
}

SVGLength::~SVGLength() = default;

float SVGLength::value() const
{
    return parsed_value_from_style_value(internal_value()).value;
}

u8 SVGLength::unit_type() const
{
    return parsed_value_from_style_value(internal_value()).unit;
}

static RefPtr<CSS::StyleValue const> parse_css_length_value(JS::Realm& realm, Utf16View const& value)
{
    // https://svgwg.org/svg2-draft/types.html#presentation-attribute-css-value
    // When a presentation attribute defined using the CSS Value Definition Syntax is parsed, this is done as follows:
    // Replace all instances of <length-percentage> in grammar with [<length-percentage> | <number>].
    // FIXME: This is implemented in parse_literal_length_value() when using
    //        ParsingMode::SVGPresentationAttribute but is incomplete as it only supports literal numbers and
    //        immediately converts them to the equivalent length value in pixels (we should support all
    //        <number> values, including math and tree-counting functions) so we implement this again here until
    //        that is fixed.

    // FIXME: Respect attribute specific range restrictions (e.g. <circle>/r must be non-negative)

    CSS::Parser::ParsingParams parsing_params { realm, CSS::Parser::ParsingMode::SVGPresentationAttribute };
    if (auto parsed_style_value = parse_css_type(parsing_params, value, CSS::ValueType::Number))
        return parsed_style_value.release_nonnull();

    if (auto parsed_style_value = parse_css_type(parsing_params, value, CSS::ValueType::LengthPercentage))
        return parsed_style_value.release_nonnull();

    return nullptr;
}

NonnullRefPtr<CSS::StyleValue const> SVGLength::internal_value() const
{
    return m_source.visit(
        [&](ReflectedAttributeSource const& source) -> NonnullRefPtr<CSS::StyleValue const> {
            // NB: All attribute reflecting lengths should have an associated element
            VERIFY(m_element);

            // FIXME: Respect source.type once we support SMIL animation.
            // FIXME: Respect attribute namespaces
            auto maybe_attribute_value = m_element->get_attribute_value_view(source.name);
            if (!maybe_attribute_value.has_value())
                return source.default_value;

            auto attribute_value = maybe_attribute_value.release_value();

            if (auto parsed_style_value = parse_css_length_value(realm(), attribute_value))
                return parsed_style_value.release_nonnull();

            return source.default_value;
        },
        [](DetachedSource const& source) -> NonnullRefPtr<CSS::StyleValue const> {
            return source.value;
        });
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGLength__value
WebIDL::ExceptionOr<void> SVGLength::set_value(float value)
{
    // 1. If the SVGLength object is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnly::Yes)
        return WebIDL::NoModificationAllowedError::create(realm(), "Cannot modify value of read-only SVGLength"_utf16);

    // 2. Let value be the value being assigned to value.
    // 3. Set the SVGLength's value to a <number> whose value is value.
    // NB: Modes other than DetachedSource have their value set implicitly when reserializing the reflected attribute.
    if (m_source.has<DetachedSource>())
        m_source.get<DetachedSource>().value = CSS::NumberStyleValue::create(value);

    // 4. If the SVGLength reflects the base value of a reflected attribute, reflects a presentation attribute, or
    //    reflects an element of the base value of a reflected attribute, then reserialize the reflected attribute.
    // FIXME: Implement this for "reflects a presentation attribute" and "reflects an element of the base value of a
    //        reflected attribute" if/when we support those modes.
    if (m_source.has<ReflectedAttributeSource>()) {
        // NB: All attribute reflecting lengths should have an associated element
        VERIFY(m_element);

        m_element->set_attribute_value(m_source.get<ReflectedAttributeSource>().name, Utf16String::number(value));
    }

    return {};
}

}
