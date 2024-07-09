/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
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

void SVGGraphicsElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value)
{
    SVGElement::attribute_changed(name, old_value, value);
    if (name == "transform"sv) {
        auto transform_list = AttributeParser::parse_transform(value.value_or(String {}));
        if (transform_list.has_value())
            m_transform = transform_from_transform_list(*transform_list);
        // FIXME: This should only invalidate the contents of the SVG.
        document().invalidate_layout();
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

JS::GCPtr<SVG::SVGMaskElement const> SVGGraphicsElement::mask() const
{
    auto const& mask_reference = layout_node()->computed_values().mask();
    if (!mask_reference.has_value())
        return {};
    return try_resolve_url_to<SVG::SVGMaskElement const>(mask_reference->url());
}

JS::GCPtr<SVG::SVGClipPathElement const> SVGGraphicsElement::clip_path() const
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

void SVGGraphicsElement::apply_presentational_hints(CSS::StyleProperties& style) const
{
    static Array const attribute_style_properties {
        // FIXME: The `fill` attribute and CSS `fill` property are not the same! But our support is limited enough that they are equivalent for now.
        NamedPropertyID(CSS::PropertyID::Fill),
        // FIXME: The `stroke` attribute and CSS `stroke` property are not the same! But our support is limited enough that they are equivalent for now.
        NamedPropertyID(CSS::PropertyID::Stroke),
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
    };

    CSS::Parser::ParsingContext parsing_context { document(), CSS::Parser::ParsingContext::Mode::SVGPresentationAttribute };
    for_each_attribute([&](auto& name, auto& value) {
        for (auto property : attribute_style_properties) {
            if (!name.equals_ignoring_ascii_case(property.name))
                continue;
            if (auto style_value = parse_css_value(parsing_context, value, property.id))
                style.set_property(property.id, style_value.release_nonnull());
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

Optional<float> SVGGraphicsElement::stroke_opacity() const
{
    if (!layout_node())
        return {};
    return layout_node()->computed_values().stroke_opacity();
}

Optional<float> SVGGraphicsElement::stroke_width() const
{
    if (!layout_node())
        return {};
    // FIXME: Converting to pixels isn't really correct - values should be in "user units"
    //        https://svgwg.org/svg2-draft/coords.html#TermUserUnits
    auto width = layout_node()->computed_values().stroke_width();
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
    return width.to_px(*layout_node(), scaled_viewport_size).to_double();
}

JS::NonnullGCPtr<Geometry::DOMRect> SVGGraphicsElement::get_b_box(Optional<SVGBoundingBoxOptions>)
{
    // FIXME: It should be possible to compute this without layout updates. The bounding box is within the
    // SVG coordinate space (before any viewbox or other transformations), so it should be possible to
    // calculate this from SVG geometry without a full layout tree (at least for simple cases).
    // See: https://svgwg.org/svg2-draft/coords.html#BoundingBoxes
    const_cast<DOM::Document&>(document()).update_layout();
    if (!layout_node())
        return Geometry::DOMRect::create(realm());
    // Invert the SVG -> screen space transform.
    auto svg_element_rect = shadow_including_first_ancestor_of_type<SVG::SVGSVGElement>()->paintable_box()->absolute_rect();
    auto inverse_transform = static_cast<Painting::SVGGraphicsPaintable&>(*paintable_box()).computed_transforms().svg_to_css_pixels_transform().inverse();
    return Geometry::DOMRect::create(realm(),
        inverse_transform->map(
            paintable_box()->absolute_rect().to_type<float>().translated(-svg_element_rect.location().to_type<float>())));
}

JS::NonnullGCPtr<SVGAnimatedTransformList> SVGGraphicsElement::transform() const
{
    dbgln("(STUBBED) SVGGraphicsElement::transform(). Called on: {}", debug_description());
    auto base_val = SVGTransformList::create(realm());
    auto anim_val = SVGTransformList::create(realm());
    return SVGAnimatedTransformList::create(realm(), base_val, anim_val);
}

}
