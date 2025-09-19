/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <LibWeb/Bindings/SVGFilterElementPrototype.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/SVG/SVGFEBlendElement.h>
#include <LibWeb/SVG/SVGFECompositeElement.h>
#include <LibWeb/SVG/SVGFEFloodElement.h>
#include <LibWeb/SVG/SVGFEGaussianBlurElement.h>
#include <LibWeb/SVG/SVGFEImageElement.h>
#include <LibWeb/SVG/SVGFEMergeElement.h>
#include <LibWeb/SVG/SVGFEMergeNodeElement.h>
#include <LibWeb/SVG/SVGFEOffsetElement.h>
#include <LibWeb/SVG/SVGFilterElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFilterElement);

SVGFilterElement::SVGFilterElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFilterElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFilterElement);
    Base::initialize(realm);
}

void SVGFilterElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
}

void SVGFilterElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    Base::apply_presentational_hints(cascaded_properties);
    auto parsing_context = CSS::Parser::ParsingParams { document(), CSS::Parser::ParsingMode::SVGPresentationAttribute };

    auto x_attribute = attribute(AttributeNames::x);
    if (auto x_value = parse_css_value(parsing_context, x_attribute.value_or(String {}), CSS::PropertyID::X))
        cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::X, x_value.release_nonnull());

    auto y_attribute = attribute(AttributeNames::y);
    if (auto y_value = parse_css_value(parsing_context, y_attribute.value_or(String {}), CSS::PropertyID::Y))
        cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Y, y_value.release_nonnull());

    auto width_attribute = attribute(AttributeNames::width);
    if (auto width_value = parse_css_value(parsing_context, width_attribute.value_or(String {}), CSS::PropertyID::Width))
        cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Width, width_value.release_nonnull());

    auto height_attribute = attribute(AttributeNames::height);
    if (auto height_value = parse_css_value(parsing_context, height_attribute.value_or(String {}), CSS::PropertyID::Height))
        cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Height, height_value.release_nonnull());
}

bool SVGFilterElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return name.is_one_of(AttributeNames::x, AttributeNames::y, AttributeNames::width, AttributeNames::height);
}

void SVGFilterElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == AttributeNames::filterUnits)
        m_filter_units = AttributeParser::parse_units(value.value_or({}));
    else if (name == AttributeNames::primitiveUnits)
        m_primitive_units = AttributeParser::parse_units(value.value_or({}));
}

