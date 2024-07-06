/*
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <LibGfx/Bitmap.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Statuses.h>
#include <LibWeb/HTML/AnimatedBitmapDecodedImageData.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/SharedImageRequest.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(SharedImageRequest);

JS::NonnullGCPtr<SharedImageRequest> SharedImageRequest::get_or_create(JS::Realm& realm, JS::NonnullGCPtr<Page> page, URL::URL const& url)
{
    auto document = Bindings::host_defined_environment_settings_object(realm).responsible_document();
    VERIFY(document);
    auto& shared_image_requests = document->shared_image_requests();
    if (auto it = shared_image_requests.find(url); it != shared_image_requests.end())
        return *it->value;
    auto request = realm.heap().allocate<SharedImageRequest>(realm, page, url, *document);
    shared_image_requests.set(url, request);
    return request;
}

SharedImageRequest::SharedImageRequest(JS::NonnullGCPtr<Page> page, URL::URL url, JS::NonnullGCPtr<DOM::Document> document)
    : m_page(page)
    , m_url(move(url))
    , m_document(document)
{
}

SharedImageRequest::~SharedImageRequest() = default;

void SharedImageRequest::finalize()
{
    Base::finalize();
    auto& shared_image_requests = m_document->shared_image_requests();
    shared_image_requests.remove(m_url);
}

void SharedImageRequest::visit_edges(JS::Cell::Visitor& visitor)
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

JS::GCPtr<DecodedImageData> SharedImageRequest::image_data() const
{
    return m_image_data;
}

JS::GCPtr<Fetch::Infrastructure::FetchController> SharedImageRequest::fetch_controller()
{
    return m_fetch_controller.ptr();
}

void SharedImageRequest::set_fetch_controller(JS::GCPtr<Fetch::Infrastructure::FetchController> fetch_controller)
{
    m_fetch_controller = move(fetch_controller);
}

void SharedImageRequest::fetch_image(JS::Realm& realm, JS::NonnullGCPtr<Fetch::Infrastructure::Request> request)
{
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    fetch_algorithms_input.process_response = [this, &realm, request](JS::NonnullGCPtr<Fetch::Infrastructure::Response> response) {
        // FIXME: If the response is CORS cross-origin, we must use its internal response to query any of its data. See:
        //        https://github.com/whatwg/html/issues/9355
        response = response->unsafe_response();

        auto process_body = JS::create_heap_function(heap(), [this, request, response](ByteBuffer data) {
            auto extracted_mime_type = response->header_list()->extract_mime_type();
            auto mime_type = extracted_mime_type.has_value() ? extracted_mime_type.value().essence().bytes_as_string_view() : StringView {};
            handle_successful_fetch(request->url(), mime_type, move(data));
        });
        auto process_body_error = JS::create_heap_function(heap(), [this](JS::Value) {
            handle_failed_fetch();
        });

        // Check for failed fetch response
        if (!Fetch::Infrastructure::is_ok_status(response->status()) || !response->body()) {
            handle_failed_fetch();
            return;
        }

        response->body()->fully_read(realm, process_body, process_body_error, JS::NonnullGCPtr { realm.global_object() });
    };

    m_state = State::Fetching;

    auto fetch_controller = Fetch::Fetching::fetch(
        realm,
        request,
        Fetch::Infrastructure::FetchAlgorithms::create(realm.vm(), move(fetch_algorithms_input)))
                                .release_value_but_fixme_should_propagate_errors();

    set_fetch_controller(fetch_controller);
}

void SharedImageRequest::add_callbacks(Function<void()> on_finish, Function<void()> on_fail)
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
        callbacks.on_finish = JS::create_heap_function(vm().heap(), move(on_finish));
    if (on_fail)
        callbacks.on_fail = JS::create_heap_function(vm().heap(), move(on_fail));

    m_callbacks.append(move(callbacks));
}

void SharedImageRequest::handle_successful_fetch(URL::URL const& url_string, StringView mime_type, ByteBuffer data)
{
    // AD-HOC: At this point, things gets very ad-hoc.
    // FIXME: Bring this closer to spec.

    bool const is_svg_image = mime_type == "image/svg+xml"sv || url_string.basename().ends_with(".svg"sv);

    auto handle_failed_decode = [strong_this = JS::Handle(*this)](Error&) -> void {
        strong_this->m_state = State::Failed;
        for (auto& callback : strong_this->m_callbacks) {
            if (callback.on_fail)
                callback.on_fail->function()();
        }
    };

    auto handle_successful_decode = [](SharedImageRequest& self) {
        self.m_state = State::Finished;
        for (auto& callback : self.m_callbacks) {
            if (callback.on_finish)
                callback.on_finish->function()();
        }
        self.m_callbacks.clear();
    };

    if (is_svg_image) {
        auto result = SVG::SVGDecodedImageData::create(m_document->realm(), m_page, url_string, data);
        if (result.is_error()) {
            handle_failed_decode(result.error());
        } else {
            m_image_data = result.release_value();
            handle_successful_decode(*this);
        }
        return;
    }

    auto handle_successful_bitmap_decode = [strong_this = JS::Handle(*this), handle_successful_decode = move(handle_successful_decode)](Web::Platform::DecodedImage& result) -> ErrorOr<void> {
        Vector<AnimatedBitmapDecodedImageData::Frame> frames;
        for (auto& frame : result.frames) {
            frames.append(AnimatedBitmapDecodedImageData::Frame {
                .bitmap = Gfx::ImmutableBitmap::create(*frame.bitmap),
                .duration = static_cast<int>(frame.duration),
            });
        }
        strong_this->m_image_data = AnimatedBitmapDecodedImageData::create(strong_this->m_document->realm(), move(frames), result.loop_count, result.is_animated).release_value_but_fixme_should_propagate_errors();
        handle_successful_decode(*strong_this);
        return {};
    };

    (void)Web::Platform::ImageCodecPlugin::the().decode_image(data.bytes(), move(handle_successful_bitmap_decode), move(handle_failed_decode));
}

void SharedImageRequest::handle_failed_fetch()
{
    m_state = State::Failed;
    for (auto& callback : m_callbacks) {
        if (callback.on_fail)
            callback.on_fail->function()();
    }
    m_callbacks.clear();
}

bool SharedImageRequest::needs_fetching() const
{
    return m_state == State::New;
}

bool SharedImageRequest::is_fetching() const
{
    return m_state == State::Fetching;
}

}
