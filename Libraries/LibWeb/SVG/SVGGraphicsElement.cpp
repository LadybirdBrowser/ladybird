/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGGraphicsElementPrototype.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/PaintStyle.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/SVGGraphicsPaintable.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGMaskElement.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/SVG/SVGSymbolElement.h>

namespace Web::SVG {

SVGGraphicsElement::SVGGraphicsElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, move(qualified_name))
{
}

void SVGGraphicsElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGGraphicsElement);
}

void SVGGraphicsElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == "transform"sv) {
        auto transform_list = AttributeParser::parse_transform(value.value_or(String {}));
        if (transform_list.has_value())
            m_transform = transform_from_transform_list(*transform_list);
        set_needs_layout_tree_update(true);
    }
}

Optional<Painting::PaintStyle> SVGGraphicsElement::svg_paint_computed_value_to_gfx_paint_style(SVGPaintContext const& paint_context, Optional<CSS::SVGPaint> const& paint_value) const
{
    // FIXME: This entire function is an ad-hoc hack:
    if (!paint_value.has_value() || !paint_value->is_url())
        return {};
    if (auto gradient = try_resolve_url_to<SVG::SVGGradientElement const>(paint_value->as_url()))
        return gradient->to_gfx_paint_style(paint_context);
    return {};
}

Optional<Painting::PaintStyle> SVGGraphicsElement::fill_paint_style(SVGPaintContext const& paint_context) const
{
    if (!layout_node())
        return {};
    return svg_paint_computed_value_to_gfx_paint_style(paint_context, layout_node()->computed_values().fill());
}

Optional<Painting::PaintStyle> SVGGraphicsElement::stroke_paint_style(SVGPaintContext const& paint_context) const
{
    if (!layout_node())
        return {};
    return svg_paint_computed_value_to_gfx_paint_style(paint_context, layout_node()->computed_values().stroke());
}

GC::Ptr<SVG::SVGMaskElement const> SVGGraphicsElement::mask() const
{
    auto const& mask_reference = layout_node()->computed_values().mask();
    if (!mask_reference.has_value())
        return {};
    return try_resolve_url_to<SVG::SVGMaskElement const>(mask_reference->url());
}

GC::Ptr<SVG::SVGClipPathElement const> SVGGraphicsElement::clip_path() const
{
    auto const& clip_path_reference = layout_node()->computed_values().clip_path();
    if (!clip_path_reference.has_value() || !clip_path_reference->is_url())
        return {};
    return try_resolve_url_to<SVG::SVGClipPathElement const>(clip_path_reference->url());
}

Gfx::AffineTransform transform_from_transform_list(ReadonlySpan<Transform> transform_list)
{
    Gfx::AffineTransform affine_transform;
    for (auto& transform : transform_list) {
        transform.operation.visit(
            [&](Transform::Translate const& translate) {
                affine_transform.multiply(Gfx::AffineTransform {}.translate({ translate.x, translate.y }));
            },
            [&](Transform::Scale const& scale) {
                affine_transform.multiply(Gfx::AffineTransform {}.scale({ scale.x, scale.y }));
            },
            [&](Transform::Rotate const& rotate) {
                Gfx::AffineTransform translate_transform;
                affine_transform.multiply(
                    Gfx::AffineTransform {}
                        .translate({ rotate.x, rotate.y })
                        .rotate_radians(AK::to_radians(rotate.a))
                        .translate({ -rotate.x, -rotate.y }));
            },
            [&](Transform::SkewX const& skew_x) {
                affine_transform.multiply(Gfx::AffineTransform {}.skew_radians(AK::to_radians(skew_x.a), 0));
            },
            [&](Transform::SkewY const& skew_y) {
                affine_transform.multiply(Gfx::AffineTransform {}.skew_radians(0, AK::to_radians(skew_y.a)));
            },
            [&](Transform::Matrix const& matrix) {
                affine_transform.multiply(Gfx::AffineTransform {
                    matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f });
            });
    }
    return affine_transform;
}

Gfx::AffineTransform SVGGraphicsElement::get_transform() const
{
    Gfx::AffineTransform transform = m_transform;
    for (auto* svg_ancestor = shadow_including_first_ancestor_of_type<SVGGraphicsElement>(); svg_ancestor; svg_ancestor = svg_ancestor->shadow_including_first_ancestor_of_type<SVGGraphicsElement>()) {
        transform = Gfx::AffineTransform { svg_ancestor->element_transform() }.multiply(transform);
    }
    return transform;
}

struct NamedPropertyID {
    NamedPropertyID(CSS::PropertyID property_id)
        : id(property_id)
        , name(CSS::string_from_property_id(property_id))
    {
    }

    CSS::PropertyID id;
    StringView name;
};

