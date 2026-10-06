/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageSetStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLMapElement.h>
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

// The facts read off an element are queued for the box bound to it, which the render state finds as it applies them. A
// node the style mirror has not named, or one of a document with no boxes, has no box.
template<typename Queue>
static void queue_element_paint_facts(DOM::Element const& element, Queue queue)
{
    auto identity = DOM::NodeIdentity::of(element);
    auto* arena = element.document().layout_node_arena_if_created();
    if (identity && arena)
        queue(arena->host(), identity.style_node().value());
}

static void queue_form_control_paint_facts(HTML::HTMLInputElement const& input)
{
    Layout::RustFFI::FfiFormControlPaintFacts facts {
        .enabled = input.enabled(),
        .checked = input.checked(),
        .indeterminate = input.indeterminate(),
        .being_activated = input.is_being_activated(),
    };
    queue_element_paint_facts(input, [&](auto* host, u32 element) {
        Layout::RustFFI::render_state_set_form_control_paint_facts(host, element, facts);
    });
}

void push_form_control_paint_facts(HTML::HTMLInputElement& input)
{
    using enum HTML::HTMLInputElement::TypeAttributeState;
    if (input.has_layout_box() && first_is_one_of(input.type_state(), Checkbox, RadioButton))
        queue_form_control_paint_facts(input);
}

static void queue_canvas_paint_facts(HTML::HTMLCanvasElement const& canvas)
{
    Layout::RustFFI::FfiCanvasPaintFacts facts {};
    if (auto content_size = canvas.canvas_surface_content_size(); content_size.has_value()) {
        facts.has_content = true;
        facts.content_width = content_size->width();
        facts.content_height = content_size->height();
        facts.canvas_id = canvas.canvas_id().value().value();
        facts.content_generation = canvas.content_generation();
    }
    // Where the facts changed, the canvas's paint cache goes with them.
    queue_element_paint_facts(canvas, [&](auto* host, u32 element) {
        Layout::RustFFI::render_state_set_canvas_paint_facts(host, element, facts);
    });
}

