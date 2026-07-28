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
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Painting/SVGSVGPaintable.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGForeignObjectElement.h>
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

static Gfx::Size<double> svg_viewport_size(SVGElement& element)
{
    auto viewport_size_from_layout = [](SVGSVGElement& viewport_element) -> Gfx::Size<double> {
        // NB: A disconnected element may have stale layout objects from before it was removed.
        if (!viewport_element.is_connected())
            return {};

        if (auto const* svg_paintable = as_if<Painting::SVGSVGPaintable>(viewport_element.paintable().ptr()))
            return svg_paintable->svg_viewport_size().to_type<double>();

        return {};
    };

    // NB: Presentational attributes on SVGSVGElements are resolved irrespective of it's own viewBox (which only affects
    //     the internal coordinate system)
    if (auto* svg_element = as_if<SVGSVGElement>(element)) {
        auto const* parent = svg_element->parent_or_shadow_host_element();

        // https://w3c.github.io/svgwg/svg2-draft/coords.html#InitialViewport
        if (!parent || !is<SVGElement>(*parent))
            return viewport_size_from_layout(*svg_element);

        // https://w3c.github.io/svgwg/svg2-draft/coords.html#EstablishingANewSVGViewport
        if (is<SVGForeignObjectElement>(*parent))
            return viewport_size_from_layout(*svg_element);
    }

    auto viewport_element = element.owner_svg_element();

    if (!viewport_element)
        return {};

    if (auto view_box = viewport_element->active_view_box(); view_box.has_value() && view_box->width > 0 && view_box->height > 0)
        return { view_box->width, view_box->height };

    return viewport_size_from_layout(*viewport_element);
}

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

    auto viewport = svg_viewport_size(*element);

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
    if (value->is_number())
        return value->as_number().number();

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
    auto error_value = [&] { return WebIDL::NotSupportedError::create(realm(), ""_utf16); };

    if (!length_resolution_context.has_value()) {
        if (!value->is_computationally_independent())
            return error_value();

        // NB: Values which rely on viewport relative lengths are computationally independent but we still can't resolve
        //    them - we pass a bogus LRC and check if absolutization depends on viewport metrics.
        CSS::ComputationContext computation_context {
            .length_resolution_context = { .viewport_rect = { 0, 0, 0, 0 }, .font_metrics = { 0, {}, {} }, .root_font_metrics = { 0, {}, {} } },
        };

        auto absolutized_value = value->absolutized(computation_context);

        if (computation_context.depends_on_viewport_metrics())
            return error_value();

        // AD-HOC: We handle literal numbers in step two but calculated values that resolve to numbers haven't been
        //         handled yet (tree counting functions have been filtered out since they aren't computationally
        //         independent).
        if (absolutized_value->is_calculated() && absolutized_value->as_calculated().resolves_to_number())
            return number_from_style_value(*absolutized_value, {});

        return CSS::Length::from_style_value(absolutized_value, CSS::Length::make_px(viewport_size)).absolute_length_to_px_without_rounding();
    }

    CSS::ComputationContext computation_context {
        .length_resolution_context = length_resolution_context.release_value(),
        .abstract_element = { *m_element },
    };

    // NB: Update layout so any container query units are resolved correctly.
    // NB: We can be sure we have an element since we wouldn't have a length resolution context otherwise.
    m_element->document().update_layout(DOM::UpdateLayoutReason::SVGLengthValue);

    auto absolutized_value = value->absolutized(computation_context);

    // AD-HOC: We handle literal numbers in step two but tree counting functions (which absolutize to NumberStyleValue)
    //         and calculated values that resolve to numbers haven't been handled yet.
    if (absolutized_value->is_number() || (absolutized_value->is_calculated() && absolutized_value->as_calculated().resolves_to_number()))
        return number_from_style_value(*absolutized_value, {});

    return CSS::Length::from_style_value(absolutized_value, CSS::Length::make_px(viewport_size)).absolute_length_to_px_without_rounding();
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