Optional<Gfx::Filter> SVGFilterElement::gfx_filter(Layout::NodeWithStyle const& referenced_node)
{
    HashMap<String, Gfx::Filter> result_map;
    Optional<Gfx::Filter> root_filter;

    auto update_result_map = [&](auto& filter_primitive) {
        auto result = filter_primitive.result()->base_val();
        if (!result.is_empty())
            result_map.set(result, *root_filter);
    };

    // https://www.w3.org/TR/filter-effects-1/#element-attrdef-filter-primitive-in
    auto resolve_input_filter = [&](String const& name) -> Optional<Gfx::Filter> {
        // TODO: Add missing ones.
        if (name == "SourceGraphic"sv)
            return {};
        if (name == "SourceAlpha"sv) {
            float matrix[20] = {
                0, 0, 0, 0, 0,
                0, 0, 0, 0, 0,
                0, 0, 0, 0, 0,
                0, 0, 0, 1, 0
            };
            return Gfx::Filter::color_matrix(matrix);
        }

        auto filter_from_map = result_map.get(name);
        if (filter_from_map.has_value())
            return filter_from_map.value();

        return root_filter;
    };

    for_each_child_of_type<DOM::Element>([&](auto& node) {
        if (auto* flood_primitive = as_if<SVGFEFloodElement>(node)) {
            root_filter = Gfx::Filter::flood(flood_primitive->flood_color(), flood_primitive->flood_opacity());
            update_result_map(*flood_primitive);
        } else if (auto* blend_primitive = as_if<SVGFEBlendElement>(node)) {
            auto foreground = resolve_input_filter(blend_primitive->in1()->base_val());
            auto background = resolve_input_filter(blend_primitive->in2()->base_val());
            auto blend_mode = blend_primitive->mode();

            root_filter = Gfx::Filter::blend(background, foreground, blend_mode);
            update_result_map(*blend_primitive);
        } else if (auto* composite_primitive = as_if<SVGFECompositeElement>(node)) {
            auto foreground = resolve_input_filter(composite_primitive->in1()->base_val());
            auto background = resolve_input_filter(composite_primitive->in2()->base_val());
            auto operator_ = composite_primitive->operator_();
            if (operator_ == SVGFECompositeElement::CompositingOperator::Arithmetic) {
                auto k1 = composite_primitive->k1()->base_val();
                auto k2 = composite_primitive->k2()->base_val();
                auto k3 = composite_primitive->k3()->base_val();
                auto k4 = composite_primitive->k4()->base_val();

                root_filter = Gfx::Filter::arithmetic(background, foreground, k1, k2, k3, k4);
            } else {
                auto to_compositing_and_blending_operator = [](SVGFECompositeElement::CompositingOperator operator_) {
                    switch (operator_) {
                    case SVGFECompositeElement::CompositingOperator::Over:
                        return Gfx::CompositingAndBlendingOperator::SourceOver;
                    case SVGFECompositeElement::CompositingOperator::In:
                        return Gfx::CompositingAndBlendingOperator::SourceIn;
                    case SVGFECompositeElement::CompositingOperator::Out:
                        return Gfx::CompositingAndBlendingOperator::DestinationOut;
                    case SVGFECompositeElement::CompositingOperator::Atop:
                        return Gfx::CompositingAndBlendingOperator::SourceATop;
                    case SVGFECompositeElement::CompositingOperator::Xor:
                        return Gfx::CompositingAndBlendingOperator::Xor;
                    case SVGFECompositeElement::CompositingOperator::Lighter:
                        return Gfx::CompositingAndBlendingOperator::Lighter;
                    default:
                        break;
                    }
                    return Gfx::CompositingAndBlendingOperator::SourceOver;
                };

                root_filter = Gfx::Filter::blend(background, foreground, to_compositing_and_blending_operator(operator_));
            }

            update_result_map(*composite_primitive);
        } else if (auto* blur_primitive = as_if<SVGFEGaussianBlurElement>(node)) {
            auto input = resolve_input_filter(blur_primitive->in1()->base_val());

            auto radius_x = blur_primitive->std_deviation_x()->base_val();
            auto radius_y = blur_primitive->std_deviation_y()->base_val();

            root_filter = Gfx::Filter::blur(radius_x, radius_y, input);
            update_result_map(*blur_primitive);
        } else if (auto* image_primitive = as_if<SVGFEImageElement>(node)) {
            auto bitmap = image_primitive->current_image_bitmap({});
            if (!bitmap)
                return IterationDecision::Continue;

            auto src_rect = image_primitive->content_rect();
            if (!src_rect.has_value())
                return IterationDecision::Continue;

            auto* dom_node = referenced_node.dom_node();
            if (!dom_node)
                return IterationDecision::Continue;

            auto* paintable_box = dom_node->paintable_box();
            if (!paintable_box)
                return IterationDecision::Continue;

            auto dest_rect = Gfx::enclosing_int_rect(paintable_box->absolute_rect().to_type<float>());
            auto scaling_mode = CSS::to_gfx_scaling_mode(paintable_box->computed_values().image_rendering(), *src_rect, dest_rect);
            root_filter = Gfx::Filter::image(*bitmap, *src_rect, dest_rect, scaling_mode);
            update_result_map(*image_primitive);
        } else if (auto* merge_primitive = as_if<SVGFEMergeElement>(node)) {
            Vector<Optional<Gfx::Filter>> merge_inputs;
            merge_primitive->template for_each_child_of_type<SVGFEMergeNodeElement>([&](auto& merge_node) {
                merge_inputs.append(resolve_input_filter(merge_node.in1()->base_val()));
                return IterationDecision::Continue;
            });

            root_filter = Gfx::Filter::merge(merge_inputs);
            update_result_map(*merge_primitive);
        } else if (auto* offset_primitive = as_if<SVGFEOffsetElement>(node)) {
            auto input = resolve_input_filter(offset_primitive->in1()->base_val());

            auto dx = offset_primitive->dx()->base_val();
            auto dy = offset_primitive->dy()->base_val();

            root_filter = Gfx::Filter::offset(dx, dy, input);
            update_result_map(*offset_primitive);
        } else {
            dbgln("SVGFilterElement::gfx_filter(): Unknown or unsupported filter element '{}'", node.debug_description());
        }

        return IterationDecision::Continue;
    });

    return root_filter;
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-filter-filterunits
GC::Ref<SVGAnimatedEnumeration> SVGFilterElement::filter_units() const
{
    return SVGAnimatedEnumeration::create(realm(), to_underlying(m_filter_units.value_or(SVGUnits::ObjectBoundingBox)));
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-filter-primitiveunits
GC::Ref<SVGAnimatedEnumeration> SVGFilterElement::primitive_units() const
{
    return SVGAnimatedEnumeration::create(realm(), to_underlying(m_primitive_units.value_or(SVGUnits::UserSpaceOnUse)));
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-filter-x
GC::Ref<SVGAnimatedLength> SVGFilterElement::x() const
{
    return svg_animated_length_for_property(CSS::PropertyID::X);
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-filter-y
GC::Ref<SVGAnimatedLength> SVGFilterElement::y() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Y);
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-filter-width
GC::Ref<SVGAnimatedLength> SVGFilterElement::width() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Width);
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-filter-height
GC::Ref<SVGAnimatedLength> SVGFilterElement::height() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Height);
}

}
