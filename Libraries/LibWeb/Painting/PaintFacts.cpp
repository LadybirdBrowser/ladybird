/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageSetStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/InvalidationJournal.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/HTML/RemoteNavigable.h>
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

// The journal finds the box as it drains, so the box is not looked up here.
static void note_paint_facts(DOM::Node const& node, PaintFactsFamily families)
{
    if (!node.has_layout_box())
        return;
    if (auto identity = DOM::NodeIdentity::of(node))
        const_cast<DOM::Document&>(node.document()).invalidation_journal().note_paint_facts(identity, families);
    else
        apply_paint_facts(*node.unsafe_layout_node(), families);
}

void push_form_control_paint_facts(HTML::HTMLInputElement& input)
{
    using enum HTML::HTMLInputElement::TypeAttributeState;
    if (first_is_one_of(input.type_state(), Checkbox, RadioButton))
        note_paint_facts(input, PaintFactsFamily::FormControl);
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
    note_paint_facts(canvas, PaintFactsFamily::Canvas);
}

static Optional<u64> composited_context_id_for_navigable_container(HTML::NavigableContainer const& navigable_container)
{
    auto content_navigable = navigable_container.content_navigable();
    if (!content_navigable || content_navigable->has_been_destroyed())
        return {};
    Optional<Web::CompositorContextId> context_id;
    if (auto const* remote_navigable = as_if<HTML::RemoteNavigable>(*content_navigable)) {
        // The content is composited by the process hosting it.
        context_id = remote_navigable->compositor_context_id();
    } else {
        auto const& local_navigable = as<HTML::LocalNavigable>(*content_navigable);
        if (local_navigable.has_compositor_context()) {
            auto const* hosted_document = navigable_container.content_document_without_origin_check();
            if (!hosted_document || !hosted_document->is_render_blocked())
                context_id = local_navigable.compositor_context().id();
        }
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

static Layout::RustFFI::FfiNaturalSize natural_size_facts(Optional<CSSPixels> width, Optional<CSSPixels> height, Optional<CSSPixelFraction> aspect_ratio)
{
    Layout::RustFFI::FfiNaturalSize natural {};
    natural.width = width;
    natural.height = height;
    if (aspect_ratio.has_value()) {
        natural.has_aspect_ratio = true;
        natural.aspect_ratio_numerator = aspect_ratio->numerator();
        natural.aspect_ratio_denominator = aspect_ratio->denominator();
    }
    return natural;
}

static Layout::RustFFI::FfiImageContent image_content_facts(GC::Ptr<HTML::DecodedImageData> decoded_image_data, Optional<Gfx::DecodedImageFrame>& current_frame_storage)
{
    Layout::RustFFI::FfiImageContent content {};
    if (!decoded_image_data)
        return content;
    if (auto const* svg_image_data = as_if<SVG::SVGDecodedImageData>(*decoded_image_data)) {
        content.kind = Layout::RustFFI::FfiImageContentKind::Vector;
        content.vector_content_identity = svg_image_data->vector_content_identity();
        content.vector_has_active_view_box = svg_image_data->has_active_view_box();
        return content;
    }
    content.kind = Layout::RustFFI::FfiImageContentKind::Raster;
    current_frame_storage = decoded_image_data->current_frame();
    if (current_frame_storage.has_value())
        content.frame = &current_frame_storage.value();
    return content;
}

static Layout::RustFFI::FfiLayerImagePaintFacts layer_image_paint_facts_for(CSS::AbstractImageStyleValue const& image, GC::Ptr<HTML::DecodedImageData> decoded_image_data, Optional<Gfx::DecodedImageFrame>& current_frame_storage)
{
    Layout::RustFFI::FfiLayerImagePaintFacts facts {};
    facts.is_paintable = image.is_paintable(decoded_image_data);
    facts.content = image_content_facts(decoded_image_data, current_frame_storage);
    if (decoded_image_data) {
        auto natural_size = image.natural_size(*decoded_image_data);
        facts.natural = natural_size_facts(natural_size.width, natural_size.height, natural_size.aspect_ratio);
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

static void push_layer_image_paint_facts_onto(Layout::NodeWithStyle const& layout_node)
{
    auto const& background_layers = layout_node.background_layers();
    auto const& mask_layers = layout_node.mask_layers();
    Vector<Layout::RustFFI::FfiLayerImagePaintFactsEntry> entries;
    Vector<Optional<Gfx::DecodedImageFrame>> current_frames;
    current_frames.ensure_capacity(background_layers.size() + mask_layers.size() + 1);
    auto append_entry = [&](Layout::RustFFI::FfiLayerImageList list, size_t computed_index, CSS::AbstractImageStyleValue const* image, Layout::NodeWithStyle::ImageObserver const* observer) {
        if (!image)
            return;
        current_frames.append({});
        entries.append({
            .list = list,
            .computed_index = static_cast<u32>(computed_index),
            .facts = layer_image_paint_facts_for(*image, decoded_image_data_of(observer), current_frames.last()),
        });
    };
    for (size_t layer_index = 0; layer_index < background_layers.size(); ++layer_index)
        append_entry(Layout::RustFFI::FfiLayerImageList::Background, layer_index, background_layers[layer_index].background_image.ptr(), layout_node.background_image_observer(layer_index));
    for (size_t layer_index = 0; layer_index < mask_layers.size(); ++layer_index)
        append_entry(Layout::RustFFI::FfiLayerImageList::Mask, layer_index, mask_layers[layer_index].background_image.ptr(), layout_node.mask_image_observer(layer_index));
    append_entry(Layout::RustFFI::FfiLayerImageList::BorderImageSource, 0, layout_node.border_image().source.ptr(), layout_node.border_image_source_observer());
    Layout::RustFFI::layout_arena_set_layer_image_paint_facts(layout_node.arena_handle(), Layout::Node::slot_id(&layout_node), entries.data(), entries.size());
}

static bool push_replaced_image_paint_facts_onto(Layout::ImageProvider const& image_provider, Layout::Node const& layout_node)
{
    Optional<Gfx::DecodedImageFrame> current_frame;
    Layout::RustFFI::FfiReplacedImagePaintFacts facts {
        .natural = natural_size_facts(image_provider.intrinsic_width(), image_provider.intrinsic_height(), image_provider.intrinsic_aspect_ratio()),
        .content = image_content_facts(image_provider.decoded_image_data(), current_frame),
    };
    return Layout::RustFFI::layout_arena_set_replaced_image_paint_facts(layout_node.arena_handle(), Layout::Node::slot_id(&layout_node), facts);
}

static bool push_video_paint_facts_onto(HTML::HTMLVideoElement const& video_element, Layout::Node const& layout_node)
{
    Layout::RustFFI::FfiVideoPaintFacts facts {};
    switch (video_element.current_representation()) {
    case HTML::HTMLVideoElement::Representation::FirstVideoFrame:
    case HTML::HTMLVideoElement::Representation::VideoFrame: {
        facts.representation = Layout::RustFFI::FfiVideoRepresentation::VideoFrame;
        auto sink_handle = video_element.video_sink_handle();
        if (sink_handle.has_value() && video_element.natural_media_size().has_value()) {
            facts.has_video_frame = true;
            auto src_size = video_element.natural_media_size()->to_type<int>();
            facts.video_src_width = src_size.width();
            facts.video_src_height = src_size.height();
            facts.video_sink_resource_id = video_element.video_sink_resource_id().value().value();
            facts.video_sink_handle = sink_handle->value();
        }
        break;
    }
    case HTML::HTMLVideoElement::Representation::PosterFrame:
        facts.representation = Layout::RustFFI::FfiVideoRepresentation::PosterFrame;
        if (auto const& poster_frame = video_element.poster_frame(); poster_frame.has_value())
            facts.poster_frame = &poster_frame.value();
        break;
    case HTML::HTMLVideoElement::Representation::TransparentBlack:
        facts.representation = Layout::RustFFI::FfiVideoRepresentation::TransparentBlack;
        break;
    }
    return Layout::RustFFI::layout_arena_set_video_paint_facts(layout_node.arena_handle(), Layout::Node::slot_id(&layout_node), facts);
}

static bool paints_replaced_image_from_facts(Layout::Node const& layout_node)
{
    return layout_node.kind() == Layout::RustFFI::NodeKind::ImageBox || layout_node.kind() == Layout::RustFFI::NodeKind::SVGImageBox;
}

static void note_box_paint_facts(Layout::Node const& layout_node, PaintFactsFamily families)
{
    if (auto identity = journal_identity_of(layout_node))
        const_cast<DOM::Document&>(layout_node.document()).invalidation_journal().note_paint_facts(identity, families);
    else
        apply_paint_facts(layout_node, families);
}

void push_layer_image_paint_facts(Layout::NodeWithStyle const& layout_node)
{
    note_box_paint_facts(layout_node, PaintFactsFamily::LayerImages);
}

void push_replaced_image_paint_facts(Layout::Node const& layout_node)
{
    if (paints_replaced_image_from_facts(layout_node))
        note_box_paint_facts(layout_node, PaintFactsFamily::ReplacedImage);
}

void push_video_paint_facts(HTML::HTMLVideoElement const& video_element)
{
    note_paint_facts(video_element, PaintFactsFamily::Video);
}

void push_paint_facts_after_style_attach(Layout::NodeWithStyle& layout_node, StyleHoldsImageValues style_holds_image_values)
{
    auto families = style_holds_image_values == StyleHoldsImageValues::Yes ? PaintFactsFamily::LayerImages : PaintFactsFamily::NoLayerImages;
    if (paints_form_control_from_facts(layout_node))
        families |= PaintFactsFamily::FormControl;
    else if (layout_node.kind() == Layout::RustFFI::NodeKind::CanvasBox)
        families |= PaintFactsFamily::Canvas;
    else if (paints_replaced_image_from_facts(layout_node))
        families |= PaintFactsFamily::ReplacedImage;
    else if (layout_node.kind() == Layout::RustFFI::NodeKind::VideoBox)
        families |= PaintFactsFamily::Video;
    note_box_paint_facts(layout_node, families);
}

void apply_paint_facts(Layout::Node const& layout_node, PaintFactsFamily families)
{
    if (has_flag(families, PaintFactsFamily::NoLayerImages))
        Layout::RustFFI::layout_arena_set_layer_image_paint_facts(layout_node.arena_handle(), Layout::Node::slot_id(&layout_node), nullptr, 0);
    if (auto const* node_with_style = as_if<Layout::NodeWithStyle>(layout_node); node_with_style && has_flag(families, PaintFactsFamily::LayerImages))
        push_layer_image_paint_facts_onto(*node_with_style);
    if (has_flag(families, PaintFactsFamily::FormControl) && paints_form_control_from_facts(layout_node))
        push_form_control_paint_facts_onto(as<HTML::HTMLInputElement>(*layout_node.dom_node()), layout_node);
    if (has_flag(families, PaintFactsFamily::Canvas) && layout_node.kind() == Layout::RustFFI::NodeKind::CanvasBox)
        push_canvas_paint_facts_onto(as<HTML::HTMLCanvasElement>(*layout_node.dom_node()), layout_node);
    if (has_flag(families, PaintFactsFamily::ReplacedImage) && paints_replaced_image_from_facts(layout_node)) {
        auto const& image_provider = layout_node.kind() == Layout::RustFFI::NodeKind::ImageBox
            ? static_cast<Layout::Box const&>(layout_node).image_provider()
            : static_cast<Layout::ImageProvider const&>(as<SVG::SVGImageElement>(*layout_node.dom_node()));
        if (push_replaced_image_paint_facts_onto(image_provider, layout_node))
            apply_repaint_damage(layout_node, InvalidateDisplayList::PaintCommands);
    }
    if (has_flag(families, PaintFactsFamily::Video) && layout_node.kind() == Layout::RustFFI::NodeKind::VideoBox) {
        if (push_video_paint_facts_onto(as<HTML::HTMLVideoElement>(*layout_node.dom_node()), layout_node))
            apply_repaint_damage(layout_node, InvalidateDisplayList::PaintCommands);
    }
}

}
