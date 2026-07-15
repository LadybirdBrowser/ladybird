/*
 * Copyright (c) 2023-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/NeverDestroyed.h>
#include <AK/NumericLimits.h>
#include <AK/ScopeGuard.h>
#include <LibGC/WeakHashMap.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/PaintingSurface.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/XMLDocument.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/XML/XMLDocumentBuilder.h>
#include <LibXML/Parser/Parser.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGDecodedImageData);
GC_DEFINE_ALLOCATOR(SVGDecodedImageData::SVGPageClient);

static GC::WeakHashMap<Page, SVGDecodedImageData::SVGPageClient>& shared_svg_page_clients()
{
    static NeverDestroyed<GC::WeakHashMap<Page, SVGDecodedImageData::SVGPageClient>> page_clients;
    return *page_clients;
}

static GC::Ref<SVGDecodedImageData::SVGPageClient> shared_svg_page_client_for_page(GC::Ref<Page> host_page)
{
    if (auto* page_client = shared_svg_page_clients().get(host_page))
        return *page_client;

    auto page_client = SVGDecodedImageData::SVGPageClient::create(Bindings::main_thread_vm(), host_page);
    auto page = Page::create(Bindings::main_thread_vm(), page_client);
    page->set_is_scripting_enabled(false);
    page_client->m_svg_page = page.ptr();
    page->set_top_level_traversable(HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {}));
    shared_svg_page_clients().set(host_page, page_client);
    return page_client;
}

class ScopedSVGImageDocument {
public:
    enum class FrameRequests {
        Suppress,
        RouteToCurrentImage,
    };

    ScopedSVGImageDocument(SVGDecodedImageData::SVGPageClient& page_client, DOM::Document& document, FrameRequests frame_requests, GC::Ptr<SVGDecodedImageData> current_image_data = nullptr)
        : m_page_client(page_client)
        , m_navigable(page_client.page().top_level_traversable())
        , m_window(page_client.window())
        , m_previous_document(*m_navigable->active_document())
        , m_previous_current_image_data(page_client.current_svg_image_data())
        , m_should_unsuppress_frame_requests(frame_requests == FrameRequests::Suppress)
    {
        if (m_should_unsuppress_frame_requests)
            m_page_client->suppress_frame_requests();
        m_page_client->set_current_svg_image_data(current_image_data);

        document.set_browsing_context(m_navigable->active_browsing_context());
        document.set_window(*m_window);
        m_window->set_associated_document(document);
        m_navigable->set_active_document(document);
    }

    ~ScopedSVGImageDocument()
    {
        m_navigable->set_active_document(m_previous_document);
        m_window->set_associated_document(m_previous_document);

        m_page_client->set_current_svg_image_data(m_previous_current_image_data.ptr());
        if (m_should_unsuppress_frame_requests)
            m_page_client->unsuppress_frame_requests();
    }

private:
    GC::Ref<SVGDecodedImageData::SVGPageClient> m_page_client;
    GC::Ref<HTML::LocalTraversableNavigable> m_navigable;
    GC::Ref<HTML::Window> m_window;
    GC::Ref<DOM::Document> m_previous_document;
    GC::Weak<SVGDecodedImageData> m_previous_current_image_data;
    bool m_should_unsuppress_frame_requests { false };
};

ErrorOr<GC::Ref<SVGDecodedImageData>> SVGDecodedImageData::create(JS::Realm& realm, GC::Ref<Page> host_page, URL::URL const& url, ReadonlyBytes data)
{
    auto page_client = shared_svg_page_client_for_page(host_page);
    auto& page = page_client->page();

    auto& svg_realm = page.top_level_traversable()->active_document()->realm();
    auto document = DOM::XMLDocument::create(svg_realm);
    document->set_content_type("image/svg+xml"_utf16_fly_string);
    document->set_origin(URL::Origin::create_opaque());
    document->set_url(url);

    ScopedSVGImageDocument scoped_document { page_client, document, ScopedSVGImageDocument::FrameRequests::Suppress };

    auto result = [&] {
        document->set_suppresses_attribute_style_invalidation(true);
        ScopeGuard restore_attribute_style_invalidation = [&] {
            document->set_suppresses_attribute_style_invalidation(false);
        };

        XML::Parser parser(data, { .resolve_named_html_entity = resolve_named_html_entity });
        XMLDocumentBuilder builder { document, XMLScriptingSupport::Disabled };
        return parser.parse_with_listener(builder);
    }();
    if (result.is_error())
        dbgln("SVGDecodedImageData: Failed to parse SVG: {}", result.error());

    // Mark the document as completely loaded so that <use> elements
    // (which defer cloning until the document is complete) resolve
    // forward references to elements parsed after them.
    document->completely_finish_loading();

    auto* svg_root = document->first_child_of_type<SVG::SVGSVGElement>();
    if (!svg_root) {
        dbgln("SVGDecodedImageData: Invalid SVG input (no SVGSVGElement found)");
        return Error::from_string_literal("SVGDecodedImageData: Invalid SVG input");
    }
    auto svg_image_data = realm.create<SVGDecodedImageData>(page, page_client, document, *svg_root);
    page_client->register_svg_image_data(svg_image_data);
    return svg_image_data;
}

SVGDecodedImageData::SVGDecodedImageData(GC::Ref<Page> page, GC::Ref<SVGPageClient> page_client, GC::Ref<DOM::Document> document, GC::Ref<SVG::SVGSVGElement> root_element)
    : m_page(page)
    , m_page_client(page_client)
    , m_document(document)
    , m_root_element(root_element)
{
}

SVGDecodedImageData::~SVGDecodedImageData() = default;

void SVGDecodedImageData::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
    visitor.visit(m_document);
    visitor.visit(m_page_client);
    visitor.visit(m_root_element);
}

static size_t surface_external_memory_size(Gfx::PaintingSurface const& surface)
{
    auto surface_size = surface.size();
    if (surface_size.is_empty())
        return 0;

    Checked<size_t> pixel_size = static_cast<size_t>(surface_size.width());
    pixel_size *= static_cast<size_t>(surface_size.height());
    pixel_size *= sizeof(u32);
    if (pixel_size.has_overflow())
        return NumericLimits<size_t>::max();
    return pixel_size.value();
}

size_t SVGDecodedImageData::external_memory_size() const
{
    size_t size = Base::external_memory_size();
    size = JS::saturating_add_external_memory_size(size, JS::hash_map_external_memory_size(m_cached_rendered_frames));
    for (auto const& cached_frame : m_cached_rendered_frames)
        size = JS::saturating_add_external_memory_size(size, cached_frame.value.bitmap().data_size());

    size = JS::saturating_add_external_memory_size(size, JS::hash_map_external_memory_size(m_cached_rendered_surfaces));
    for (auto const& cached_surface : m_cached_rendered_surfaces)
        size = JS::saturating_add_external_memory_size(size, surface_external_memory_size(*cached_surface.value));

    return size;
}

static void copy_referenced_resources_to(
    Painting::DisplayListResourceStorage& destination,
    Painting::DisplayListResourceStorage const& source,
    Painting::DisplayListResourceSet const& referenced_resources)
{
    Painting::DisplayListResourceSet empty_resource_set;
    destination.apply_transaction(source.create_transaction(empty_resource_set, referenced_resources));
}

void SVGDecodedImageData::prune_cached_display_list_resources() const
{
    m_page_client->prune_cached_display_list_resources();
}

void SVGDecodedImageData::append_cached_display_list_resources(Painting::DisplayListResourceSet& retained_resources) const
{
    for (auto const& cached_display_list : m_cached_display_lists)
        retained_resources.include(cached_display_list.value.referenced_resources);
}

Optional<Painting::DisplayListResource> SVGDecodedImageData::record_display_list(Gfx::IntSize size, Painting::DisplayListResourceStorage& destination_resource_storage) const
{
    ScopedSVGImageDocument scoped_document { *m_page_client, *m_document, ScopedSVGImageDocument::FrameRequests::RouteToCurrentImage, const_cast<SVGDecodedImageData&>(*this) };
    auto& navigable = *m_document->navigable();
    auto& resource_storage = navigable.display_list_resource_storage();

    if (auto it = m_cached_display_lists.find(size); it != m_cached_display_lists.end()) {
        copy_referenced_resources_to(destination_resource_storage, resource_storage, it->value.referenced_resources);
        return Painting::DisplayListResource { *it->value.display_list, it->value.visual_context_tree };
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

    navigable.set_viewport_size(size.to_type<CSSPixels>());
    m_document->update_layout(DOM::UpdateLayoutReason::SVGDecodedImageDataRender);
    auto display_list = m_document->record_display_list({}, resource_storage);
    if (!display_list)
        return {};

    auto referenced_resources = resource_storage.collect_referenced_resources(*display_list);
    copy_referenced_resources_to(destination_resource_storage, resource_storage, referenced_resources);
    auto document_paintable = m_document->paintable();
    VERIFY(document_paintable);
    auto visual_context_tree = document_paintable->visual_context_tree();
    auto display_list_resource = Painting::DisplayListResource { *display_list, visual_context_tree };
    m_cached_display_lists.set(size, CachedDisplayList { NonnullRefPtr<Painting::DisplayList> { *display_list }, move(visual_context_tree), move(referenced_resources) });
    prune_cached_display_list_resources();
    return display_list_resource;
}

RefPtr<Gfx::PaintingSurface> SVGDecodedImageData::render_to_surface(Gfx::IntSize size) const
{
    if (size.is_empty())
        return {};

    if (auto it = m_cached_rendered_surfaces.find(size); it != m_cached_rendered_surfaces.end())
        return it->value;

    // Prevent the cache from growing too big.
    // FIXME: Evict least used entries.
    if (m_cached_rendered_surfaces.size() > 10)
        m_cached_rendered_surfaces.remove(m_cached_rendered_surfaces.begin());

    auto surface = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied);
    Painting::DisplayListResourceStorage resource_storage;
    auto display_list = record_display_list(size, resource_storage);
    if (!display_list.has_value())
        return nullptr;

    Painting::DisplayListPlayerSkia display_list_player;
    display_list_player.execute(*display_list->display_list, display_list->visual_context_tree, resource_storage, {}, surface);
    display_list_player.flush(*surface);

    m_cached_rendered_surfaces.set(size, *surface);
    return surface;
}

Optional<Gfx::DecodedImageFrame> SVGDecodedImageData::current_frame(Gfx::IntSize size) const
{
    if (size.is_empty())
        return {};

    if (auto it = m_cached_rendered_frames.find(size); it != m_cached_rendered_frames.end())
        return it->value;

    // Prevent the cache from growing too big.
    // FIXME: Evict least used entries.
    if (m_cached_rendered_frames.size() > 10)
        m_cached_rendered_frames.remove(m_cached_rendered_frames.begin());

    auto decoded_frame = Gfx::DecodedImageFrame { *render_to_surface(size)->snapshot_bitmap() };
    m_cached_rendered_frames.set(size, decoded_frame);
    return decoded_frame;
}

Optional<Gfx::DecodedImageFrame> SVGDecodedImageData::default_frame(Gfx::IntSize size) const
{
    // FIXME: Implement this properly once we support animated SVGs, potentially by creating a temporary internal
    //        document which has animations disabled.
    return current_frame(size);
}

Optional<CSSPixels> SVGDecodedImageData::intrinsic_width() const
{
    // https://www.w3.org/TR/SVG2/coords.html#SizingSVGInCSS
    ScopedSVGImageDocument scoped_document { *m_page_client, *m_document, ScopedSVGImageDocument::FrameRequests::RouteToCurrentImage, const_cast<SVGDecodedImageData&>(*this) };
    m_document->update_style();
    auto const root_element_style = m_root_element->computed_values();
    VERIFY(root_element_style);
    auto const& width_value = root_element_style->width();
    if (width_value.is_length() && width_value.length().is_absolute())
        return width_value.length().absolute_length_to_px();
    return {};
}

Optional<CSSPixels> SVGDecodedImageData::intrinsic_height() const
{
    // https://www.w3.org/TR/SVG2/coords.html#SizingSVGInCSS
    ScopedSVGImageDocument scoped_document { *m_page_client, *m_document, ScopedSVGImageDocument::FrameRequests::RouteToCurrentImage, const_cast<SVGDecodedImageData&>(*this) };
    m_document->update_style();
    auto const root_element_style = m_root_element->computed_values();
    VERIFY(root_element_style);
    auto const& height_value = root_element_style->height();
    if (height_value.is_length() && height_value.length().is_absolute())
        return height_value.length().absolute_length_to_px();
    return {};
}

Optional<CSSPixelFraction> SVGDecodedImageData::intrinsic_aspect_ratio() const
{
    // https://www.w3.org/TR/SVG2/coords.html#SizingSVGInCSS
    auto width = intrinsic_width();
    auto height = intrinsic_height();
    if (width.has_value() && height.has_value() && *width > 0 && *height > 0)
        return *width / *height;

    if (auto const& viewbox = m_root_element->view_box(); viewbox.has_value()) {
        auto viewbox_width = CSSPixels::nearest_value_for(viewbox->width);

        if (viewbox_width == 0)
            return {};

        auto viewbox_height = CSSPixels::nearest_value_for(viewbox->height);
        if (viewbox_height == 0)
            return {};

        return viewbox_width / viewbox_height;
    }
    return {};
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
    Painting::DisplayListResourceSet retained_resources;
    for (auto& svg_image_data : m_svg_image_data)
        svg_image_data.append_cached_display_list_resources(retained_resources);

    m_svg_page->top_level_traversable()->display_list_resource_storage().retain_only(retained_resources);
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
    auto window = m_svg_page->top_level_traversable()->active_window();
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
    notify_clients_did_update();
}

void SVGDecodedImageData::invalidate_cached_rendering()
{
    m_cached_rendered_frames.clear();
    m_cached_rendered_surfaces.clear();
    m_cached_display_lists.clear();
    prune_cached_display_list_resources();
}

void SVGDecodedImageData::paint(DisplayListRecordingContext& context, Gfx::IntRect dst_rect, CSS::ImageRendering) const
{
    auto display_list = record_display_list(dst_rect.size(), context.display_list_recorder().resource_storage());
    if (!display_list.has_value())
        return;

    context.display_list_recorder().paint_nested_display_list(*display_list, dst_rect);
}

}