void push_canvas_paint_facts(HTML::HTMLCanvasElement const& canvas)
{
    if (canvas.has_layout_box())
        queue_canvas_paint_facts(canvas);
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

void reconcile_navigable_container_paint_facts(Layout::BegunRead const& read, DOM::Document const& document)
{
    for (auto const* navigable_container : HTML::NavigableContainer::all_instances()) {
        if (&navigable_container->document() != &document)
            continue;
        auto const* layout_node = navigable_container->layout_node(read);
        if (!layout_node || !is_navigable_container_viewport_paintable(*layout_node))
            continue;
        Layout::RustFFI::FfiNavigableContainerPaintFacts facts {};
        if (auto context_id = composited_context_id_for_navigable_container(*navigable_container); context_id.has_value()) {
            facts.has_composited_context = true;
            facts.composited_context_id = *context_id;
        }
        // Where the facts changed, the container's paint cache goes with them.
        Layout::RustFFI::render_state_set_navigable_container_paint_facts(layout_node->document_host(), Layout::Node::slot_id(layout_node), facts);
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
    auto background_images = layout_node.background_images();
    auto mask_images = layout_node.mask_images();
    Vector<Layout::RustFFI::FfiLayerImagePaintFactsEntry> entries;
    Vector<Optional<Gfx::DecodedImageFrame>> current_frames;
    current_frames.ensure_capacity(background_images.size() + mask_images.size() + 1);
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
    for (size_t layer_index = 0; layer_index < background_images.size(); ++layer_index)
        append_entry(Layout::RustFFI::FfiLayerImageList::Background, layer_index, background_images[layer_index].ptr(), layout_node.background_image_observer(layer_index));
    for (size_t layer_index = 0; layer_index < mask_images.size(); ++layer_index)
        append_entry(Layout::RustFFI::FfiLayerImageList::Mask, layer_index, mask_images[layer_index].ptr(), layout_node.mask_image_observer(layer_index));
    append_entry(Layout::RustFFI::FfiLayerImageList::BorderImageSource, 0, layout_node.border_image_source(), layout_node.border_image_source_observer());
    Layout::RustFFI::render_state_set_layer_image_paint_facts(layout_node.document_host(), Layout::Node::slot_id(&layout_node), entries.data(), entries.size());
}

// Hands `queue` the facts of the image `image_provider` provides, which live as long as the call.
template<typename Queue>
static void with_replaced_image_paint_facts(Layout::ImageProvider const& image_provider, Queue queue)
{
    Optional<Gfx::DecodedImageFrame> current_frame;
    Layout::RustFFI::FfiReplacedImagePaintFacts facts {
        .natural = natural_size_facts(image_provider.intrinsic_width(), image_provider.intrinsic_height(), image_provider.intrinsic_aspect_ratio()),
        .content = image_content_facts(image_provider.decoded_image_data(), current_frame),
    };
    queue(facts);
}

static void queue_video_paint_facts(HTML::HTMLVideoElement const& video_element)
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
    queue_element_paint_facts(video_element, [&](auto* host, u32 element) {
        Layout::RustFFI::render_state_set_video_paint_facts(host, element, facts);
    });
}

static bool paints_replaced_image_from_facts(Layout::Node const& layout_node)
{
    return layout_node.kind() == Layout::RustFFI::NodeKind::ImageBox || layout_node.kind() == Layout::RustFFI::NodeKind::SVGImageBox;
}

static void queue_box_image_paint_facts(Layout::Node const& layout_node)
{
    Layout::ImageProvider const* image_provider = layout_node.kind() == Layout::RustFFI::NodeKind::ImageBox
        ? static_cast<Layout::Box const&>(layout_node).image_provider_if_any()
        : as_if<SVG::SVGImageElement>(layout_node.dom_node());
    if (!image_provider)
        return;
    with_replaced_image_paint_facts(*image_provider, [&](auto const& facts) {
        Layout::RustFFI::render_state_set_replaced_image_paint_facts(layout_node.document_host(), Layout::Node::slot_id(&layout_node), facts);
    });
}

void push_layer_image_paint_facts(Layout::NodeWithStyle& layout_node)
{
    push_layer_image_paint_facts_onto(layout_node);
    mark_box(layout_node, repaint_marks(InvalidateDisplayList::PaintCommands));
}

void push_replaced_image_paint_facts(Layout::Node& layout_node)
{
    if (!paints_replaced_image_from_facts(layout_node))
        return;
    queue_box_image_paint_facts(layout_node);
    request_document_repaint(layout_node.document(), InvalidateDisplayList::PaintCommands);
}

void push_replaced_image_paint_facts(DOM::Element const& element, Layout::ImageProvider const& image_provider)
{
    if (!element.has_layout_box())
        return;
    with_replaced_image_paint_facts(image_provider, [&](auto const& facts) {
        queue_element_paint_facts(element, [&](auto* host, u32 style_node) {
            Layout::RustFFI::render_state_set_element_image_paint_facts(host, style_node, facts);
        });
    });
    request_document_repaint(element.document(), InvalidateDisplayList::PaintCommands);
}

void push_video_paint_facts(HTML::HTMLVideoElement const& video_element)
{
    if (!video_element.has_layout_box())
        return;
    queue_video_paint_facts(video_element);
    request_document_repaint(video_element.document(), InvalidateDisplayList::PaintCommands);
}

// The `<area>` elements of the image map an image is associated with, in tree order, each named by its style-tree
// identity, because that is what a hit hands back. An area is never rendered, so it has no row of its own to carry its
// shape; the image whose map lists it does.
static void push_image_map_area_facts_onto(GC::Ptr<HTML::HTMLMapElement> map_element, Layout::Node const& layout_node)
{
    // The values AreaShape::from_raw() reads.
    static_assert(to_underlying(HTML::HTMLAreaElement::ShapeState::Circle) == 0);
    static_assert(to_underlying(HTML::HTMLAreaElement::ShapeState::Default) == 1);
    static_assert(to_underlying(HTML::HTMLAreaElement::ShapeState::Polygon) == 2);
    static_assert(to_underlying(HTML::HTMLAreaElement::ShapeState::Rectangle) == 3);

    Vector<Layout::RustFFI::FfiImageMapArea> areas;
    Vector<double> coords;
    if (map_element) {
        // https://html.spec.whatwg.org/multipage/image-maps.html#image-map-processing-model
        // 3. Otherwise, the user agent must collect all the area elements that are descendants of the map. Let areas
        //    be that list.
        map_element->for_each_in_subtree_of_type<HTML::HTMLAreaElement>([&](HTML::HTMLAreaElement& area_element) {
            auto area_coords = area_element.shape_coords();
            areas.append({
                .style_node = DOM::NodeIdentity::of(area_element).style_node().value(),
                .shape = to_underlying(area_element.shape_state()),
                .coords_offset = static_cast<u32>(coords.size()),
                .coords_count = static_cast<u32>(area_coords.size()),
            });
            coords.extend(move(area_coords));
            return TraversalDecision::Continue;
        });
    }
    Layout::RustFFI::render_state_publish_image_map_areas(layout_node.document_host(), Layout::Node::slot_id(&layout_node), areas.data(), areas.size(), coords.data(), coords.size());
}

// Which map an image is associated with is a hash-name reference resolved against the image's root, so any map or area
// of the document can decide any image's areas and there is no smaller funnel than the document. The funnels only mark
// the document, and this runs before the next hit test, after layout, so the association it reads is current and a
// map that gains many areas is walked once. Nearly every page has no image map at all, and never marks it.
void publish_image_map_area_facts_if_needed(Layout::BegunRead const& read, DOM::Document& document)
{
    if (!document.take_image_map_areas_need_publication())
        return;
    document.for_each_shadow_including_descendant([&read](DOM::Node& node) {
        auto* image_element = as_if<HTML::HTMLImageElement>(node);
        if (!image_element)
            return TraversalDecision::Continue;
        // NB: Any box an image has answers for its map, including the one it takes when it renders as its alt text.
        if (auto const* layout_node = image_element->unsafe_layout_node(read))
            push_image_map_area_facts_onto(image_element->associated_map_element(), *layout_node);
        return TraversalDecision::Continue;
    });
}

void push_paint_facts_after_style_attach(Layout::NodeWithStyle& layout_node, StyleHoldsImageValues style_holds_image_values)
{
    // NB: An image with no map has no image map areas to write: a row starts out with no areas, and when an image loses
    //     its map, the document's publication pass clears what its row had.
    if (auto* image_element = as_if<HTML::HTMLImageElement>(layout_node.dom_node())) {
        if (auto map_element = image_element->associated_map_element())
            push_image_map_area_facts_onto(map_element, layout_node);
    }
    if (style_holds_image_values == StyleHoldsImageValues::Yes)
        push_layer_image_paint_facts_onto(layout_node);
    else
        Layout::RustFFI::render_state_set_layer_image_paint_facts(layout_node.document_host(), Layout::Node::slot_id(&layout_node), nullptr, 0);
    // NB: A box a layout built beside the removal of its element has no DOM node left to take facts from. It paints from
    //     its style until the removal takes it away.
    auto* dom_node = layout_node.dom_node();
    switch (layout_node.kind()) {
    case Layout::RustFFI::NodeKind::CheckBox:
    case Layout::RustFFI::NodeKind::RadioButton:
        if (dom_node)
            queue_form_control_paint_facts(as<HTML::HTMLInputElement>(*dom_node));
        break;
    case Layout::RustFFI::NodeKind::CanvasBox:
        if (dom_node)
            queue_canvas_paint_facts(as<HTML::HTMLCanvasElement>(*dom_node));
        break;
    case Layout::RustFFI::NodeKind::ImageBox:
    case Layout::RustFFI::NodeKind::SVGImageBox:
        queue_box_image_paint_facts(layout_node);
        request_document_repaint(layout_node.document(), InvalidateDisplayList::PaintCommands);
        break;
    case Layout::RustFFI::NodeKind::VideoBox:
        if (!dom_node)
            break;
        queue_video_paint_facts(as<HTML::HTMLVideoElement>(*dom_node));
        request_document_repaint(layout_node.document(), InvalidateDisplayList::PaintCommands);
        break;
    default:
        break;
    }
}

}
