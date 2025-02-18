/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/Painting/PaintContext.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGDecodedImageData);
GC_DEFINE_ALLOCATOR(SVGDecodedImageData::SVGPageClient);

ErrorOr<GC::Ref<SVGDecodedImageData>> SVGDecodedImageData::create(JS::Realm& realm, GC::Ref<Page> host_page, URL::URL const& url, ByteBuffer data)
{
    auto page_client = SVGPageClient::create(Bindings::main_thread_vm(), host_page);
    auto page = Page::create(Bindings::main_thread_vm(), *page_client);
    page_client->m_svg_page = page.ptr();
    page->set_top_level_traversable(MUST(Web::HTML::TraversableNavigable::create_a_new_top_level_traversable(*page, nullptr, {})));
    GC::Ref<HTML::Navigable> navigable = page->top_level_traversable();
    auto response = Fetch::Infrastructure::Response::create(navigable->vm());
    response->url_list().append(url);
    auto navigation_params = navigable->heap().allocate<HTML::NavigationParams>();
    navigation_params->navigable = navigable;
    navigation_params->response = response;
    navigation_params->origin = URL::Origin {};
    navigation_params->policy_container = navigable->heap().allocate<HTML::PolicyContainer>(navigable->active_document()->realm());
    navigation_params->final_sandboxing_flag_set = HTML::SandboxingFlagSet {};
    navigation_params->opener_policy = HTML::OpenerPolicy {};

    // FIXME: Use Navigable::navigate() instead of manually replacing the navigable's document.
    auto document = MUST(DOM::Document::create_and_initialize(DOM::Document::Type::HTML, "text/html"_string, navigation_params));
    navigable->set_ongoing_navigation({});
    navigable->active_document()->destroy();
    navigable->active_session_history_entry()->document_state()->set_document(document);
    auto& window = as<HTML::Window>(HTML::relevant_global_object(document));
    document->browsing_context()->window_proxy()->set_window(window);

    auto parser = HTML::HTMLParser::create_with_uncertain_encoding(document, data);
    parser->run(document->url());

    // Perform some DOM surgery to make the SVG root element be the first child of the Document.
    // FIXME: This is a huge hack until we figure out how to actually parse separate SVG files.
    auto* svg_root = document->body()->first_child_of_type<SVG::SVGSVGElement>();
    if (!svg_root)
        return Error::from_string_literal("SVGDecodedImageData: Invalid SVG input");

    svg_root->remove();
    document->remove_all_children();

    MUST(document->append_child(*svg_root));

    return realm.create<SVGDecodedImageData>(page, page_client, document, *svg_root);
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

RefPtr<Gfx::Bitmap> SVGDecodedImageData::render(Gfx::IntSize size) const
{
    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, size).release_value_but_fixme_should_propagate_errors();
    VERIFY(m_document->navigable());
    m_document->navigable()->set_viewport_size(size.to_type<CSSPixels>());
    m_document->update_layout();

    auto display_list = m_document->record_display_list({});
    if (!display_list)
        return {};

    auto painting_command_executor_type = m_page_client->display_list_player_type();
    switch (painting_command_executor_type) {
    case DisplayListPlayerType::SkiaGPUIfAvailable:
    case DisplayListPlayerType::SkiaCPU: {
        auto painting_surface = Gfx::PaintingSurface::wrap_bitmap(*bitmap);
        Painting::DisplayListPlayerSkia display_list_player;
        display_list_player.set_surface(painting_surface);
        display_list_player.execute(*display_list);
        break;
    }
    default:
        VERIFY_NOT_REACHED();
    }

    return bitmap;
}

RefPtr<Gfx::ImmutableBitmap> SVGDecodedImageData::bitmap(size_t, Gfx::IntSize size) const
{
    if (size.is_empty())
        return nullptr;

    if (auto it = m_cached_rendered_bitmaps.find(size); it != m_cached_rendered_bitmaps.end())
        return it->value;

    // Prevent the cache from growing too big.
    // FIXME: Evict least used entries.
    if (m_cached_rendered_bitmaps.size() > 10)
        m_cached_rendered_bitmaps.remove(m_cached_rendered_bitmaps.begin());

    auto immutable_bitmap = Gfx::ImmutableBitmap::create(*render(size));
    m_cached_rendered_bitmaps.set(size, immutable_bitmap);
    return immutable_bitmap;
}

Optional<CSSPixels> SVGDecodedImageData::intrinsic_width() const
{
    // https://www.w3.org/TR/SVG2/coords.html#SizingSVGInCSS
    m_document->update_style();
    auto const root_element_style = m_root_element->computed_properties();
    VERIFY(root_element_style);
    auto const& width_value = root_element_style->size_value(CSS::PropertyID::Width);
    if (width_value.is_length() && width_value.length().is_absolute())
        return width_value.length().absolute_length_to_px();
    return {};
}

Optional<CSSPixels> SVGDecodedImageData::intrinsic_height() const
{
    // https://www.w3.org/TR/SVG2/coords.html#SizingSVGInCSS
    m_document->update_style();
    auto const root_element_style = m_root_element->computed_properties();
    VERIFY(root_element_style);
    auto const& height_value = root_element_style->size_value(CSS::PropertyID::Height);
    if (height_value.is_length() && height_value.length().is_absolute())
        return height_value.length().absolute_length_to_px();
    return {};
}

Optional<CSSPixelFraction> SVGDecodedImageData::intrinsic_aspect_ratio() const
{
    // https://www.w3.org/TR/SVG2/coords.html#SizingSVGInCSS
    auto width = intrinsic_width();
    auto height = intrinsic_height();
    if (height.has_value() && *height == 0)
        return {};

    if (width.has_value() && height.has_value())
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

}
