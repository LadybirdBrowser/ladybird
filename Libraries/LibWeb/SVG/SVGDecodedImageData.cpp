/*
 * Copyright (c) 2023-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/SharedImage.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWeb/HTML/PaintConfig.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/ExternalContentSource.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>
#include <LibWeb/SVG/SVGSVGElement.h>
#include <LibWeb/XML/XMLDocumentBuilder.h>
#include <LibXML/Parser/Parser.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGDecodedImageData);
GC_DEFINE_ALLOCATOR(SVGDecodedImageData::SVGPageClient);

struct SVGDecodedImageData::CachedOffscreenRender {
    RefPtr<Gfx::DecodedImageFrame> frame;
    RefPtr<Painting::ExternalContentSource> source;
    bool pending { false };
};

ErrorOr<GC::Ref<SVGDecodedImageData>> SVGDecodedImageData::create(JS::Realm& realm, GC::Ref<Page> host_page, URL::URL const& url, ReadonlyBytes encoded_svg)
{
    auto page_client = SVGPageClient::create(Bindings::main_thread_vm(), host_page);
    auto page = Page::create(Bindings::main_thread_vm(), *page_client);
    page->set_is_scripting_enabled(false);
    page_client->m_svg_page = page.ptr();
    page->set_top_level_traversable(Web::HTML::TraversableNavigable::create_a_new_top_level_traversable(*page, nullptr, {}));
    GC::Ref<HTML::Navigable> navigable = page->top_level_traversable();
    auto response = Fetch::Infrastructure::Response::create(navigable->vm());
    response->url_list().append(url);
    auto origin = URL::Origin::create_opaque();
    auto navigation_params = navigable->heap().allocate<HTML::NavigationParams>(OptionalNone {},
        navigable,
        nullptr,
        response,
        nullptr,
        nullptr,
        HTML::OpenerPolicyEnforcementResult { .url = url, .origin = origin, .opener_policy = HTML::OpenerPolicy {} },
        nullptr,
        origin,
        navigable->heap().allocate<HTML::PolicyContainer>(realm.heap()),
        HTML::SandboxingFlagSet {},
        HTML::OpenerPolicy {},
        OptionalNone {},
        HTML::UserNavigationInvolvement::None);

    // FIXME: Use Navigable::navigate() instead of manually replacing the navigable's document.
    auto document = MUST(DOM::Document::create_and_initialize(DOM::Document::Type::XML, "image/svg+xml"_string, navigation_params));
    navigable->set_ongoing_navigation({});
    navigable->active_document()->destroy();
    navigable->set_active_document(document);
    auto& window = as<HTML::Window>(HTML::relevant_global_object(document));
    document->browsing_context()->window_proxy()->set_window(window);

    XML::Parser parser(encoded_svg, { .resolve_named_html_entity = resolve_named_html_entity });
    XMLDocumentBuilder builder { document, XMLScriptingSupport::Disabled };
    auto result = parser.parse_with_listener(builder);
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

SVGDecodedImageData::CachedOffscreenRender const* SVGDecodedImageData::ensure_offscreen_render(Gfx::IntSize size) const
{
    VERIFY(m_document->navigable());

    if (size.is_empty())
        return nullptr;

    if (auto it = m_cached_offscreen_renders.find(size); it != m_cached_offscreen_renders.end())
        return &it->value;

    // Prevent the cache from growing too big.
    // FIXME: Evict least used entries.
    if (m_cached_offscreen_renders.size() > 10)
        m_cached_offscreen_renders.remove(m_cached_offscreen_renders.begin());

    m_document->navigable()->set_viewport_size(size.to_type<CSSPixels>());
    m_document->update_layout(DOM::UpdateLayoutReason::SVGDecodedImageDataRender);

    PaintServer::OffscreenRenderTarget target {
        .kind = PaintServer::OffscreenTargetKind::ContentImage,
        .backend_preference = PaintServer::OffscreenBackendPreference::PreferGPU,
    };
    HTML::PaintConfig paint_config {};

    auto source = Painting::ExternalContentSource::create();
    source->set_pending_content();
    m_cached_offscreen_renders.set(size, CachedOffscreenRender {
                                             .frame = nullptr,
                                             .source = source,
                                             .pending = true,
                                         });
    auto request_id = m_page_client->m_host_page->client().request_offscreen_render(PageClient::OffscreenPaintRequest {
        .document = *m_document,
        .paint_config = paint_config,
        .target = target,
        .readiness = Painting::OffscreenPaintSync::SubmitImmediately,
        .callback = [self = GC::make_root(this), source, size](PageClient::OffscreenRenderResult result) mutable {
            if (!result.content_image.has_value() || result.image_id == 0) {
                source->clear();
                self->m_cached_offscreen_renders.remove(size);
                return;
            }

            auto shared_image = make<Gfx::SharedImage>(Gfx::SharedImage::import_from_payload(result.content_image.release_value()));
            source->set_content_image(result.image_id, shared_image->export_payload());
            auto frame = Gfx::DecodedImageFrame::create(*shared_image->bitmap(), shared_image->color_space());
            self->m_cached_offscreen_renders.set(size, CachedOffscreenRender {
                                                           .frame = frame,
                                                           .source = move(source),
                                                           .pending = false,
                                                       });
            self->m_page_client->m_host_page->client().request_frame();
        },
    });
    if (!request_id.has_value()) {
        source->clear();
        m_cached_offscreen_renders.remove(size);
        return nullptr;
    }

    if (auto it = m_cached_offscreen_renders.find(size); it != m_cached_offscreen_renders.end())
        return &it->value;
    return nullptr;
}

RefPtr<Gfx::DecodedImageFrame> SVGDecodedImageData::frame(size_t, Gfx::IntSize size) const
{
    if (size.is_empty())
        return nullptr;

    auto const* cached_render = ensure_offscreen_render(size);
    if (!cached_render || cached_render->pending)
        return nullptr;
    return cached_render->frame;
}

RefPtr<Painting::ExternalContentSource> SVGDecodedImageData::external_content_source(size_t, Gfx::IntSize size) const
{
    if (size.is_empty())
        return nullptr;

    auto const* cached_render = ensure_offscreen_render(size);
    if (!cached_render)
        return nullptr;
    return cached_render->source;
}

void SVGDecodedImageData::when_frame_available(Gfx::IntSize size, ESCAPING GC::Root<GC::Function<void()>> callback) const
{
    auto const* cached_render = ensure_offscreen_render(size);
    if (!cached_render || !cached_render->source || !cached_render->pending) {
        Core::deferred_invoke([callback = move(callback)]() mutable {
            callback->function()();
        });
        return;
    }
    cached_render->source->when_content_is_finalized(move(callback));
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

Optional<Gfx::IntRect> SVGDecodedImageData::frame_rect(size_t) const
{
    return {};
}

void SVGDecodedImageData::paint(DisplayListRecordingContext& context, size_t, Gfx::IntRect dst_rect, Gfx::IntRect clip_rect, Gfx::ScalingMode scaling_mode) const
{
    auto const* cached_render = ensure_offscreen_render(dst_rect.size());
    if (!cached_render || !cached_render->source)
        return;

    context.display_list_recorder().save();
    context.display_list_recorder().add_clip_rect(clip_rect);
    context.display_list_recorder().draw_external_content(dst_rect, NonnullRefPtr<Painting::ExternalContentSource> { *cached_render->source }, scaling_mode);
    context.display_list_recorder().restore();
}

}