static Array const attribute_style_properties {
    // FIXME: The `fill` attribute and CSS `fill` property are not the same! But our support is limited enough that they are equivalent for now.
    NamedPropertyID(CSS::PropertyID::Fill),
    // FIXME: The `stroke` attribute and CSS `stroke` property are not the same! But our support is limited enough that they are equivalent for now.
    NamedPropertyID(CSS::PropertyID::Stroke),
    NamedPropertyID(CSS::PropertyID::StrokeDasharray),
    NamedPropertyID(CSS::PropertyID::StrokeDashoffset),
    NamedPropertyID(CSS::PropertyID::StrokeLinecap),
    NamedPropertyID(CSS::PropertyID::StrokeLinejoin),
    NamedPropertyID(CSS::PropertyID::StrokeMiterlimit),
    NamedPropertyID(CSS::PropertyID::StrokeWidth),
    NamedPropertyID(CSS::PropertyID::FillRule),
    NamedPropertyID(CSS::PropertyID::FillOpacity),
    NamedPropertyID(CSS::PropertyID::StrokeOpacity),
    NamedPropertyID(CSS::PropertyID::Opacity),
    NamedPropertyID(CSS::PropertyID::TextAnchor),
    NamedPropertyID(CSS::PropertyID::FontSize),
    NamedPropertyID(CSS::PropertyID::Mask),
    NamedPropertyID(CSS::PropertyID::MaskType),
    NamedPropertyID(CSS::PropertyID::ClipPath),
    NamedPropertyID(CSS::PropertyID::ClipRule),
    NamedPropertyID(CSS::PropertyID::Display),
};

bool SVGGraphicsElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return any_of(attribute_style_properties, [&](auto& property) { return name.equals_ignoring_ascii_case(property.name); });
}

void SVGGraphicsElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    CSS::Parser::ParsingParams parsing_context { document(), CSS::Parser::ParsingMode::SVGPresentationAttribute };
    for_each_attribute([&](auto& name, auto& value) {
        for (auto property : attribute_style_properties) {
            if (!name.equals_ignoring_ascii_case(property.name))
                continue;
            if (property.id == CSS::PropertyID::Mask) {
                // Mask is a shorthand property in CSS, but parse_css_value does not take that into account. For now,
                // just parse as 'mask-image' as anything else is currently not supported.
                // FIXME: properly parse longhand 'mask' property
                if (auto style_value = parse_css_value(parsing_context, value, CSS::PropertyID::MaskImage)) {
                    cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MaskImage, style_value.release_nonnull());
                }
            } else {
                if (auto style_value = parse_css_value(parsing_context, value, property.id))
                    cascaded_properties->set_property_from_presentational_hint(property.id, style_value.release_nonnull());
            }
            break;
        }
    });
}

static FillRule to_svg_fill_rule(CSS::FillRule fill_rule)
{
    switch (fill_rule) {
    case CSS::FillRule::Nonzero:
        return FillRule::Nonzero;
    case CSS::FillRule::Evenodd:
        return FillRule::Evenodd;
    default:
        VERIFY_NOT_REACHED();
    }
}

Optional<FillRule> SVGGraphicsElement::fill_rule() const
{
    if (!layout_node())
        return {};
    return to_svg_fill_rule(layout_node()->computed_values().fill_rule());
}

Optional<ClipRule> SVGGraphicsElement::clip_rule() const
{
    if (!layout_node())
        return {};
    return to_svg_fill_rule(layout_node()->computed_values().clip_rule());
}

Optional<Gfx::Color> SVGGraphicsElement::fill_color() const
{
    if (!layout_node())
        return {};
    // FIXME: In the working-draft spec, `fill` is intended to be a shorthand, with `fill-color`
    //        being what we actually want to use. But that's not final or widely supported yet.
    return layout_node()->computed_values().fill().map([&](auto& paint) -> Gfx::Color {
        if (!paint.is_color())
            return Color::Black;
        return paint.as_color();
    });
}

Optional<Gfx::Color> SVGGraphicsElement::stroke_color() const
{
    if (!layout_node())
        return {};
    // FIXME: In the working-draft spec, `stroke` is intended to be a shorthand, with `stroke-color`
    //        being what we actually want to use. But that's not final or widely supported yet.
    return layout_node()->computed_values().stroke().map([](auto& paint) -> Gfx::Color {
        if (!paint.is_color())
            return Color::Black;
        return paint.as_color();
    });
}

Optional<float> SVGGraphicsElement::fill_opacity() const
{
    if (!layout_node())
        return {};
    return layout_node()->computed_values().fill_opacity();
}

Optional<CSS::StrokeLinecap> SVGGraphicsElement::stroke_linecap() const
{
    if (!layout_node())
        return {};
    return layout_node()->computed_values().stroke_linecap();
}

Optional<CSS::StrokeLinejoin> SVGGraphicsElement::stroke_linejoin() const
{
    if (!layout_node())
        return {};
    return layout_node()->computed_values().stroke_linejoin();
}

Optional<CSS::NumberOrCalculated> SVGGraphicsElement::stroke_miterlimit() const
{
    if (!layout_node())
        return {};
    return layout_node()->computed_values().stroke_miterlimit();
}

Optional<float> SVGGraphicsElement::stroke_opacity() const
{
    if (!layout_node())
        return {};
    return layout_node()->computed_values().stroke_opacity();
}

