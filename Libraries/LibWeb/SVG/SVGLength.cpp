/*
 * Copyright (c) 2022, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGLength.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ComputationContext.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGForeignObjectElement.h>
#include <LibWeb/SVG/SVGLength.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGLength);

GC::Ref<SVGLength> SVGLength::create_detached(JS::Realm& realm, SVGLengthValue value, ReadOnly read_only)
{
    return realm.create<SVGLength>(realm, nullptr, Directionality::Unspecified, DetachedSource { .value = value }, read_only);
}

GC::Ref<SVGLength> SVGLength::create_reflected_attribute(JS::Realm& realm, GC::Ref<SVGElement> element, Utf16FlyString name, Directionality directionality, ReflectedAttributeType type, SVGLengthValue default_value, ReadOnly read_only)
{
    return realm.create<SVGLength>(realm, element, directionality, ReflectedAttributeSource { .name = move(name), .type = type, .default_value = default_value }, read_only);
}

SVGLength::SVGLength(JS::Realm& realm, GC::Ptr<SVGElement> associated_element, Directionality directionality, Source&& source, ReadOnly read_only)
    : Bindings::GCAllocatedWrappable()
    , m_element(associated_element)
    , m_realm(realm)
    , m_directionality(directionality)
    , m_source(move(source))
    , m_read_only(read_only)
{
}

void SVGLength::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (m_source.has<ReflectedAttributeSource>())
        visitor.visit(m_element);
    visitor.visit(m_realm);
}

SVGLength::~SVGLength() = default;

static double percentage_resolution_basis_for_attribute_reflecting_length(GC::Ptr<SVGElement> element, SVGLength::Directionality directionality)
{
    // AD-HOC: This is defined in the algorithms of both SVGLength::value and SVGLength::convertToSpecifiedUnits so we
    //         factor it out here to avoid duplication.

    // - has no associated element
    if (!element) {
        // size is 100
        return 100.0;
    }

    // NB: Make sure the SVG layout is up to date so the viewport size is up to date.
    element->document().update_layout(DOM::UpdateLayoutReason::SVGLengthValue);

    auto viewport = element->viewport_size_for_percentage_resolution();

    switch (directionality) {
    case SVGLength::Directionality::Horizontal:
        // - has an associated element and horizontal directionality
        //     size is the width of the associated element's SVG viewport
        return viewport.width();
    case SVGLength::Directionality::Vertical:
        // - has an associated element and vertical directionality
        //     size is the height of the associated element's SVG viewport
        return viewport.height();
    case SVGLength::Directionality::Unspecified:
        // - has an associated element and unspecified directionality
        //     size is the length of the associated element's SVG viewport diagonal (see Units)
        return AK::sqrt((viewport.width() * viewport.width() + viewport.height() * viewport.height()) / 2.0);
    }

    VERIFY_NOT_REACHED();
}

static Optional<CSS::Length::ResolutionContext> length_resolution_context_for_element(GC::Ptr<SVGElement> element)
{
    // AD-HOC: This is defined in the algorithms of both SVGLength::value and SVGLength::convertToSpecifiedUnits so we
    //         factor it out here to avoid duplication.

    // - has no associated element
    // AD-HOC: Other browsers also do this for disconnected elements
    // FIXME: Length::ResolutionContext::for_element doesn't support elements without a navigable so we bail on that
    //        too which is incorrect
    if (!element || !element->is_connected() || !element->navigable()) {
        // font size is the absolute length of the initial value of the font-size property
        // AD-HOC: Browsers disagree on what we should actually do here, WebKit implements the spec, Gecko uses a LRC
        //         with all zeros, and Blink throws an error. We return an OptionalNone and callers should throw an
        //         error matching Blink's behavior since that's the easiest to implement.
        return {};
    }

    // - has an associated element
    {
        // NB: Make sure style updates are applied so the LRC is up to date
        element->document().update_style_for_element(*element);

        // size is the computed value of the associated element's font-size property
        return CSS::Length::ResolutionContext::for_element(*element);
    }
}

// https://w3c.github.io/svgwg/svg2-draft/types.html#__svg__SVGLength__value
WebIDL::ExceptionOr<float> SVGLength::value() const
{
    // 1. Let value be the SVGLength's value.
    auto value = internal_value();

    // 2. If value is a <number>, return that number.
    // AD-HOC: We only return literal numbers here, non-literal values (e.g. math and tree counting functions) are
    //         returned once they have been absolutized in step 4.
    if (auto const* scalar = value.get_pointer<SVGLengthValue>(); scalar && scalar->kind() == SVGLengthValue::Kind::Number)
        return scalar->value();

    // 3. Let viewport size be a basis to resolve percentages against, based on the SVGLength's associated element
    //    and directionality:
    auto viewport_size = percentage_resolution_basis_for_attribute_reflecting_length(m_element, m_directionality);

    // 4. Let font size be a basis to resolve font size values against, based on the SVGLength's associated element:
    // AD-HOC: We need a full-fledged Length::ResolutionContext here instead of just the font size so we can resolve all
    //         relative lengths, not just em (e.g. ch, vw, etc).
    auto length_resolution_context = length_resolution_context_for_element(m_element);

    // 5. Return the result of converting value to an absolute length, using viewport size and font size as percentage
    //    and font size bases. If the conversion is not possible due to the lack of an associated element, return 0.

    // AD-HOC: Chrome raises an error rather than returning 0 so we do the same here.
    auto error_value = [&] { return WebIDL::NotSupportedError::create(*m_realm, ""_utf16); };

    if (!length_resolution_context.has_value()) {
        if (auto const* scalar = value.get_pointer<SVGLengthValue>()) {
            if (scalar->kind() == SVGLengthValue::Kind::Percentage)
                return CSS::Length::make_px(viewport_size).percentage_of(scalar->to_percentage()).absolute_length_to_px_without_rounding();

            auto length = scalar->to_length();

            // NB: Font-relative and container-relative lengths are not computationally independent; viewport relative
            //     lengths are but we still can't resolve them without an element.
            if (!length.is_absolute())
                return error_value();

            return length.absolute_length_to_px_without_rounding();
        }

        auto const& style_value = value.get<NonnullRefPtr<CSS::StyleValue const>>();

        if (!style_value->is_computationally_independent())
            return error_value();

        // NB: Values which rely on viewport relative lengths are computationally independent but we still can't resolve
        //    them - we pass a bogus LRC and check if absolutization depends on viewport metrics.
        CSS::ComputationContext computation_context {
            .length_resolution_context = { .viewport_rect = { 0, 0, 0, 0 }, .font_metrics = { 0, {}, {} }, .root_font_metrics = { 0, {}, {} } },
        };

        auto absolutized_value = style_value->absolutized(computation_context);

        if (computation_context.depends_on_viewport_metrics())
            return error_value();

        // AD-HOC: We handle literal numbers in step two but calculated values that resolve to numbers haven't been
        //         handled yet (tree counting functions have been filtered out since they aren't computationally
        //         independent).
        if (absolutized_value->is_calculated() && absolutized_value->as_calculated().resolves_to_number())
            return number_from_style_value(*absolutized_value, {});

        return CSS::Length::from_style_value(absolutized_value, CSS::Length::make_px(viewport_size)).absolute_length_to_px_without_rounding();
    }

    // NB: Update layout so any container query units are resolved correctly.
    // NB: We can be sure we have an element since we wouldn't have a length resolution context otherwise.
    m_element->document().update_layout(DOM::UpdateLayoutReason::SVGLengthValue);

    if (auto const* scalar = value.get_pointer<SVGLengthValue>()) {
        if (scalar->kind() == SVGLengthValue::Kind::Percentage)
            return CSS::Length::make_px(viewport_size).percentage_of(scalar->to_percentage()).absolute_length_to_px_without_rounding();

        return scalar->to_length().to_px_without_rounding(*length_resolution_context);
    }

    auto const& style_value = value.get<NonnullRefPtr<CSS::StyleValue const>>();

    CSS::ComputationContext computation_context {
        .length_resolution_context = length_resolution_context.release_value(),
        .abstract_element = { *m_element },
    };

    auto absolutized_value = style_value->absolutized(computation_context);

    // AD-HOC: We handle literal numbers in step two but tree counting functions (which absolutize to NumberStyleValue)
    //         and calculated values that resolve to numbers haven't been handled yet.
    if (absolutized_value->is_number() || (absolutized_value->is_calculated() && absolutized_value->as_calculated().resolves_to_number()))
        return number_from_style_value(*absolutized_value, {});

    return CSS::Length::from_style_value(absolutized_value, CSS::Length::make_px(viewport_size)).absolute_length_to_px_without_rounding();
}

// https://w3c.github.io/svgwg/svg2-draft/types.html#__svg__SVGLength__unitType
u8 SVGLength::unit_type() const
{
    // 1. If the SVGLength's value is a unitless <number>, a <percentage>, or a <length> with an em, ex, px, cm, mm, in,
    //    pt or pc unit, then return the corresponding constant value from the length unit type table above.
    auto value = internal_value();

    if (auto const* scalar = value.get_pointer<SVGLengthValue>()) {
        switch (scalar->kind()) {
        case SVGLengthValue::Kind::Number:
            return SVG_LENGTHTYPE_NUMBER;
        case SVGLengthValue::Kind::Percentage:
            return SVG_LENGTHTYPE_PERCENTAGE;
        case SVGLengthValue::Kind::Length:
            switch (scalar->unit()) {
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
                break;
            }
            break;
        }
    }

    // 2. Otherwise, return SVG_LENGTHTYPE_UNKNOWN.
    return SVG_LENGTHTYPE_UNKNOWN;
}

static RefPtr<CSS::StyleValue const> parse_css_length_value(Utf16View const& value)
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

    CSS::Parser::ParsingParams parsing_params { CSS::Parser::ParsingMode::SVGPresentationAttribute };
    if (auto parsed_style_value = parse_css_type(parsing_params, value, CSS::ValueType::Number))
        return parsed_style_value.release_nonnull();

    if (auto parsed_style_value = parse_css_type(parsing_params, value, CSS::ValueType::LengthPercentage))
        return parsed_style_value.release_nonnull();

    return nullptr;
}

SVGLength::InternalValue SVGLength::internal_value() const
{
    return m_source.visit(
        [&](ReflectedAttributeSource const& source) -> InternalValue {
            // NB: All attribute reflecting lengths should have an associated element
            VERIFY(m_element);

            // FIXME: Respect source.type once we support SMIL animation.
            // FIXME: Respect attribute namespaces
            auto maybe_attribute_value = m_element->get_attribute_value_view(source.name);
            if (!maybe_attribute_value.has_value())
                return source.default_value;

            auto attribute_value = maybe_attribute_value.release_value();

            if (auto parsed_style_value = parse_css_length_value(attribute_value)) {
                if (auto scalar = SVGLengthValue::from_style_value(*parsed_style_value); scalar.has_value())
                    return scalar.release_value();

                return parsed_style_value.release_nonnull();
            }

            return source.default_value;
        },
        [](DetachedSource const& source) -> InternalValue {
            return source.value.visit(
                [](SVGLengthValue const& scalar) -> InternalValue { return scalar; },
                [](Utf16String const& text) -> InternalValue {
                    // NB: The text is the canonical serialization of a non-scalar value, so it always parses; keep it
                    //     non-scalar even if serialization simplified it to something a plain value could hold.
                    if (auto parsed_style_value = parse_css_length_value(text))
                        return parsed_style_value.release_nonnull();

                    return SVGLengthValue::number(0);
                });
        });
}

struct Number { };
struct Percentage { };
struct Length {
    CSS::LengthUnit unit;
};

using Unit = Variant<Number, Percentage, Length>;

static WebIDL::ExceptionOr<SVGLengthValue> convert_px_to_specified_units(JS::Realm& realm, float value, Unit unit, GC::Ptr<SVGElement> element, SVGLength::Directionality directionality)
{
    if (unit.has<Number>())
        return SVGLengthValue::number(value);

    if (unit.has<Percentage>()) {
        auto percentage_resolution_basis = percentage_resolution_basis_for_attribute_reflecting_length(element, directionality);

        if (percentage_resolution_basis == 0.0)
            return SVGLengthValue::percentage(0.0);

        return SVGLengthValue::percentage(value / percentage_resolution_basis * 100.0);
    }

    auto length_unit = unit.get<Length>().unit;

    auto ratio = TRY([&]() -> WebIDL::ExceptionOr<double> {
        if (CSS::is_container_relative(length_unit))
            VERIFY_NOT_REACHED();

        if (CSS::is_absolute(length_unit))
            return CSS::ratio_between_units(CSS::LengthUnit::Px, length_unit);

        auto length_resolution_context = length_resolution_context_for_element(element);

        if (!length_resolution_context.has_value())
            return WebIDL::NotSupportedError::create(realm, "Cannot convert to relative length without an associated element"_utf16);

        if (CSS::is_font_relative(length_unit))
            return 1.0 / CSS::ratio_between_font_relative_unit_and_px(length_unit, length_resolution_context->font_metrics, length_resolution_context->root_font_metrics);

        if (CSS::is_viewport_relative(length_unit))
            return 1.0 / CSS::ratio_between_viewport_relative_unit_and_px(length_unit, length_resolution_context->viewport_rect);

        VERIFY_NOT_REACHED();
    }());

    if (!isfinite(ratio))
        ratio = 0;

    return SVGLengthValue::length(value * ratio, length_unit);
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGLength__value
WebIDL::ExceptionOr<void> SVGLength::set_value(float value)
{
    // 1. If the SVGLength object is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnly::Yes)
        return WebIDL::NoModificationAllowedError::create("Cannot modify value of read-only SVGLength"_utf16);

    // 2. Let value be the value being assigned to value.

    // AD-HOC: Other browsers set the value to the equivalent value using the existing value's unit (i.e. if the
    //         existing value is 1em and the value is set to 32, with a font-size of 16px, the new value is 2em) - the
    //         exception is if the old value is non-scalar (e.g. calc) or is a container unit in which case it is always
    //         set to a number.
    auto target_unit = [&]() -> Unit {
        auto existing_value = internal_value();

        auto const* scalar = existing_value.get_pointer<SVGLengthValue>();
        if (!scalar)
            return Number {};

        switch (scalar->kind()) {
        case SVGLengthValue::Kind::Number:
            return Number {};
        case SVGLengthValue::Kind::Percentage:
            return Percentage {};
        case SVGLengthValue::Kind::Length: {
            auto length_unit = scalar->unit();

            if (CSS::is_container_relative(length_unit))
                return Number {};

            return Length { length_unit };
        }
        }

        VERIFY_NOT_REACHED();
    }();

    auto new_value = TRY(convert_px_to_specified_units(*m_realm, value, target_unit, m_element, m_directionality));

    // 3. Set the SVGLength's value to a <number> whose value is value.
    // NB: Modes other than DetachedSource have their value set implicitly when reserializing the reflected attribute.
    if (m_source.has<DetachedSource>())
        m_source.get<DetachedSource>().value = new_value;

    // 4. If the SVGLength reflects the base value of a reflected attribute, reflects a presentation attribute, or
    //    reflects an element of the base value of a reflected attribute, then reserialize the reflected attribute.
    // FIXME: Implement this for "reflects a presentation attribute" and "reflects an element of the base value of a
    //        reflected attribute" if/when we support those modes.
    if (m_source.has<ReflectedAttributeSource>()) {
        // NB: All attribute reflecting lengths should have an associated element
        VERIFY(m_element);

        m_element->set_attribute_value(m_source.get<ReflectedAttributeSource>().name, new_value.to_utf16_string());
    }

    return {};
}

// https://w3c.github.io/svgwg/svg2-draft/types.html#__svg__SVGLength__valueInSpecifiedUnits
float SVGLength::value_in_specified_units() const
{
    // On getting valueInSpecifiedUnits, the following steps are run:

    // 1. Let value be the SVGLength's value.
    auto value = internal_value();

    // 2. If value is a <number>, return that number.
    // 3. Otherwise, if value is a <percentage> or any scalar <length> value, return the numeric factor before its unit.
    if (auto const* scalar = value.get_pointer<SVGLengthValue>())
        return scalar->value();

    // 4. Otherwise, return 0.
    return 0;

    // NOTE: Thus valueInSpecifiedUnits would return 12 for both '12%' and 12em, but 0 would be returned for non-scalar
    //       values like calc(12px + 5%).
}

// https://w3c.github.io/svgwg/svg2-draft/types.html#__svg__SVGLength__valueInSpecifiedUnits
WebIDL::ExceptionOr<void> SVGLength::set_value_in_specified_units(float value)
{
    // On setting valueInSpecifiedUnits, the following steps are run:

    auto existing_value = internal_value();

    // 1. If the SVGLength object is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnly::Yes)
        return WebIDL::NoModificationAllowedError::create(*m_realm, "Cannot modify value of read-only SVGLength"_utf16);

    // 2. Let value be the value being assigned to valueInSpecifiedUnits.

    auto new_value = [&] {
        auto const* existing_scalar = existing_value.get_pointer<SVGLengthValue>();

        // 5. Otherwise, the SVGLength's value is of some other type. Set it to a <number> whose value is value.
        if (!existing_scalar)
            return SVGLengthValue::number(value);

        switch (existing_scalar->kind()) {
        // 3. If the SVGLength's value is a <number>, then update its value to value.
        case SVGLengthValue::Kind::Number:
            return SVGLengthValue::number(value);
        // 4. Otherwise, if the SVGLength's value is a <percentage> or a scalar-valued <length>, then update its numeric
        //    factor to value.
        case SVGLengthValue::Kind::Percentage:
            return SVGLengthValue::percentage(value);
        case SVGLengthValue::Kind::Length:
            return SVGLengthValue::length(value, existing_scalar->unit());
        }

        VERIFY_NOT_REACHED();
    }();

    // NB: Modes other than DetachedSource have their value set implicitly when reserializing the reflected attribute.
    if (m_source.has<DetachedSource>())
        m_source.get<DetachedSource>().value = new_value;

    // 6. If the SVGLength reflects the base value of a reflected attribute or reflects an element of the base value of
    //    a reflected attribute, then reserialize the reflected attribute.

    // FIXME: Implement this for "reflects an element of the base value of a reflected attribute" when we support it.
    // FIXME: Should this also happen for "reflects a presentation attribute" as we do with set_value()?
    if (m_source.has<ReflectedAttributeSource>()) {
        // NB: All attribute reflecting lengths should have an associated element
        VERIFY(m_element);

        m_element->set_attribute_value(m_source.get<ReflectedAttributeSource>().name, new_value.to_utf16_string());
    }

    return {};
}

// https://w3c.github.io/svgwg/svg2-draft/types.html#__svg__SVGLength__valueAsString
Utf16String SVGLength::value_as_string() const
{
    // On getting valueAsString, the following steps are run:

    // 1. Let value be the SVGLength's value.

    // 2. Let string be an empty string.

    // 3. If value is a <number>, <percentage> or scalar <length> value, then:
    {
        // 1. Let factor be value's numeric factor, if it is a <percentage> or <length>, or value itself it is a
        //    <number>.

        // 2. Append to string an implementation specific string that, if parsed as a <number> using CSS syntax,
        //    would return the number value closest to factor, given the implementation's supported real number
        //    precision.

        // 3. If value is a <percentage> then append to string a single U+0025 PERCENT SIGN character.

        // 4. Otherwise, if value is a <length>, then append to string the canonical spelling of value's unit.

        // 5. Return string.
    }

    // 6. Otherwise, return an implementation specific string that, if parsed as a <length>, would return the closest
    //    length value to value, given the implementation's supported real number precision.

    // AD-HOC: We can achieve the same functionality by just serializing the internal value to a string.
    return internal_value().visit(
        [](SVGLengthValue const& scalar) { return scalar.to_utf16_string(); },
        [](NonnullRefPtr<CSS::StyleValue const> const& style_value) { return style_value->to_utf16_string(CSS::SerializationMode::Normal); });
}

// https://w3c.github.io/svgwg/svg2-draft/types.html#__svg__SVGLength__valueAsString
WebIDL::ExceptionOr<void> SVGLength::set_value_as_string(Utf16String value)
{
    // On setting valueAsString, the following steps are run:

    // 1. If the SVGLength object is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnly::Yes)
        return WebIDL::NoModificationAllowedError::create(*m_realm, "Cannot modify value of read-only SVGLength"_utf16);

    // 2. Let value be the value being assigned to valueAsString.

    // 3. Parse value using the CSS syntax [ <number> | <length> | <percentage> ].
    auto parsed_value = parse_css_length_value(value);

    // 4. If parsing failed, then throw a SyntaxError.
    if (!parsed_value)
        return WebIDL::SyntaxError::create(*m_realm, "Failed to parse value as a <number>, <length> or <percentage>"_utf16);

    auto scalar = SVGLengthValue::from_style_value(*parsed_value);
    auto serialized = scalar.has_value() ? scalar->to_utf16_string() : parsed_value->to_utf16_string(CSS::SerializationMode::Normal);

    // 5. Otherwise, parsing succeeded. Set SVGLength's value to the parsed value.
    // NB: Modes other than DetachedSource have their value set implicitly when reserializing the reflected attribute.
    if (m_source.has<DetachedSource>()) {
        if (scalar.has_value())
            m_source.get<DetachedSource>().value = *scalar;
        else
            m_source.get<DetachedSource>().value = serialized;
    }

    // 6. If the SVGLength reflects the base value of a reflected attribute or reflects an element of the base value of
    //    a reflected attribute, then reserialize the reflected attribute.
    // FIXME: Implement this for "reflects an element of the base value of a reflected attribute" when we support it.
    // FIXME: Should this also happen for "reflects a presentation attribute" as we do with set_value()?
    if (m_source.has<ReflectedAttributeSource>()) {
        // NB: All attribute reflecting lengths should have an associated element
        VERIFY(m_element);

        m_element->set_attribute_value(m_source.get<ReflectedAttributeSource>().name, serialized);
    }

    return {};
}

static CSS::LengthUnit svg_length_type_to_css_length_unit(u8 unit_type)
{
    switch (unit_type) {
    case SVGLength::SVG_LENGTHTYPE_EMS:
        return CSS::LengthUnit::Em;
    case SVGLength::SVG_LENGTHTYPE_EXS:
        return CSS::LengthUnit::Ex;
    case SVGLength::SVG_LENGTHTYPE_PX:
        return CSS::LengthUnit::Px;
    case SVGLength::SVG_LENGTHTYPE_CM:
        return CSS::LengthUnit::Cm;
    case SVGLength::SVG_LENGTHTYPE_MM:
        return CSS::LengthUnit::Mm;
    case SVGLength::SVG_LENGTHTYPE_IN:
        return CSS::LengthUnit::In;
    case SVGLength::SVG_LENGTHTYPE_PT:
        return CSS::LengthUnit::Pt;
    case SVGLength::SVG_LENGTHTYPE_PC:
        return CSS::LengthUnit::Pc;
    default:
        break;
    }

    VERIFY_NOT_REACHED();
}

WebIDL::ExceptionOr<void> SVGLength::new_value_specified_units(u16 unit_type, float value_in_specified_units)
{
    // The newValueSpecifiedUnits method is used to set the SVGLength's value in a typed manner. When
    // newValueSpecifiedUnits(unitType, valueInSpecifiedUnits) is called, the following steps are run:
    //
    // 1. If the SVGLength object is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnly::Yes)
        return WebIDL::NoModificationAllowedError::create(*m_realm, "Cannot modify value of read-only SVGLength"_utf16);

    // 2. If unitType is SVG_LENGTHTYPE_UNKNOWN or is a value that does not appear in the length unit type table above,
    //    then throw a NotSupportedError.
    if (unit_type == SVG_LENGTHTYPE_UNKNOWN || unit_type > SVG_LENGTHTYPE_PC)
        return WebIDL::NotSupportedError::create(*m_realm, "Unsupported SVGLength unit type"_utf16);

    // 3. Set SVGLength's value depending on the value of unitType:
    auto new_value = [&] {
        switch (unit_type) {
        case SVG_LENGTHTYPE_NUMBER:
            // - SVG_LENGTHTYPE_NUMBER
            //     a <number> whose value is valueInSpecifiedUnits
            return SVGLengthValue::number(value_in_specified_units);
        case SVG_LENGTHTYPE_PERCENTAGE:
            // - SVG_LENGTHTYPE_PERCENTAGE
            //     a <percentage> whose numeric factor is valueInSpecifiedUnits
            return SVGLengthValue::percentage(value_in_specified_units);
        default:
            // - anything else
            //     a <length> whose numeric factor is valueInSpecifiedUnits and whose unit is as indicated by the length unit
            //     type table above
            return SVGLengthValue::length(value_in_specified_units, svg_length_type_to_css_length_unit(unit_type));
        }
    }();

    // NB: Modes other than DetachedSource have their value set implicitly when reserializing the reflected attribute.
    if (m_source.has<DetachedSource>())
        m_source.get<DetachedSource>().value = new_value;

    // 4. If the SVGLength reflects the base value of a reflected attribute or reflects an element of the base value of
    //    a reflected attribute, then reserialize the reflected attribute.
    // FIXME: Implement this for "reflects an element of the base value of a reflected attribute" when we support it.
    // FIXME: Should this also happen for "reflects a presentation attribute" as we do with set_value()?
    if (m_source.has<ReflectedAttributeSource>()) {
        // NB: All attribute reflecting lengths should have an associated element
        VERIFY(m_element);

        m_element->set_attribute_value(m_source.get<ReflectedAttributeSource>().name, new_value.to_utf16_string());
    }

    return {};
}

// https://w3c.github.io/svgwg/svg2-draft/types.html#__svg__SVGLength__convertToSpecifiedUnits
WebIDL::ExceptionOr<void> SVGLength::convert_to_specified_units(u16 unit_type)
{
    // 1. If the SVGLength object is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnly::Yes)
        return WebIDL::NoModificationAllowedError::create(*m_realm, "Cannot modify value of read-only SVGLength"_utf16);

    // 2. If unitType is SVG_LENGTHTYPE_UNKNOWN or is a value that does not appear in the length unit type table above,
    //    then throw a NotSupportedError.
    if (unit_type == SVG_LENGTHTYPE_UNKNOWN || unit_type > SVG_LENGTHTYPE_PC)
        return WebIDL::NotSupportedError::create(*m_realm, "Unsupported SVGLength unit type"_utf16);

    // 3. Let absolute be the value that would be returned from the value member.
    auto absolute = TRY(value());

    // 4. If unitType is SVG_LENGTHTYPE_NUMBER, then:
    //    1. Set the SVGLength's value to a <number> whose value is absolute.

    // 5. Otherwise, if unitType is SVG_LENGTHTYPE_PERCENTAGE, then:
    //    1. Let viewport size be a basis to resolve percentages against, based on the SVGLength's associated element
    //       and directionality:
    //    2. Set the SVGLength's value to the result of converting absolute to a <percentage>, using viewport size as
    //       the percentage basis.

    // 6. Otherwise, if unitType is SVG_LENGTHTYPE_EMS or SVG_LENGTHTYPE_EXS, then:
    //    1. Let font size be a basis to resolve font size values against, based on the SVGLength's associated element:
    //    2. Set the SVGLength's value to the result of converting absolute to a <length> with an em or ex unit
    //       (depending on unitType), using font size as the font-size basis.

    // 7. Otherwise:
    //    1. Set the SVGLength's value to the result of converting absolute to a <length> with the unit found by
    //       looking up unitType in the length unit type table above.

    // AD-HOC: We implement this out of line since it's also used in an set_value
    auto target_unit = [&]() {
        if (unit_type == SVG_LENGTHTYPE_NUMBER)
            return Unit { Number {} };

        if (unit_type == SVG_LENGTHTYPE_PERCENTAGE)
            return Unit { Percentage {} };

        return Unit { Length { svg_length_type_to_css_length_unit(unit_type) } };
    }();

    auto new_value = TRY(convert_px_to_specified_units(*m_realm, absolute, target_unit, m_element, m_directionality));

    // NB: Modes other than DetachedSource have their value set implicitly when reserializing the reflected attribute.
    if (m_source.has<DetachedSource>())
        m_source.get<DetachedSource>().value = new_value;

    // 8. If the SVGLength reflects the base value of a reflected attribute or reflects an element of the base value of
    //    a reflected attribute, then reserialize the reflected attribute.
    // FIXME: Implement this for "reflects an element of the base value of a reflected attribute" when we support it.
    // FIXME: Should this also happen for "reflects a presentation attribute" as we do with set_value()?
    if (m_source.has<ReflectedAttributeSource>()) {
        // NB: All attribute reflecting lengths should have an associated element
        VERIFY(m_element);

        m_element->set_attribute_value(m_source.get<ReflectedAttributeSource>().name, new_value.to_utf16_string());
    }

    return {};
}

}
