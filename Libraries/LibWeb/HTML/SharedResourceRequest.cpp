/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/Weak.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Fetching/Fetching.h>
#include <LibWeb/Fetch/Infrastructure/FetchAlgorithms.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/HTML/AnimatedBitmapDecodedImageData.h>
#include <LibWeb/HTML/BitmapDecodedImageData.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/SharedResourceRequest.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>
#include <LibWebCommon/Fetch/Infrastructure/HTTP/Statuses.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SharedResourceRequest);

static u64 s_next_memory_cache_touch_serial;

GC::Ref<SharedResourceRequest> SharedResourceRequest::get_or_create(DOM::Document& document, URL::URL const& url)
{
    auto& shared_resource_requests = document.shared_resource_requests();
    if (auto it = shared_resource_requests.find(url); it != shared_resource_requests.end()) {
        it->value->touch_memory_cache_entry();
        return *it->value;
    }
    auto request = GC::Heap::the().allocate<SharedResourceRequest>(document.page(), url, document);
    shared_resource_requests.set(url, request);
    return request;
}

SharedResourceRequest::SharedResourceRequest(GC::Ref<Page> page, URL::URL url, GC::Ref<DOM::Document> document)
    : m_page(page)
    , m_url(move(url))
    , m_document(document)
{
    touch_memory_cache_entry();
}

SharedResourceRequest::~SharedResourceRequest() = default;

void SharedResourceRequest::finalize()
{
    Base::finalize();

    m_callbacks.clear();
    m_load_event_delayer.clear();
    m_image_data = nullptr;
    m_fetch_controller = nullptr;

    if (m_document) {
        remove_from_document();
        m_document = nullptr;
    }
}

void SharedResourceRequest::remove_from_document()
{
    auto& shared_resource_requests = m_document->shared_resource_requests();
    if (auto it = shared_resource_requests.find(m_url); it != shared_resource_requests.end() && it->value.ptr() == this)
        shared_resource_requests.remove(it);
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
        visitor.visit(callback.on_stop);
    }
    visitor.visit(m_image_data);
}

GC::Ptr<DecodedImageData> SharedResourceRequest::image_data() const
{
    return m_image_data;
}

bool SharedResourceRequest::can_be_pruned_from_memory_cache() const
{
    // NB: Pruning an image that still has clients (e.g. a live element displaying it) frees no memory, since the
    //     clients keep the decoded data alive, but it forces a refetch if script recreates an element with the
    //     same URL, e.g. when a framework re-renders the page.
    return m_image_data && !m_image_data->has_clients();
}

void SharedResourceRequest::touch_memory_cache_entry()
{
    m_cache_touch_serial = ++s_next_memory_cache_touch_serial;
}

GC::Ptr<Fetch::Infrastructure::FetchController> SharedResourceRequest::fetch_controller()
{
    return m_fetch_controller.ptr();
}

void SharedResourceRequest::set_fetch_controller(GC::Ptr<Fetch::Infrastructure::FetchController> fetch_controller)
{
    m_fetch_controller = move(fetch_controller);
}

void SharedResourceRequest::fetch_resource(GC::Ref<Fetch::Infrastructure::Request> request)
{
    auto& realm = HTML::relevant_realm(*m_document);
    VERIFY(needs_fetching());

    if (!ResourceLoader::is_initialized()) {
        handle_failed_fetch();
        return;
    }

    GC::Weak weak_this { *this };
    Fetch::Infrastructure::FetchAlgorithms::Input fetch_algorithms_input {};
    fetch_algorithms_input.process_response = [weak_this, &realm, request](GC::Ref<Fetch::Infrastructure::Response> response) {
        auto self = weak_this.ptr();
        if (!self)
            return;

        auto image_data_is_cors_cross_origin = response->is_cors_cross_origin();

        // FIXME: If the response is CORS cross-origin, we must use its internal response to query any of its data. See:
        //        https://github.com/whatwg/html/issues/9355
        response = response->unsafe_response();

        auto process_body = GC::create_function(GC::Heap::the(), [weak_this, request, response, image_data_is_cors_cross_origin](ByteBuffer data) {
            auto self = weak_this.ptr();
            if (!self)
                return;

            auto extracted_mime_type = Fetch::Infrastructure::extract_mime_type(response->header_list());
            auto const is_svg_image = extracted_mime_type.has_value()
                ? extracted_mime_type.value().essence() == "image/svg+xml"sv
                : request->url().basename().ends_with(".svg"sv);
            self->handle_successful_fetch(request->url(), is_svg_image ? IsSVGImage::Yes : IsSVGImage::No, move(data), image_data_is_cors_cross_origin);
        });
        auto process_body_error = GC::create_function(GC::Heap::the(), [weak_this](JS::Value) {
            auto self = weak_this.ptr();
            if (!self)
                return;

            self->handle_failed_fetch();
        });

        // Check for failed fetch response
        if (!Fetch::Infrastructure::is_ok_status(response->status()) || !response->body()) {
            self->handle_failed_fetch();
            return;
        }

        response->body()->fully_read(realm, process_body, process_body_error, GC::Ref { realm.global_object() });
    };

    m_state = State::Fetching;
    m_load_event_delayer.emplace(*m_document);

    auto fetch_controller = Fetch::Fetching::fetch(
        realm,
        request,
        Fetch::Infrastructure::FetchAlgorithms::create(move(fetch_algorithms_input)));

    fetch_controller->set_stop_steps(GC::create_function(GC::Heap::the(), [weak_this] {
        if (auto self = weak_this.ptr())
            self->handle_stopped_fetch();
    }));
    set_fetch_controller(fetch_controller);
}

