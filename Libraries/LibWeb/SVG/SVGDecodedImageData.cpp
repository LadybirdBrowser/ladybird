/*
 * Copyright (c) 2023-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/ScopeGuard.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibGC/Heap.h>
#include <LibGC/WeakHashMap.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/XMLDocument.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/PaintableTypes.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/XML/XMLDocumentBuilder.h>
#include <LibXML/Parser/Parser.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGDecodedImageData);
GC_DEFINE_ALLOCATOR(SVGDecodedImageData::SVGPageClient);

static GC::Ref<SVGDecodedImageData::SVGPageClient> shared_svg_page_client_for_page(GC::Ref<Page> host_page)
{
    static NeverDestroyed<GC::WeakHashMap<Page, SVGDecodedImageData::SVGPageClient>> page_clients;
    if (auto* page_client = page_clients->get(host_page))
        return *page_client;

    auto page_client = SVGDecodedImageData::SVGPageClient::create(*host_page);
    auto page = Page::create(*page_client);
    page->set_is_scripting_enabled(false);
    page_client->m_svg_page = page.ptr();
    page->set_top_level_traversable(HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {}));
    page_clients->set(host_page, page_client);
    return page_client;
}

ScopedSVGImageDocument::ScopedSVGImageDocument(DOM::Document& document, FrameRequests frame_requests)
    : m_page_client(as<SVGDecodedImageData::SVGPageClient>(document.page().client()))
    , m_navigable(m_page_client->page().local_traversable())
    , m_window(m_page_client->window())
    , m_previous_document(m_window->associated_document())
    , m_previous_current_image_data(m_page_client->current_svg_image_data())
    , m_should_unsuppress_frame_requests(frame_requests == FrameRequests::Suppress)
{
    VERIFY(document.is_decoded_svg());

    if (m_should_unsuppress_frame_requests)
        m_page_client->suppress_frame_requests();

    GC::Ptr<SVGDecodedImageData> current_image_data;

    for (auto& image : m_page_client->m_svg_image_data) {
        if (&image.svg_document() == &document) {
            current_image_data = &image;
            break;
        }
    }

    m_page_client->set_current_svg_image_data(current_image_data);

    document.set_browsing_context(m_navigable->active_browsing_context());
    document.set_window(*m_window);
    m_window->set_associated_document(document);
    m_navigable->set_active_document(document);
}

ScopedSVGImageDocument::ScopedSVGImageDocument(ScopedSVGImageDocument&& other)
    : m_page_client(other.m_page_client)
    , m_navigable(other.m_navigable)
    , m_window(other.m_window)
    , m_previous_document(other.m_previous_document)
    , m_previous_current_image_data(move(other.m_previous_current_image_data))
    , m_should_unsuppress_frame_requests(other.m_should_unsuppress_frame_requests)
    , m_is_active(exchange(other.m_is_active, false))
{
}

ScopedSVGImageDocument::~ScopedSVGImageDocument()
{
    if (!m_is_active)
        return;

    m_navigable->set_active_document(m_previous_document);
    m_window->set_associated_document(m_previous_document);

    m_page_client->set_current_svg_image_data(m_previous_current_image_data.ptr());
    if (m_should_unsuppress_frame_requests)
        m_page_client->unsuppress_frame_requests();
}

ErrorOr<GC::Ref<SVGDecodedImageData>> SVGDecodedImageData::create(GC::Ref<Page> host_page, URL::URL const& url, ReadonlyBytes data)
{
    auto page_client = shared_svg_page_client_for_page(host_page);
    auto& page = page_client->page();

    auto document = DOM::XMLDocument::create(page, page_client->window(), url);
    document->set_content_type("image/svg+xml"_utf16_fly_string);
    document->set_origin(URL::Origin::create_opaque());

    ScopedSVGImageDocument scoped_document { *document, ScopedSVGImageDocument::FrameRequests::Suppress };

    auto parse_failed = [&] {
        document->set_suppresses_attribute_style_invalidation(true);
        ScopeGuard restore_attribute_style_invalidation = [&] {
            document->set_suppresses_attribute_style_invalidation(false);
        };

        XML::Parser parser(data, { .resolve_named_html_entity = resolve_named_html_entity });
        XMLDocumentBuilder builder { document, XMLScriptingSupport::Disabled };
        auto result = parser.parse_with_listener(builder);
        if (result.is_error()) {
            dbgln("SVGDecodedImageData: Failed to parse SVG: {}", result.error());
            return true;
        }
        if (builder.has_error()) {
            dbgln("SVGDecodedImageData: Failed to parse SVG: not namespace-well-formed");
            return true;
        }
        return false;
    }();
    // A parse error fails the image outright, as it does in Blink (SVGImage::DataChanged reports kSizeUnavailable once
    // XMLErrors has replaced the document's root) and Gecko (VectorImage::OnSVGDocumentParsed calls OnSVGDocumentError
    // when no SVG root came out of the parse) — whatever the builder had made of the document before the error isn't
    // shown as if it were the image.
    if (parse_failed)
        return Error::from_string_literal("SVGDecodedImageData: Failed to parse SVG");

    auto* svg_root = document->first_child_of_type<SVG::SVGSVGElement>();
    if (!svg_root) {
        dbgln("SVGDecodedImageData: Invalid SVG input (no SVGSVGElement found)");
        return Error::from_string_literal("SVGDecodedImageData: Invalid SVG input");
    }
    auto svg_image_data = GC::Heap::the().allocate<SVGDecodedImageData>(page, page_client, document, *svg_root);
    page_client->register_svg_image_data(svg_image_data);
    return svg_image_data;
}

SVGDecodedImageData::SVGDecodedImageData(GC::Ref<Page> page, GC::Ref<SVGPageClient> page_client, GC::Ref<DOM::Document> document, GC::Ref<SVG::SVGSVGElement> root_element)
    : m_page(page)
    , m_page_client(page_client)
    , m_document(document)
    , m_root_element(root_element)
    , m_vector_content_identity(next_vector_content_identity())
{
}

u64 SVGDecodedImageData::next_vector_content_identity()
{
    static u64 s_next_vector_content_identity = 1;
    return s_next_vector_content_identity++;
}

SVGDecodedImageData::~SVGDecodedImageData() = default;

void SVGDecodedImageData::finalize()
{
    Base::finalize();
    m_document->tear_down_layout_tree_for_svg_image_document({});
}

void SVGDecodedImageData::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
    visitor.visit(m_document);
    visitor.visit(m_page_client);
    visitor.visit(m_root_element);
}

size_t SVGDecodedImageData::external_memory_size() const
{
    size_t size = Base::external_memory_size();
    size = JS::saturating_add_external_memory_size(size, JS::hash_map_external_memory_size(m_cached_rendered_frames));
    for (auto const& cached_frame : m_cached_rendered_frames)
        size = JS::saturating_add_external_memory_size(size, cached_frame.value.bitmap().data_size());

    return size;
}

static void copy_referenced_resources_to(
    Compositing::DisplayListResourceStorage& destination,
    Compositing::DisplayListResourceStorage const& source,
    Compositing::DisplayListResourceSet const& referenced_resources)
{
    Compositing::DisplayListResourceSet empty_resource_set;
    destination.apply_transaction(source.create_transaction(empty_resource_set, referenced_resources));
}

void SVGDecodedImageData::prune_cached_display_list_resources() const
{
    m_page_client->prune_cached_display_list_resources();
}

void SVGDecodedImageData::append_cached_display_list_resources(Compositing::DisplayListResourceSet& retained_resources) const
{
    for (auto const& cached_display_list : m_cached_display_lists)
        retained_resources.include(cached_display_list.value.referenced_resources);
}

void SVGDecodedImageData::append_paint_command_cache_source_resources(Compositing::DisplayListResourceSet& retained_resources) const
{
    if (!m_document->has_committed_viewport_box())
        return;
    m_document->paint_state().append_paint_command_cache_source_resources(retained_resources);
}

Optional<Compositing::DisplayListResource> SVGDecodedImageData::record_display_list(Gfx::IntSize size, CSS::PreferredColorScheme color_scheme, Compositing::DisplayListResourceStorage& destination_resource_storage) const
{
    return record_display_list_at_scale(size.to_type<CSSPixels>(), 1, color_scheme, destination_resource_storage);
}

Optional<Compositing::DisplayListResource> SVGDecodedImageData::record_display_list_at_scale(CSSPixelSize css_size, float raster_scale, CSS::PreferredColorScheme color_scheme, Compositing::DisplayListResourceStorage& destination_resource_storage) const
{
    ScopedSVGImageDocument scoped_document { *m_document, ScopedSVGImageDocument::FrameRequests::RouteToCurrentImage };
    auto& navigable = *m_document->navigable();
    auto& resource_storage = navigable.display_list_resource_storage();

    RenderKey const key { css_size, raster_scale, color_scheme };
    if (auto it = m_cached_display_lists.find(key); it != m_cached_display_lists.end()) {
        copy_referenced_resources_to(destination_resource_storage, resource_storage, it->value.referenced_resources);
        return Compositing::DisplayListResource { *it->value.display_list, it->value.visual_context_tree };
    }

    // FIXME: Evict least used entries.
    if (m_cached_display_lists.size() > 10) {
        m_cached_display_lists.remove(m_cached_display_lists.begin());
        prune_cached_display_list_resources();
    }

    m_page_client->begin_recording_display_list();
    ScopeGuard finish_recording_display_list = [&] {
        m_page_client->end_recording_display_list();
    };

    m_is_recording_display_list = true;
    ScopeGuard clear_recording_flag = [&] {
        m_is_recording_display_list = false;
    };

    auto previous_viewport_size = navigable.viewport_size();
    ScopeGuard restore_viewport_size = [&] {
        navigable.set_viewport_size(previous_viewport_size);
    };

    auto previous_device_pixels_per_css_pixel = m_page_client->device_pixels_per_css_pixel();
    m_page_client->set_device_pixels_per_css_pixel(raster_scale);
    ScopeGuard restore_device_pixels_per_css_pixel = [&] {
        m_page_client->set_device_pixels_per_css_pixel(previous_device_pixels_per_css_pixel);
    };

    // `prefers-color-scheme` inside the image answers with the embedding element's used scheme, so
    // the media query has to be evaluated again whenever that differs from the last recording.
    if (m_color_scheme != color_scheme) {
        m_color_scheme = color_scheme;
        m_cached_rendered_frames.clear();
        m_natural_size.clear();
        m_document->set_svg_image_color_scheme(color_scheme);
        m_document->style_scope().invalidate_style_cache();
        m_document->record_style_environment_change();
    }

    navigable.set_viewport_size(css_size);
    Layout::ForcedReadScope read { *m_document };
    m_document->update_layout(DOM::UpdateLayoutReason::SVGDecodedImageDataRender);
    auto display_list = m_document->record_display_list(read, {}, resource_storage, Painting::PaintCommandCacheMode::ReadWrite);
    if (!display_list)
        return {};

    VERIFY(m_document->has_committed_viewport_box());
    auto& document_paint_state = m_document->paint_state();
    VERIFY(document_paint_state.display_list_used_as_paint_command_cache_source() == display_list.ptr());
    auto referenced_resources = document_paint_state.paint_command_cache_source_referenced_resources();
    auto visual_context_tree = document_paint_state.visual_context_tree(*m_document);
    referenced_resources.include(resource_storage.collect_referenced_resources(visual_context_tree));
    copy_referenced_resources_to(destination_resource_storage, resource_storage, referenced_resources);
    auto display_list_resource = Compositing::DisplayListResource { *display_list, visual_context_tree };
    m_cached_display_lists.set(key, CachedDisplayList { NonnullRefPtr<Compositing::DisplayList> { *display_list }, move(visual_context_tree), move(referenced_resources) });
    prune_cached_display_list_resources();
    return display_list_resource;
}

// An SVG image inside another SVG image has an SVG page as its host. Walk up to the page that shows the outermost one.
Compositor::CompositorHost* SVGDecodedImageData::host_compositor() const
{
    GC::Ref<Page> page = m_page_client->m_host_page;
    while (page->client().is_svg_page_client())
        page = static_cast<SVGPageClient&>(page->client()).m_host_page;
    return page->client().compositor_host();
}

RefPtr<Gfx::Bitmap> SVGDecodedImageData::render_frame(Gfx::IntSize size) const
{
    auto* compositor_host = host_compositor();
    if (!compositor_host)
        return nullptr;

    auto bitmap = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size);
    if (bitmap.is_error())
        return nullptr;

    Compositing::DisplayListResourceStorage resource_storage;
    auto display_list = record_display_list(size, m_color_scheme, resource_storage);
    if (!display_list.has_value())
        return nullptr;

    if (!compositor_host->rasterize_display_list(*display_list, resource_storage, bitmap.value()))
        return nullptr;
    return bitmap.release_value();
}

Optional<Gfx::DecodedImageFrame> SVGDecodedImageData::current_frame(Gfx::IntSize size) const
{
    if (size.is_empty())
        return {};

    if (auto it = m_cached_rendered_frames.find(size); it != m_cached_rendered_frames.end())
        return it->value;

    auto bitmap = render_frame(size);
    if (!bitmap)
        return {};

    // Prevent the cache from growing too big.
    // FIXME: Evict least used entries.
    if (m_cached_rendered_frames.size() > 10)
        m_cached_rendered_frames.remove(m_cached_rendered_frames.begin());

    auto decoded_frame = Gfx::DecodedImageFrame { *bitmap };
    m_cached_rendered_frames.set(size, decoded_frame);
    return decoded_frame;
}

Optional<Gfx::DecodedImageFrame> SVGDecodedImageData::default_frame(Gfx::IntSize size) const
{
    // FIXME: Implement this properly once we support animated SVGs, potentially by creating a temporary internal
    //        document which has animations disabled.
    return current_frame(size);
}

// https://svgwg.org/svg2-draft/coords.html#SizingSVGInCSS
CSS::SizeWithAspectRatio const& SVGDecodedImageData::natural_size() const
{
    if (m_natural_size.has_value())
        return *m_natural_size;

    CSS::SizeWithAspectRatio natural_size;
    {
        ScopedSVGImageDocument scoped_document { *m_document, ScopedSVGImageDocument::FrameRequests::RouteToCurrentImage };
        m_document->update_style();
        auto const* sizing_values = m_root_element->style_group<CSS::ComputedValues::SizingValues>();
        VERIFY(sizing_values);
        auto absolute_length = [](auto const& size_handle) -> Optional<CSSPixels> {
            auto const& size = CSS::Size::view(size_handle);
            if (size.is_length() && size.length().is_absolute())
                return size.length().absolute_length_to_px();
            return {};
        };
        natural_size.width = absolute_length(sizing_values->width);
        natural_size.height = absolute_length(sizing_values->height);
    }

    if (natural_size.width.has_value() && natural_size.height.has_value() && *natural_size.width > 0 && *natural_size.height > 0) {
        natural_size.aspect_ratio = *natural_size.width / *natural_size.height;
    } else if (auto const& viewbox = m_root_element->view_box(); viewbox.has_value()) {
        auto viewbox_width = CSSPixels::nearest_value_for(viewbox->width);
        auto viewbox_height = CSSPixels::nearest_value_for(viewbox->height);
        if (viewbox_width != 0 && viewbox_height != 0)
            natural_size.aspect_ratio = viewbox_width / viewbox_height;
    }

    // The style flush above may clear this cache through a frame request, so the result is stored only after it.
    m_natural_size = natural_size;
    return *m_natural_size;
}

Optional<CSSPixels> SVGDecodedImageData::intrinsic_width() const
{
    return natural_size().width;
}

Optional<CSSPixels> SVGDecodedImageData::intrinsic_height() const
{
    return natural_size().height;
}

Optional<CSSPixelFraction> SVGDecodedImageData::intrinsic_aspect_ratio() const
{
    return natural_size().aspect_ratio;
}

void SVGDecodedImageData::SVGPageClient::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_host_page);
    visitor.visit(m_svg_page);
}

void SVGDecodedImageData::SVGPageClient::register_svg_image_data(SVGDecodedImageData& svg_image_data)
{
    m_svg_image_data.set(svg_image_data);
}

void SVGDecodedImageData::SVGPageClient::prune_cached_display_list_resources() const
{
    if (m_display_list_recording_count > 0) {
        m_has_pending_display_list_resource_prune = true;
        return;
    }

    prune_cached_display_list_resources_now();
}

void SVGDecodedImageData::SVGPageClient::prune_cached_display_list_resources_now() const
{
    Compositing::DisplayListResourceSet retained_resources;
    for (auto& svg_image_data : m_svg_image_data) {
        svg_image_data.append_cached_display_list_resources(retained_resources);
        svg_image_data.append_paint_command_cache_source_resources(retained_resources);
    }

    m_svg_page->local_traversable()->display_list_resource_storage().retain_only(retained_resources);
}

void SVGDecodedImageData::SVGPageClient::end_recording_display_list()
{
    VERIFY(m_display_list_recording_count > 0);
    --m_display_list_recording_count;

    if (m_display_list_recording_count > 0 || !m_has_pending_display_list_resource_prune)
        return;

    m_has_pending_display_list_resource_prune = false;
    prune_cached_display_list_resources_now();
}

HTML::Window& SVGDecodedImageData::SVGPageClient::window() const
{
    auto window = m_svg_page->local_traversable()->active_window();
    VERIFY(window);
    return *window;
}

void SVGDecodedImageData::SVGPageClient::set_current_svg_image_data(GC::Ptr<SVGDecodedImageData> svg_image_data)
{
    m_current_svg_image_data = svg_image_data;
}

void SVGDecodedImageData::SVGPageClient::request_frame()
{
    if (m_frame_request_suppression_count > 0)
        return;

    if (auto svg_image_data = m_current_svg_image_data.ptr()) {
        svg_image_data->did_request_frame();
        return;
    }

    for (auto& svg_image_data : m_svg_image_data)
        svg_image_data.did_request_frame();
}

void SVGDecodedImageData::did_request_frame()
{
    // Recording the SVG image can itself schedule a frame request through the
    // inner document. Ignore those requests so we do not invalidate caches while
    // populating them.
    if (m_is_recording_display_list)
        return;

    invalidate_cached_rendering();

    // NB: Frame requests arrive synchronously from the SVG document, possibly while a client document is in
    //     the middle of layout or paint; for example, a natural size query during intrinsic size measurement
    //     flushes style in the SVG document, and applying the resulting invalidation requests a frame.
    //     Clients respond to the notification by invalidating their own style and layout, which must never
    //     happen during their layout, so defer (and coalesce) notifications until the event loop spins again.
    if (m_has_pending_client_notification)
        return;
    m_has_pending_client_notification = true;
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(heap(), [self = GC::Ref { *this }] {
        self->m_has_pending_client_notification = false;
        self->notify_clients_did_update();
    }));
}

void SVGDecodedImageData::invalidate_cached_rendering()
{
    m_vector_content_identity = next_vector_content_identity();
    m_natural_size.clear();
    m_cached_rendered_frames.clear();
    m_cached_display_lists.clear();
    prune_cached_display_list_resources();
}

bool SVGDecodedImageData::has_active_view_box() const
{
    return m_root_element->active_view_box().has_value();
}

Optional<Painting::ImagePaint> SVGDecodedImageData::image_paint(Painting::ImagePaintRequest const& request) const
{
    // The destination rect is in the recording's local units, which the visual context chain may
    // magnify arbitrarily (an SVG viewport's user units, a CSS transform); the raster resolution
    // follows the accumulated on-screen scale so the replayed content stays sharp.
    auto dst_rect = request.dest_rect;
    auto accumulated_scale = request.accumulated_scale;
    auto color_scheme = request.color_scheme;
    constexpr int maximum_raster_dimension = 16384;
    auto raster_dimension = [&](float local_size, float scale) {
        if (!(scale > 0))
            scale = 1;
        return clamp(static_cast<int>(lroundf(local_size * scale)), 1, maximum_raster_dimension);
    };
    Gfx::IntSize raster_size {
        raster_dimension(dst_rect.width(), accumulated_scale.width()),
        raster_dimension(dst_rect.height(), accumulated_scale.height()),
    };

    // A document with an active viewBox renders the same proportions at any viewport, so it lays
    // out directly at the raster size and its recorded coordinates land on the raster's pixel
    // grid exactly. Without one, the layout viewport determines the content's proportions, so
    // layout happens at the local size and only the raster resolution follows the scale.
    Optional<Compositing::DisplayListResource> display_list;
    Gfx::IntSize list_size;
    if (m_root_element->active_view_box().has_value()) {
        display_list = record_display_list(raster_size, color_scheme, request.resource_storage);
        list_size = raster_size;
    } else {
        auto css_size = CSSPixelSize {
            clamp(CSSPixels::nearest_value_for(dst_rect.width()), CSSPixels::smallest_positive_value(), CSSPixels(maximum_raster_dimension)),
            clamp(CSSPixels::nearest_value_for(dst_rect.height()), CSSPixels::smallest_positive_value(), CSSPixels(maximum_raster_dimension)),
        };
        auto raster_scale = max(accumulated_scale.width(), accumulated_scale.height());
        if (!(raster_scale > 0))
            raster_scale = 1;
        auto maximum_raster_scale = static_cast<float>(maximum_raster_dimension) / max(css_size.width(), css_size.height()).to_float();
        raster_scale = min(raster_scale, maximum_raster_scale);
        display_list = record_display_list_at_scale(css_size, raster_scale, color_scheme, request.resource_storage);
        list_size = {
            max(1, static_cast<int>(lroundf(css_size.width().to_float() * raster_scale))),
            max(1, static_cast<int>(lroundf(css_size.height().to_float() * raster_scale))),
        };
    }
    if (!display_list.has_value())
        return {};

    return Painting::ImagePaint { Painting::ImagePaint::NestedDisplayList { .resource = *display_list, .list_size = list_size } };
}

}
