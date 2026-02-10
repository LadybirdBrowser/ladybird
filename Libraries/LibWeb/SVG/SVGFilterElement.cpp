/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringConversions.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibWeb/Bindings/SVGFilterElementPrototype.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/SVG/SVGComponentTransferFunctionElement.h>
#include <LibWeb/SVG/SVGFEBlendElement.h>
#include <LibWeb/SVG/SVGFEColorMatrixElement.h>
#include <LibWeb/SVG/SVGFEComponentTransferElement.h>
#include <LibWeb/SVG/SVGFECompositeElement.h>
#include <LibWeb/SVG/SVGFEDropShadowElement.h>
#include <LibWeb/SVG/SVGFEFloodElement.h>
#include <LibWeb/SVG/SVGFEFuncAElement.h>
#include <LibWeb/SVG/SVGFEFuncBElement.h>
#include <LibWeb/SVG/SVGFEFuncGElement.h>
#include <LibWeb/SVG/SVGFEFuncRElement.h>
#include <LibWeb/SVG/SVGFEGaussianBlurElement.h>
#include <LibWeb/SVG/SVGFEImageElement.h>
#include <LibWeb/SVG/SVGFEMergeElement.h>
#include <LibWeb/SVG/SVGFEMergeNodeElement.h>
#include <LibWeb/SVG/SVGFEMorphologyElement.h>
#include <LibWeb/SVG/SVGFEOffsetElement.h>
#include <LibWeb/SVG/SVGFETurbulenceElement.h>
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
        } else if (auto* component_transfer = as_if<SVGFEComponentTransferElement>(node)) {
            auto input = resolve_input_filter(component_transfer->in1()->base_val());

            // https://drafts.fxtf.org/filter-effects/#feComponentTransferElement
            // * If more than one transfer function element of the same kind is specified, the last occurrence is to be
            //   used.
            // * If any of the transfer function elements are unspecified, the feComponentTransfer must be processed as
            //   if those transfer function elements were specified with their type attributes set to identity.
            Array<GC::Ptr<SVGComponentTransferFunctionElement>, 4> argb_function_elements;
            node.for_each_child([&](auto& child) {
                if (auto* func_a = as_if<SVGFEFuncAElement>(child))
                    argb_function_elements[0] = func_a;
                else if (auto* func_r = as_if<SVGFEFuncRElement>(child))
                    argb_function_elements[1] = func_r;
                else if (auto* func_g = as_if<SVGFEFuncGElement>(child))
                    argb_function_elements[2] = func_g;
                else if (auto* func_b = as_if<SVGFEFuncBElement>(child))
                    argb_function_elements[3] = func_b;
                return IterationDecision::Continue;
            });

            root_filter = Gfx::Filter::color_table(
                argb_function_elements[0] ? argb_function_elements[0]->color_table() : Optional<ReadonlyBytes> {},
                argb_function_elements[1] ? argb_function_elements[1]->color_table() : Optional<ReadonlyBytes> {},
                argb_function_elements[2] ? argb_function_elements[2]->color_table() : Optional<ReadonlyBytes> {},
                argb_function_elements[3] ? argb_function_elements[3]->color_table() : Optional<ReadonlyBytes> {},
                input);
            update_result_map(*component_transfer);
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
        } else if (auto* colormatrix_primitive = as_if<SVGFEColorMatrixElement>(node)) {
            auto in_attr = colormatrix_primitive->in1()->base_val();
            auto input = resolve_input_filter(in_attr);

            auto type_value = colormatrix_primitive->attribute(AttributeNames::type).value_or(String {});
            auto values_value = colormatrix_primitive->attribute(AttributeNames::values).value_or(String {});

            // Default type is "matrix" per spec.
            if (type_value.is_empty() || type_value.equals_ignoring_ascii_case("matrix"sv)) {
                // Parse up to 20 numbers; if we don't get a full 4x5, skip applying.
                float matrix[20] = { 0 };
                size_t count = 0;

                StringView sv = values_value;
                auto skip_leading_whitespace = [&] {
                    sv = sv.trim_whitespace(AK::TrimMode::Left);
                };
                auto consume_comma_and_whitespace = [&] {
                    if (!sv.is_empty() && sv[0] == ',')
                        sv = sv.substring_view(1);
                    skip_leading_whitespace();
                };

                skip_leading_whitespace();
                while (!sv.is_empty() && count < 20) {
                    // Parse the next number without trimming (we already trimmed on the left).
                    auto result = AK::parse_first_number<float>(sv, AK::TrimWhitespace::No);
                    if (!result.has_value())
                        break;
                    matrix[count++] = result->value;
                    // Advance exactly past the number just parsed, then consume optional comma + whitespace.
                    sv = sv.substring_view(result->characters_parsed);
                    consume_comma_and_whitespace();
                }

                if (count == 20) {
                    root_filter = Gfx::Filter::color_matrix(matrix, input);
                    update_result_map(*colormatrix_primitive);
                } else {
                    // If invalid or missing, treat as identity (no-op) if we already have an input.
                    if (input.has_value()) {
                        root_filter = input;
                        update_result_map(*colormatrix_primitive);
                    }
                }
            } else if (type_value.equals_ignoring_ascii_case("saturate"sv)) {
                // values: single number s (1 = original)
                float s = 1.0f;
                if (!values_value.is_empty()) {
                    if (auto parsed = AK::parse_number<float>(values_value, AK::TrimWhitespace::Yes); parsed.has_value())
                        s = *parsed;
                }
                root_filter = Gfx::Filter::saturate(s, input);
                update_result_map(*colormatrix_primitive);
            } else if (type_value.equals_ignoring_ascii_case("hueRotate"sv)) {
                // values: angle in degrees
                float angle_degrees = 0.0f;
                if (!values_value.is_empty()) {
                    if (auto parsed = AK::parse_number<float>(values_value, AK::TrimWhitespace::Yes); parsed.has_value())
                        angle_degrees = *parsed;
                }
                root_filter = Gfx::Filter::hue_rotate(angle_degrees, input);
                update_result_map(*colormatrix_primitive);
            } else if (type_value.equals_ignoring_ascii_case("luminanceToAlpha"sv)) {
                // values ignored; convert luminance to alpha and zero RGB.
                float matrix[20] = {
                    0, 0, 0, 0, 0,
                    0, 0, 0, 0, 0,
                    0, 0, 0, 0, 0,
                    0.2126f, 0.7152f, 0.0722f, 0, 0
                };
                root_filter = Gfx::Filter::color_matrix(matrix, input);
                update_result_map(*colormatrix_primitive);
            } else {
                // Unknown 'type' value on feColorMatrix; skip creating a filter and log.
                dbgln("SVGFEColorMatrixElement: Unknown type '{}' — skipping filter primitive", type_value);
            }
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
            auto scaling_mode = CSS::to_gfx_scaling_mode(paintable_box->computed_values().image_rendering(), src_rect->size(), dest_rect.size());
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
        } else if (auto* morphology_primitive = as_if<SVGFEMorphologyElement>(node)) {
            auto input = resolve_input_filter(morphology_primitive->in1()->base_val());

            auto radius_x = morphology_primitive->radius_x()->base_val();
            auto radius_y = morphology_primitive->radius_y()->base_val();
            auto morphology_operator = morphology_primitive->morphology_operator();
            switch (morphology_operator) {
            case Gfx::MorphologyOperator::Erode:
                root_filter = Gfx::Filter::erode(radius_x, radius_y, input);
                break;
            case Gfx::MorphologyOperator::Dilate:
                root_filter = Gfx::Filter::dilate(radius_x, radius_y, input);
                break;
            case Gfx::MorphologyOperator::Unknown:
                VERIFY_NOT_REACHED();
            }

            update_result_map(*morphology_primitive);
        } else if (auto* offset_primitive = as_if<SVGFEOffsetElement>(node)) {
            auto input = resolve_input_filter(offset_primitive->in1()->base_val());

            auto dx = offset_primitive->dx()->base_val();
            auto dy = offset_primitive->dy()->base_val();

            root_filter = Gfx::Filter::offset(dx, dy, input);
            update_result_map(*offset_primitive);
        } else if (auto* drop_shadow = as_if<SVGFEDropShadowElement>(node)) {
            // https://drafts.csswg.org/filter-effects-1/#elementdef-fedropshadow
            auto input = resolve_input_filter(drop_shadow->in1()->base_val());
            // 1. Take the alpha channel of the input to the feDropShadow filter primitive and the stdDeviation on the
            //    feDropShadow and do processing as if the following feGaussianBlur was applied:
            //
            // <feGaussianBlur in="alpha-channel-of-feDropShadow-in" stdDeviation="stdDeviation-of-feDropShadow"/>
            float alpha_matrix[20] = {
                0, 0, 0, 0, 0,
                0, 0, 0, 0, 0,
                0, 0, 0, 0, 0,
                0, 0, 0, 1, 0
            };
            auto alpha_input = Gfx::Filter::color_matrix(alpha_matrix, input);
            auto std_x = drop_shadow->std_deviation_x()->base_val();
            auto std_y = drop_shadow->std_deviation_y()->base_val();
            auto blurred = Gfx::Filter::blur(std_x, std_y, alpha_input);

            // 2. Offset the result of step 1 by dx and dy as specified on the feDropShadow element, equivalent to
            //    applying an feOffset with these parameters:
            //
            // <feOffset dx="dx-of-feDropShadow" dy="dy-of-feDropShadow" result="offsetblur"/>
            auto dx = drop_shadow->dx()->base_val();
            auto dy = drop_shadow->dy()->base_val();
            auto offset_blur = Gfx::Filter::offset(dx, dy, blurred);

            // 3. Do processing as if an feFlood element with flood-color and flood-opacity as specified on the
            //    feDropShadow was applied:
            //
            // <feFlood flood-color="flood-color-of-feDropShadow" flood-opacity="flood-opacity-of-feDropShadow"/>
            auto shadow_color = Gfx::Filter::flood(drop_shadow->flood_color(), drop_shadow->flood_opacity());

            // 4. Composite the result of the feFlood in step 3 with the result of the feOffset in step 2 as if an
            //    feComposite filter primitive with operator="in" was applied:
            //
            // <feComposite in2="offsetblur" operator="in"/>
            auto colored_shadow = Gfx::Filter::blend(offset_blur, shadow_color, Gfx::CompositingAndBlendingOperator::SourceIn);

            // 5. Finally merge the result of the previous step, doing processing as if the following feMerge was performed:
            //
            // <feMerge>
            //   <feMergeNode/>
            //   <feMergeNode in="in-of-feDropShadow"/>
            // </feMerge>
            root_filter = Gfx::Filter::merge({ colored_shadow, input });
            update_result_map(*drop_shadow);
        } else if (auto* turbulence = as_if<SVGFETurbulenceElement>(node)) {
            auto base_frequency_x = turbulence->base_frequency_x()->base_val();
            auto base_frequency_y = turbulence->base_frequency_y()->base_val();
            auto num_octaves = turbulence->num_octaves()->base_val();
            auto seed = turbulence->seed()->base_val();

            auto type = [turbulence] {
                auto turbulence_type = turbulence->type()->base_val();
                switch (turbulence_type) {
                case to_underlying(SVGFETurbulenceElement::TurbulenceType::Turbulence):
                    return Gfx::TurbulenceType::Turbulence;
                case to_underlying(SVGFETurbulenceElement::TurbulenceType::FractalNoise):
                    return Gfx::TurbulenceType::FractalNoise;
                default:
                    VERIFY_NOT_REACHED();
                }
            }();

            auto tile_stitch_size = [turbulence] {
                auto stitch_tiles = turbulence->stitch_tiles()->base_val();
                switch (stitch_tiles) {
                case to_underlying(SVGFETurbulenceElement::StitchType::Stitch):
                    // FIXME: Are these the correct width and height?
                    return Gfx::IntSize { turbulence->width()->base_val()->value(), turbulence->height()->base_val()->value() };
                case to_underlying(SVGFETurbulenceElement::StitchType::NoStitch):
                    return Gfx::IntSize {};
                default:
                    VERIFY_NOT_REACHED();
                }
            }();

            root_filter = Gfx::Filter::turbulence(type, base_frequency_x, base_frequency_y, num_octaves, seed, tile_stitch_size);
            update_result_map(*turbulence);
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
