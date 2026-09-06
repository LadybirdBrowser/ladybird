/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/DecodedImageFrame.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/SVG/SVGComponentTransferFunctionElement.h>
#include <LibWeb/SVG/SVGFEBlendElement.h>
#include <LibWeb/SVG/SVGFEColorMatrixElement.h>
#include <LibWeb/SVG/SVGFEComponentTransferElement.h>
#include <LibWeb/SVG/SVGFECompositeElement.h>
#include <LibWeb/SVG/SVGFEDisplacementMapElement.h>
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

void SVGFilterElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGURIReferenceMixin::visit_edges(visitor);
}

void SVGFilterElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == AttributeNames::filterUnits)
        m_filter_units = parse_units(value.value_or({}));
    else if (name == AttributeNames::primitiveUnits)
        m_primitive_units = parse_units(value.value_or({}));
}

namespace {

// A view of a string's code units for the Rust graph builder, valid for as long as the string is.
Layout::RustFFI::FfiUtf16View view_of(Utf16View view)
{
    return {
        .ascii = view.has_ascii_storage() ? reinterpret_cast<u8 const*>(view.ascii_span().data()) : nullptr,
        .utf16 = view.has_ascii_storage() ? nullptr : reinterpret_cast<u16 const*>(view.utf16_span().data()),
        .length = view.length_in_code_units(),
    };
}

}