float SVGGraphicsElement::resolve_relative_to_viewport_size(CSS::LengthPercentage const& length_percentage) const
{
    // FIXME: Converting to pixels isn't really correct - values should be in "user units"
    //        https://svgwg.org/svg2-draft/coords.html#TermUserUnits
    // Resolved relative to the "Scaled viewport size": https://www.w3.org/TR/2017/WD-fill-stroke-3-20170413/#scaled-viewport-size
    // FIXME: This isn't right, but it's something.
    CSSPixels viewport_width = 0;
    CSSPixels viewport_height = 0;
    if (auto* svg_svg_element = shadow_including_first_ancestor_of_type<SVGSVGElement>()) {
        if (auto svg_svg_layout_node = svg_svg_element->layout_node()) {
            viewport_width = svg_svg_layout_node->computed_values().width().to_px(*svg_svg_layout_node, 0);
            viewport_height = svg_svg_layout_node->computed_values().height().to_px(*svg_svg_layout_node, 0);
        }
    }
    auto scaled_viewport_size = (viewport_width + viewport_height) * CSSPixels(0.5);
    return length_percentage.to_px(*layout_node(), scaled_viewport_size).to_double();
}

Vector<float> SVGGraphicsElement::stroke_dasharray() const
{
    if (!layout_node())
        return {};

    Vector<float> dasharray;
    for (auto const& value : layout_node()->computed_values().stroke_dasharray()) {
        value.visit(
            [&](CSS::LengthPercentage const& length_percentage) {
                dasharray.append(resolve_relative_to_viewport_size(length_percentage));
            },
            [&](CSS::NumberOrCalculated const& number_or_calculated) {
                CSS::CalculationResolutionContext calculation_context {
                    .length_resolution_context = CSS::Length::ResolutionContext::for_layout_node(*layout_node()),
                };
                dasharray.append(number_or_calculated.resolved(calculation_context).value_or(0));
            });
    }

    // https://svgwg.org/svg2-draft/painting.html#StrokeDashing
    // If the list has an odd number of values, then it is repeated to yield an even number of values.
    if (dasharray.size() % 2 == 1)
        dasharray.extend(dasharray);

    // If any value in the list is negative, the <dasharray> value is invalid. If all of the values in the list are zero, then the stroke is rendered as a solid line without any dashing.
    bool all_zero = true;
    for (auto& value : dasharray) {
        if (value < 0)
            return {};
        if (value != 0)
            all_zero = false;
    }
    if (all_zero)
        return {};

    return dasharray;
}

Optional<float> SVGGraphicsElement::stroke_dashoffset() const
{
    if (!layout_node())
        return {};
    return resolve_relative_to_viewport_size(layout_node()->computed_values().stroke_dashoffset());
}

Optional<float> SVGGraphicsElement::stroke_width() const
{
    if (!layout_node())
        return {};
    return resolve_relative_to_viewport_size(layout_node()->computed_values().stroke_width());
}

// https://svgwg.org/svg2-draft/types.html#__svg__SVGGraphicsElement__getBBox
GC::Ref<Geometry::DOMRect> SVGGraphicsElement::get_b_box(Optional<SVGBoundingBoxOptions>)
{
    // FIXME: It should be possible to compute this without layout updates. The bounding box is within the
    // SVG coordinate space (before any viewbox or other transformations), so it should be possible to
    // calculate this from SVG geometry without a full layout tree (at least for simple cases).
    // See: https://svgwg.org/svg2-draft/coords.html#BoundingBoxes
    const_cast<DOM::Document&>(document()).update_layout(DOM::UpdateLayoutReason::SVGGraphicsElementGetBBox);
    if (!layout_node())
        return Geometry::DOMRect::create(realm());
    // Invert the SVG -> screen space transform.
    auto owner_svg_element = this->owner_svg_element();
    if (!owner_svg_element)
        return Geometry::DOMRect::create(realm());
    auto svg_element_rect = owner_svg_element->paintable_box()->absolute_rect();
    auto inverse_transform = static_cast<Painting::SVGGraphicsPaintable&>(*paintable_box()).computed_transforms().svg_to_css_pixels_transform().inverse();
    auto translated_rect = paintable_box()->absolute_rect().to_type<float>().translated(-svg_element_rect.location().to_type<float>());
    if (inverse_transform.has_value())
        translated_rect = inverse_transform->map(translated_rect);
    return Geometry::DOMRect::create(realm(), translated_rect);
}

GC::Ref<SVGAnimatedTransformList> SVGGraphicsElement::transform() const
{
    dbgln("(STUBBED) SVGGraphicsElement::transform(). Called on: {}", debug_description());
    auto base_val = SVGTransformList::create(realm());
    auto anim_val = SVGTransformList::create(realm());
    return SVGAnimatedTransformList::create(realm(), base_val, anim_val);
}

GC::Ptr<Geometry::DOMMatrix> SVGGraphicsElement::get_screen_ctm()
{
    dbgln("(STUBBED) SVGGraphicsElement::get_screen_ctm(). Called on: {}", debug_description());
    return Geometry::DOMMatrix::create(realm());
}

}