void SharedResourceRequest::add_callbacks(Function<void()> on_finish, Function<void()> on_fail, Function<void()> on_stop)
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

    if (m_state == State::Stopped) {
        if (on_stop)
            on_stop();
        return;
    }

    Callbacks callbacks;
    if (on_finish)
        callbacks.on_finish = GC::create_function(GC::Heap::the(), move(on_finish));
    if (on_fail)
        callbacks.on_fail = GC::create_function(GC::Heap::the(), move(on_fail));
    if (on_stop)
        callbacks.on_stop = GC::create_function(GC::Heap::the(), move(on_stop));

    m_callbacks.append(move(callbacks));
}

void SharedResourceRequest::handle_successful_fetch(URL::URL const& url_string, IsSVGImage is_svg_image, ByteBuffer data, bool image_data_is_cors_cross_origin)
{
    // AD-HOC: At this point, things gets very ad-hoc.
    // FIXME: Bring this closer to spec.

    if (is_svg_image == IsSVGImage::Yes) {
        SVG::SVGDecodedImageData::decode(m_page, url_string, data)
            ->when_resolved([self = GC::Root { *this }, image_data_is_cors_cross_origin](auto& image_data) {
                self->m_image_data = image_data.ptr();
                self->m_image_data->set_is_cors_cross_origin(image_data_is_cors_cross_origin);
                self->handle_successful_resource_load();
            })
            .when_rejected([self = GC::Root { *this }](Error&) {
                self->handle_failed_fetch();
            });
        return;
    }

    auto handle_successful_bitmap_decode = [strong_this = GC::Root(*this), image_data_is_cors_cross_origin](Web::Platform::DecodedImage& result) -> ErrorOr<void> {
        if (result.session_id != 0) {
            // Streaming animated decode: create AnimatedBitmapDecodedImageData.
            Vector<NonnullRefPtr<Gfx::Bitmap>> initial_bitmaps;
            initial_bitmaps.ensure_capacity(result.frames.size());
            for (auto& frame : result.frames)
                initial_bitmaps.unchecked_append(*frame.bitmap);

            auto first_bitmap = result.frames.first().bitmap;
            auto size = first_bitmap->size();

            strong_this->m_image_data = AnimatedBitmapDecodedImageData::create(
                *strong_this->m_document,
                result.session_id,
                result.frame_count,
                result.loop_count,
                size,
                move(result.color_space),
                move(result.all_durations),
                move(initial_bitmaps));
        } else {
            // Single-shot decode: create BitmapDecodedImageData as before.
            Vector<BitmapDecodedImageData::Frame> frames;
            for (auto& frame : result.frames) {
                frames.append(BitmapDecodedImageData::Frame {
                    .frame = Gfx::DecodedImageFrame { *frame.bitmap, result.color_space },
                    .duration = static_cast<int>(frame.duration),
                });
            }
            strong_this->m_image_data = BitmapDecodedImageData::create(move(frames), result.loop_count, result.is_animated).release_value_but_fixme_should_propagate_errors();
        }
        strong_this->m_image_data->set_is_cors_cross_origin(image_data_is_cors_cross_origin);
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
    m_load_event_delayer.clear();
    m_fetch_controller = nullptr;
    for (auto& callback : m_callbacks) {
        if (callback.on_fail)
            callback.on_fail->function()();
    }
    m_callbacks.clear();
}

// NB: A stopped fetch never responds, so nothing waits on it anymore, and a later request for the URL
//     fetches it again.
void SharedResourceRequest::handle_stopped_fetch()
{
    if (m_state != State::Fetching)
        return;

    m_state = State::Stopped;
    remove_from_document();
    m_load_event_delayer.clear();
    m_fetch_controller = nullptr;
    for (auto& callback : m_callbacks) {
        if (callback.on_stop)
            callback.on_stop->function()();
    }
    m_callbacks.clear();
}

void SharedResourceRequest::handle_successful_resource_load()
{
    m_state = State::Finished;
    m_load_event_delayer.clear();
    m_fetch_controller = nullptr;
    for (auto& callback : m_callbacks) {
        if (callback.on_finish)
            callback.on_finish->function()();
    }
    m_callbacks.clear();
    m_document->prune_image_resource_caches();
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
