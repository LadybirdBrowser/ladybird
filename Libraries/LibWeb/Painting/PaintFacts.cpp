/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageSetStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/ImageProvider.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/PaintFacts.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>
#include <LibWeb/SVG/SVGImageElement.h>

namespace Web::Painting {

static bool paints_form_control_from_facts(Layout::Node const& layout_node)
{
    return layout_node.kind() == Layout::RustFFI::NodeKind::CheckBox || layout_node.kind() == Layout::RustFFI::NodeKind::RadioButton;
}

static void push_form_control_paint_facts_onto(HTML::HTMLInputElement const& input, Layout::Node const& layout_node)
{
    Layout::RustFFI::FfiFormControlPaintFacts facts {
        .enabled = input.enabled(),
        .checked = input.checked(),
        .indeterminate = input.indeterminate(),
        .being_activated = input.is_being_activated(),
    };
    Layout::RustFFI::layout_arena_set_form_control_paint_facts(layout_node.arena_handle(), Layout::Node::slot_id(&layout_node), facts);
}

void push_form_control_paint_facts(HTML::HTMLInputElement& input)
{
    auto const* layout_node = input.unsafe_layout_node();
    if (!layout_node || !paints_form_control_from_facts(*layout_node))
        return;
    push_form_control_paint_facts_onto(input, *layout_node);
}

static void push_canvas_paint_facts_onto(HTML::HTMLCanvasElement const& canvas, Layout::Node const& layout_node)
{
    Layout::RustFFI::FfiCanvasPaintFacts facts {};
    if (auto content_size = canvas.canvas_surface_content_size(); content_size.has_value()) {
        facts.has_content = true;
        facts.content_width = content_size->width();
        facts.content_height = content_size->height();
        facts.canvas_id = canvas.canvas_id().value().value();
        facts.content_generation = canvas.content_generation();
    }
    bool changed = Layout::RustFFI::layout_arena_set_canvas_paint_facts(layout_node.arena_handle(), Layout::Node::slot_id(&layout_node), facts);
    if (changed && has_committed_box(layout_node))
        invalidate_paint_cache(layout_node);
}

void push_canvas_paint_facts(HTML::HTMLCanvasElement const& canvas)
{
    auto const* layout_node = canvas.unsafe_layout_node();
    if (!layout_node || layout_node->kind() != Layout::RustFFI::NodeKind::CanvasBox)
        return;
    push_canvas_paint_facts_onto(canvas, *layout_node);
}

static Optional<u64> composited_context_id_for_navigable_container(HTML::NavigableContainer const& navigable_container)
{
    auto content_navigable = navigable_container.content_navigable();
    if (!content_navigable)
        return {};
    auto const& local_navigable = as<HTML::LocalNavigable>(*content_navigable);
    if (local_navigable.has_been_destroyed())
        return {};
    auto context_id = navigable_container.document().page().client().compositor_context_id_for_remote_child_frame(content_navigable->id());
    if (!context_id.has_value() && local_navigable.has_compositor_context()) {
        auto const* hosted_document = navigable_container.content_document_without_origin_check();
        if (!hosted_document || !hosted_document->is_render_blocked())
            context_id = local_navigable.compositor_context().id();
    }
    if (!context_id.has_value())
        return {};
    return context_id->value();
}

void reconcile_navigable_container_paint_facts(DOM::Document const& document)
{
    for (auto const* navigable_container : HTML::NavigableContainer::all_instances()) {
        if (&navigable_container->document() != &document)
            continue;
        auto const* layout_node = navigable_container->layout_node();
        if (!layout_node || !is_navigable_container_viewport_paintable(*layout_node))
            continue;
        Layout::RustFFI::FfiNavigableContainerPaintFacts facts {};
        if (auto context_id = composited_context_id_for_navigable_container(*navigable_container); context_id.has_value()) {
            facts.has_composited_context = true;
            facts.composited_context_id = *context_id;
        }
        bool changed = Layout::RustFFI::layout_arena_set_navigable_container_paint_facts(layout_node->arena_handle(), Layout::Node::slot_id(layout_node), facts);
        if (changed)
            invalidate_paint_cache(*layout_node);
    }
}

static Layout::RustFFI::FfiLayerImagePaintFacts layer_image_paint_facts_for(CSS::AbstractImageStyleValue const& image, GC::Ptr<HTML::DecodedImageData> decoded_image_data)
{
    Layout::RustFFI::FfiLayerImagePaintFacts facts {};
    facts.is_paintable = image.is_paintable(decoded_image_data);
    if (decoded_image_data) {
        auto natural_size = image.natural_size(*decoded_image_data);
        facts.natural_width = natural_size.width;
        facts.natural_height = natural_size.height;
        if (natural_size.aspect_ratio.has_value()) {
            facts.has_natural_aspect_ratio = true;
            facts.natural_aspect_ratio_numerator = natural_size.aspect_ratio->numerator();
            facts.natural_aspect_ratio_denominator = natural_size.aspect_ratio->denominator();
        }
        if (auto const* svg_image_data = as_if<SVG::SVGDecodedImageData>(*decoded_image_data)) {
            facts.content_kind = Layout::RustFFI::FfiImageContentKind::Vector;
            facts.vector_content_identity = svg_image_data->vector_content_identity();
        } else {
            facts.content_kind = Layout::RustFFI::FfiImageContentKind::Raster;
        }
        facts.single_pixel_color = decoded_image_data->color_if_single_pixel_bitmap();
    }
    if (auto const* image_set = as_if<CSS::ImageSetStyleValue>(image)) {
        if (auto selected_option_index = image_set->selected_option_index(); selected_option_index.has_value()) {
            facts.has_image_set_selected_option = true;
            facts.image_set_selected_option_index = *selected_option_index;
        }
    }
    return facts;
}

static GC::Ptr<HTML::DecodedImageData> decoded_image_data_of(Layout::NodeWithStyle::ImageObserver const* observer)
{
    if (!observer)
        return nullptr;
    return observer->decoded_image_data();
}

void push_layer_image_paint_facts(Layout::NodeWithStyle const& layout_node)
{
    Vector<Layout::RustFFI::FfiLayerImagePaintFactsEntry> entries;
    Vector<Optional<Gfx::DecodedImageFrame>> current_frames;
    auto append_entry = [&](Layout::RustFFI::FfiLayerImageList list, size_t computed_index, CSS::AbstractImageStyleValue const* image, Layout::NodeWithStyle::ImageObserver const* observer) {
        if (!image)
            return;
        auto decoded_image_data = decoded_image_data_of(observer);
        entries.append({
            .list = list,
            .computed_index = static_cast<u32>(computed_index),
            .facts = layer_image_paint_facts_for(*image, decoded_image_data),
        });
        Optional<Gfx::DecodedImageFrame> current_frame;
        if (entries.last().facts.content_kind == Layout::RustFFI::FfiImageContentKind::Raster)
            current_frame = decoded_image_data->current_frame();
        current_frames.append(move(current_frame));
    };
    auto const& background_layers = layout_node.background_layers();
    for (size_t layer_index = 0; layer_index < background_layers.size(); ++layer_index)
        append_entry(Layout::RustFFI::FfiLayerImageList::Background, layer_index, background_layers[layer_index].background_image.ptr(), layout_node.background_image_observer(layer_index));
    auto const& mask_layers = layout_node.mask_layers();
    for (size_t layer_index = 0; layer_index < mask_layers.size(); ++layer_index)
        append_entry(Layout::RustFFI::FfiLayerImageList::Mask, layer_index, mask_layers[layer_index].background_image.ptr(), layout_node.mask_image_observer(layer_index));
    append_entry(Layout::RustFFI::FfiLayerImageList::BorderImageSource, 0, layout_node.border_image().source.ptr(), layout_node.border_image_source_observer());
    for (size_t entry_index = 0; entry_index < entries.size(); ++entry_index) {
        if (current_frames[entry_index].has_value())
            entries[entry_index].facts.frame = &current_frames[entry_index].value();
    }
    Layout::RustFFI::layout_arena_set_layer_image_paint_facts(layout_node.arena_handle(), Layout::Node::slot_id(&layout_node), entries.data(), entries.size());
}

bool push_replaced_image_paint_facts(Layout::ImageProvider const& image_provider, Layout::Node const& layout_node)
{
    if (layout_node.kind() != Layout::RustFFI::NodeKind::ImageBox && layout_node.kind() != Layout::RustFFI::NodeKind::SVGImageBox)
        return false;
    Layout::RustFFI::FfiReplacedImagePaintFacts facts {};
    auto decoded_image_data = image_provider.decoded_image_data();
    facts.has_decoded_image_data = decoded_image_data != nullptr;
    facts.natural_width = image_provider.intrinsic_width();
    facts.natural_height = image_provider.intrinsic_height();
    if (auto aspect_ratio = image_provider.intrinsic_aspect_ratio(); aspect_ratio.has_value()) {
        facts.has_natural_aspect_ratio = true;
        facts.natural_aspect_ratio_numerator = aspect_ratio->numerator();
        facts.natural_aspect_ratio_denominator = aspect_ratio->denominator();
    }
    Optional<Gfx::DecodedImageFrame> current_frame;
    if (decoded_image_data) {
        if (auto const* svg_image_data = as_if<SVG::SVGDecodedImageData>(*decoded_image_data)) {
            facts.content_kind = Layout::RustFFI::FfiImageContentKind::Vector;
            facts.vector_content_identity = svg_image_data->vector_content_identity();
        } else {
            facts.content_kind = Layout::RustFFI::FfiImageContentKind::Raster;
            current_frame = decoded_image_data->current_frame();
            if (current_frame.has_value())
                facts.frame = &current_frame.value();
        }
    }
    return Layout::RustFFI::layout_arena_set_replaced_image_paint_facts(layout_node.arena_handle(), Layout::Node::slot_id(&layout_node), facts);
}

void push_paint_facts_after_style_attach(Layout::NodeWithStyle& layout_node, StyleHoldsImageValues style_holds_image_values)
{
    if (style_holds_image_values == StyleHoldsImageValues::Yes)
        push_layer_image_paint_facts(layout_node);
    else
        Layout::RustFFI::layout_arena_set_layer_image_paint_facts(layout_node.arena_handle(), Layout::Node::slot_id(&layout_node), nullptr, 0);
    if (paints_form_control_from_facts(layout_node))
        push_form_control_paint_facts_onto(as<HTML::HTMLInputElement>(*layout_node.dom_node()), layout_node);
    else if (layout_node.kind() == Layout::RustFFI::NodeKind::CanvasBox)
        push_canvas_paint_facts_onto(as<HTML::HTMLCanvasElement>(*layout_node.dom_node()), layout_node);
    else if (layout_node.kind() == Layout::RustFFI::NodeKind::ImageBox)
        push_replaced_image_paint_facts(static_cast<Layout::Box const&>(layout_node).image_provider(), layout_node);
    else if (layout_node.kind() == Layout::RustFFI::NodeKind::SVGImageBox)
        push_replaced_image_paint_facts(as<SVG::SVGImageElement>(*layout_node.dom_node()), layout_node);
}

}
