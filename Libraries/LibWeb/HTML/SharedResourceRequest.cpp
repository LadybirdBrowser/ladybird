/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <LibGfx/Bitmap.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Statuses.h>
#include <LibWeb/HTML/AnimatedBitmapDecodedImageData.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/SharedResourceRequest.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SharedResourceRequest);

GC::Ref<SharedResourceRequest> SharedResourceRequest::get_or_create(JS::Realm& realm, GC::Ref<Page> page, URL::URL const& url)
{
    auto document = Bindings::principal_host_defined_environment_settings_object(realm).responsible_document();
    VERIFY(document);
    auto& shared_resource_requests = document->shared_resource_requests();
    if (auto it = shared_resource_requests.find(url); it != shared_resource_requests.end())
        return *it->value;
    auto request = realm.create<SharedResourceRequest>(page, url, *document);
    shared_resource_requests.set(url, request);
    return request;
}

SharedResourceRequest::SharedResourceRequest(GC::Ref<Page> page, URL::URL url, GC::Ref<DOM::Document> document)
    : m_page(page)
    , m_url(move(url))
    , m_document(document)
{
}

SharedResourceRequest::~SharedResourceRequest() = default;

void SharedResourceRequest::finalize()
{
    Base::finalize();
    auto& shared_resource_requests = m_document->shared_resource_requests();
    shared_resource_requests.remove(m_url);
}

void SharedResourceRequest::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_fetch_controller);
    visitor.visit(m_document);
    visitor.visit(m_page);
    for (auto& callback : m_callbacks) {
        visitor.visit(callback.on_finish);
        visitor.visit(callback.on_fail);
    }
    visitor.visit(m_image_data);
}

GC::Ptr<DecodedImageData> SharedResourceRequest::image_data() const
{
    return m_image_data;
}

GC::Ptr<Fetch::Infrastructure::FetchController> SharedResourceRequest::fetch_controller()
{
    return m_fetch_controller.ptr();
}

void SharedResourceRequest::set_fetch_controller(GC::Ptr<Fetch::Infrastructure::FetchController> fetch_controller)
{
    m_fetch_controller = move(fetch_controller);
}

void SharedResourceRequest::fetch_resource(JS::Realm& realm, GC::Ref<Fetch::Infrastructure::Request> request)
{
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    fetch_algorithms_input.process_response = [this, &realm, request](GC::Ref<Fetch::Infrastructure::Response> response) {
        // FIXME: If the response is CORS cross-origin, we must use its internal response to query any of its data. See:
        //        https://github.com/whatwg/html/issues/9355
        response = response->unsafe_response();

        auto process_body = GC::create_function(heap(), [this, request, response](ByteBuffer data) {
            auto extracted_mime_type = response->header_list()->extract_mime_type();
            auto mime_type = extracted_mime_type.has_value() ? extracted_mime_type.value().essence().bytes_as_string_view() : StringView {};
            handle_successful_fetch(request->url(), mime_type, move(data));
        });
        auto process_body_error = GC::create_function(heap(), [this](JS::Value) {
            handle_failed_fetch();
        });

        // Check for failed fetch response
        if (!Fetch::Infrastructure::is_ok_status(response->status()) || !response->body()) {
            handle_failed_fetch();
            return;
        }

        response->body()->fully_read(realm, process_body, process_body_error, GC::Ref { realm.global_object() });
    };

    m_state = State::Fetching;

    auto fetch_controller = Fetch::Fetching::fetch(
        realm,
        request,
        Fetch::Infrastructure::FetchAlgorithms::create(realm.vm(), move(fetch_algorithms_input)))
                                .release_value_but_fixme_should_propagate_errors();

    set_fetch_controller(fetch_controller);
}

void SharedResourceRequest::add_callbacks(Function<void()> on_finish, Function<void()> on_fail)
{
    if (m_state == State::Finished) {
        if (on_finish)
            on_finish();
        return;
    }

    if (m_state == State::Failed) {
        if (on_fail)
            on_fail();
        return;
    }

    Callbacks callbacks;
    if (on_finish)
        callbacks.on_finish = GC::create_function(vm().heap(), move(on_finish));
    if (on_fail)
        callbacks.on_fail = GC::create_function(vm().heap(), move(on_fail));

    m_callbacks.append(move(callbacks));
}

void SharedResourceRequest::handle_successful_fetch(URL::URL const& url_string, StringView mime_type, ByteBuffer data)
{
    // AD-HOC: At this point, things gets very ad-hoc.
    // FIXME: Bring this closer to spec.

    bool const is_svg_image = mime_type == "image/svg+xml"sv || url_string.basename().ends_with(".svg"sv);

    if (is_svg_image) {
        auto result = SVG::SVGDecodedImageData::create(m_document->realm(), m_page, url_string, data);
        if (result.is_error()) {
            handle_failed_fetch();
        } else {
            m_image_data = result.release_value();
            handle_successful_resource_load();
        }
        return;
    }

    auto handle_successful_bitmap_decode = [strong_this = GC::Root(*this)](Web::Platform::DecodedImage& result) -> ErrorOr<void> {
        Vector<AnimatedBitmapDecodedImageData::Frame> frames;
        for (auto& frame : result.frames) {
            frames.append(AnimatedBitmapDecodedImageData::Frame {
                .bitmap = Gfx::ImmutableBitmap::create(*frame.bitmap, Gfx::AlphaType::Premultiplied, result.color_space),
                .duration = static_cast<int>(frame.duration),
            });
        }
        strong_this->m_image_data = AnimatedBitmapDecodedImageData::create(strong_this->m_document->realm(), move(frames), result.loop_count, result.is_animated).release_value_but_fixme_should_propagate_errors();
        strong_this->handle_successful_resource_load();
        return {};
    };

    auto handle_failed_decode = [strong_this = GC::Root(*this)](Error&) -> void {
        strong_this->handle_failed_fetch();
    };

    (void)Web::Platform::ImageCodecPlugin::the().decode_image(data.bytes(), move(handle_successful_bitmap_decode), move(handle_failed_decode));
}

void SharedResourceRequest::handle_failed_fetch()
{
    m_state = State::Failed;
    for (auto& callback : m_callbacks) {
        if (callback.on_fail)
            callback.on_fail->function()();
    }
    m_callbacks.clear();
}

void SharedResourceRequest::handle_successful_resource_load()
{
    m_state = State::Finished;
    for (auto& callback : m_callbacks) {
        if (callback.on_finish)
            callback.on_finish->function()();
    }
    m_callbacks.clear();
}

bool SharedResourceRequest::needs_fetching() const
{
    return m_state == State::New;
}

bool SharedResourceRequest::is_fetching() const
{
    return m_state == State::Fetching;
}

}