// https://drafts.fxtf.org/filter-effects-1/#ColorInterpolationFiltersProperty
void SVGFilterElement::push_primitives(void* sink, Layout::NodeWithStyle const& referenced_node, Vector<Gfx::DecodedImageFrame>& image_frames)
{
    using PrimitiveKind = Layout::RustFFI::FfiSvgFilterPrimitiveKind;

    auto operating_color_space = [](DOM::Element const& element) {
        // linearRGB performs color operations in the linear-light sRGB color space; auto and sRGB use gamma-encoded sRGB.
        auto const* svg_values = element.style_group<CSS::ComputedValues::InheritedSVGValues>();
        auto color_interpolation_filters = svg_values ? svg_values->color_interpolation_filters_value() : CSS::ColorInterpolation::Linearrgb;
        return CSS::to_interpolation_color_space(color_interpolation_filters);
    };

    for_each_child_of_type<DOM::Element>([&](auto& node) {
        Layout::RustFFI::FfiSvgFilterPrimitive primitive {};
        auto& values = primitive.values;
        values.operating_color_space = operating_color_space(node);

        // The builder copies what the primitive's views point at while it is pushed, so this
        // storage only has to outlive the push.
        Utf16String in1;
        Utf16String in2;
        Utf16String result;
        Vector<Utf16String> merge_input_names;
        Vector<Layout::RustFFI::FfiUtf16View> merge_inputs;
        Vector<float> color_matrix_values;

        auto set_in1 = [&](auto& filter_primitive) { in1 = filter_primitive.in1()->base_val(); };
        auto set_in2 = [&](auto& filter_primitive) { in2 = filter_primitive.in2()->base_val(); };
        auto set_result = [&](auto& filter_primitive) { result = filter_primitive.result()->base_val(); };

        if (auto* flood_primitive = as_if<SVGFEFloodElement>(node)) {
            values.kind = PrimitiveKind::Flood;
            values.flood_color = flood_primitive->flood_color();
            values.flood_opacity = flood_primitive->flood_opacity();
            set_result(*flood_primitive);
        } else if (auto* blend_primitive = as_if<SVGFEBlendElement>(node)) {
            values.kind = PrimitiveKind::Blend;
            values.blend_mode = blend_primitive->mode();
            set_in1(*blend_primitive);
            set_in2(*blend_primitive);
            set_result(*blend_primitive);
        } else if (auto* component_transfer = as_if<SVGFEComponentTransferElement>(node)) {
            values.kind = PrimitiveKind::ComponentTransfer;
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
            for (size_t channel = 0; channel < argb_function_elements.size(); ++channel) {
                if (argb_function_elements[channel])
                    primitive.component_transfer_tables[channel] = argb_function_elements[channel]->color_table().data();
            }
            set_in1(*component_transfer);
            set_result(*component_transfer);
        } else if (auto* composite_primitive = as_if<SVGFECompositeElement>(node)) {
            values.kind = PrimitiveKind::Composite;
            values.composite_operator = to_underlying(composite_primitive->operator_());
            values.k1 = composite_primitive->k1()->base_val();
            values.k2 = composite_primitive->k2()->base_val();
            values.k3 = composite_primitive->k3()->base_val();
            values.k4 = composite_primitive->k4()->base_val();
            set_in1(*composite_primitive);
            set_in2(*composite_primitive);
            set_result(*composite_primitive);
        } else if (auto* blur_primitive = as_if<SVGFEGaussianBlurElement>(node)) {
            values.kind = PrimitiveKind::GaussianBlur;
            values.std_deviation_x = blur_primitive->std_deviation_x()->base_val();
            values.std_deviation_y = blur_primitive->std_deviation_y()->base_val();
            set_in1(*blur_primitive);
            set_result(*blur_primitive);
        } else if (auto* colormatrix_primitive = as_if<SVGFEColorMatrixElement>(node)) {
            auto type = colormatrix_primitive->type()->base_val();
            if (type == SVGFEColorMatrixElement::SVG_FECOLORMATRIX_TYPE_UNKNOWN) {
                dbgln("SVGFEColorMatrixElement: Unknown type '{}' — skipping filter primitive", colormatrix_primitive->attribute(AttributeNames::type).value_or({}));
                return IterationDecision::Continue;
            }
            values.kind = PrimitiveKind::ColorMatrix;
            values.color_matrix_type = type;
            color_matrix_values = parse_table_values(colormatrix_primitive->attribute(AttributeNames::values).value_or({}));
            primitive.color_matrix_values = color_matrix_values.data();
            primitive.color_matrix_value_count = color_matrix_values.size();
            set_in1(*colormatrix_primitive);
            set_result(*colormatrix_primitive);
        } else if (auto* image_primitive = as_if<SVGFEImageElement>(node)) {
            auto image_data = image_primitive->image_data();
            if (!image_data)
                return IterationDecision::Continue;

            // FIXME: Should we use the dest rect as the size here?
            auto frame = image_data->default_frame({});
            if (!frame.has_value())
                return IterationDecision::Continue;

            auto src_rect = image_primitive->content_rect();
            if (!src_rect.has_value())
                return IterationDecision::Continue;

            auto* dom_node = referenced_node.dom_node();
            if (!dom_node)
                return IterationDecision::Continue;

            // NB: We use the unsafe accessor here because this may be called
            //     during layout update, before the layout-is-up-to-date flag
            //     has been set. The committed box is valid since layout has
            //     already been performed at this point.
            auto const* layout_node = dom_node->unsafe_layout_node();
            if (!layout_node || !Painting::has_committed_box(*layout_node))
                return IterationDecision::Continue;

            auto dest_rect = Gfx::enclosing_int_rect(Painting::absolute_rect(*layout_node).to_type<float>());
            values.kind = PrimitiveKind::Image;
            values.image_frame_id = frame->id();
            values.image_src_rect = *src_rect;
            values.image_dest_rect = dest_rect;
            values.image_scaling_mode = CSS::to_gfx_scaling_mode(as<Layout::NodeWithStyle>(*layout_node).image_rendering(), src_rect->size(), dest_rect.size());
            image_frames.append(*frame);
            set_result(*image_primitive);
        } else if (auto* merge_primitive = as_if<SVGFEMergeElement>(node)) {
            values.kind = PrimitiveKind::Merge;
            merge_primitive->template for_each_child_of_type<SVGFEMergeNodeElement>([&](auto& merge_node) {
                merge_input_names.append(merge_node.in1()->base_val());
                return IterationDecision::Continue;
            });
            for (auto const& merge_input_name : merge_input_names)
                merge_inputs.append(view_of(merge_input_name));
            primitive.merge_inputs = merge_inputs.data();
            primitive.merge_input_count = merge_inputs.size();
            set_result(*merge_primitive);
        } else if (auto* morphology_primitive = as_if<SVGFEMorphologyElement>(node)) {
            auto morphology_operator = morphology_primitive->morphology_operator();
            if (morphology_operator == Gfx::MorphologyOperator::Unknown) {
                dbgln("SVGFEMorphologyElement: Unknown operator — skipping filter primitive");
                return IterationDecision::Continue;
            }
            values.kind = PrimitiveKind::Morphology;
            values.morphology_operator = to_underlying(morphology_operator);
            values.radius_x = morphology_primitive->radius_x()->base_val();
            values.radius_y = morphology_primitive->radius_y()->base_val();
            set_in1(*morphology_primitive);
            set_result(*morphology_primitive);
        } else if (auto* offset_primitive = as_if<SVGFEOffsetElement>(node)) {
            values.kind = PrimitiveKind::Offset;
            values.dx = offset_primitive->dx()->base_val();
            values.dy = offset_primitive->dy()->base_val();
            set_in1(*offset_primitive);
            set_result(*offset_primitive);
        } else if (auto* drop_shadow = as_if<SVGFEDropShadowElement>(node)) {
            values.kind = PrimitiveKind::DropShadow;
            values.std_deviation_x = drop_shadow->std_deviation_x()->base_val();
            values.std_deviation_y = drop_shadow->std_deviation_y()->base_val();
            values.dx = drop_shadow->dx()->base_val();
            values.dy = drop_shadow->dy()->base_val();
            values.flood_color = drop_shadow->flood_color();
            values.flood_opacity = drop_shadow->flood_opacity();
            set_in1(*drop_shadow);
            set_result(*drop_shadow);
        } else if (auto* turbulence = as_if<SVGFETurbulenceElement>(node)) {
            values.kind = PrimitiveKind::Turbulence;
            values.turbulence_type = turbulence->type()->base_val();
            values.base_frequency_x = turbulence->base_frequency_x()->base_val();
            values.base_frequency_y = turbulence->base_frequency_y()->base_val();
            values.num_octaves = turbulence->num_octaves()->base_val();
            values.seed = turbulence->seed()->base_val();
            if (turbulence->stitch_tiles()->base_val() == to_underlying(SVGFETurbulenceElement::StitchType::Stitch)) {
                // FIXME: Use the correct width and height
                auto maybe_width = turbulence->width()->base_val()->value();
                auto maybe_height = turbulence->height()->base_val()->value();
                values.stitch_tile_width = maybe_width.is_exception() ? 0 : maybe_width.release_value();
                values.stitch_tile_height = maybe_height.is_exception() ? 0 : maybe_height.release_value();
            }
            set_result(*turbulence);
        } else if (auto* displacement_map = as_if<SVGFEDisplacementMapElement>(node)) {
            values.kind = PrimitiveKind::DisplacementMap;
            values.scale = displacement_map->scale()->base_val();
            values.x_channel_selector = displacement_map->x_channel_selector()->base_val();
            values.y_channel_selector = displacement_map->y_channel_selector()->base_val();
            set_in1(*displacement_map);
            set_in2(*displacement_map);
            set_result(*displacement_map);
        } else {
            dbgln("SVGFilterElement::push_primitives(): Unknown or unsupported filter element '{}'", node.debug_description());
            return IterationDecision::Continue;
        }

        primitive.in1 = view_of(in1);
        primitive.in2 = view_of(in2);
        primitive.result = view_of(result);
        Layout::RustFFI::layout_arena_paint_push_svg_filter_primitive(sink, &primitive);
        return IterationDecision::Continue;
    });
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-filter-filterunits
GC::Ref<SVGAnimatedEnumeration> SVGFilterElement::filter_units() const
{
    return SVGAnimatedEnumeration::create(to_underlying(m_filter_units.value_or(SVGUnits::ObjectBoundingBox)));
}

// https://drafts.fxtf.org/filter-effects/#element-attrdef-filter-primitiveunits
GC::Ref<SVGAnimatedEnumeration> SVGFilterElement::primitive_units() const
{
    return SVGAnimatedEnumeration::create(to_underlying(m_primitive_units.value_or(SVGUnits::UserSpaceOnUse)));
}

}
