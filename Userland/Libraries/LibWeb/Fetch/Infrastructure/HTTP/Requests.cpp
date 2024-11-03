/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Fetching/PendingResponse.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::Fetch::Infrastructure {

JS_DEFINE_ALLOCATOR(Request);

Request::Request(JS::NonnullGCPtr<HeaderList> header_list)
    : m_header_list(header_list)
{
}

void Request::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_header_list);
    visitor.visit(m_client);
    m_body.visit(
        [&](JS::NonnullGCPtr<Body>& body) { visitor.visit(body); },
        [](auto&) {});
    visitor.visit(m_reserved_client);
    m_window.visit(
        [&](JS::GCPtr<HTML::EnvironmentSettingsObject> const& value) { visitor.visit(value); },
        [](auto const&) {});
    visitor.visit(m_pending_responses);
}

JS::NonnullGCPtr<Request> Request::create(JS::VM& vm)
{
    return vm.heap().allocate_without_realm<Request>(HeaderList::create(vm));
}

// https://fetch.spec.whatwg.org/#concept-request-url
URL::URL& Request::url()
{
    // A request has an associated URL (a URL).
    // NOTE: Implementations are encouraged to make this a pointer to the first URL in request’s URL list. It is provided as a distinct field solely for the convenience of other standards hooking into Fetch.
    VERIFY(!m_url_list.is_empty());
    return m_url_list.first();
}

// https://fetch.spec.whatwg.org/#concept-request-url
URL::URL const& Request::url() const
{
    return const_cast<Request&>(*this).url();
}

// https://fetch.spec.whatwg.org/#concept-request-current-url
URL::URL& Request::current_url()
{
    // A request has an associated current URL. It is a pointer to the last URL in request’s URL list.
    VERIFY(!m_url_list.is_empty());
    return m_url_list.last();
}

// https://fetch.spec.whatwg.org/#concept-request-current-url
URL::URL const& Request::current_url() const
{
    return const_cast<Request&>(*this).current_url();
}

void Request::set_url(URL::URL url)
{
    // Sometimes setting the URL and URL list are done as two distinct steps in the spec,
    // but since we know the URL is always the URL list's first item and doesn't change later
    // on, we can combine them.
    if (!m_url_list.is_empty())
        m_url_list.clear();
    m_url_list.append(move(url));
}

// https://fetch.spec.whatwg.org/#request-destination-script-like
bool Request::destination_is_script_like() const
{
    // A request’s destination is script-like if it is "audioworklet", "paintworklet", "script", "serviceworker", "sharedworker", or "worker".
    static constexpr Array script_like_destinations = {
        Destination::AudioWorklet,
        Destination::PaintWorklet,
        Destination::Script,
        Destination::ServiceWorker,
        Destination::SharedWorker,
        Destination::Worker,
    };
    return any_of(script_like_destinations, [this](auto destination) {
        return m_destination == destination;
    });
}

// https://fetch.spec.whatwg.org/#subresource-request
bool Request::is_subresource_request() const
{
    // A subresource request is a request whose destination is "audio", "audioworklet", "font", "image", "json", "manifest", "paintworklet", "script", "style", "track", "video", "xslt", or the empty string.
    static constexpr Array subresource_request_destinations = {
        Destination::Audio,
        Destination::AudioWorklet,
        Destination::Font,
        Destination::Image,
        Destination::JSON,
        Destination::Manifest,
        Destination::PaintWorklet,
        Destination::Script,
        Destination::Style,
        Destination::Track,
        Destination::Video,
        Destination::XSLT,
    };
    return any_of(subresource_request_destinations, [this](auto destination) {
        return m_destination == destination;
    }) || !m_destination.has_value();
}

// https://fetch.spec.whatwg.org/#non-subresource-request
bool Request::is_non_subresource_request() const
{
    // A non-subresource request is a request whose destination is "document", "embed", "frame", "iframe", "object", "report", "serviceworker", "sharedworker", or "worker".
    static constexpr Array non_subresource_request_destinations = {
        Destination::Document,
        Destination::Embed,
        Destination::Frame,
        Destination::IFrame,
        Destination::Object,
        Destination::Report,
        Destination::ServiceWorker,
        Destination::SharedWorker,
        Destination::Worker,
    };
    return any_of(non_subresource_request_destinations, [this](auto destination) {
        return m_destination == destination;
    });
}

// https://fetch.spec.whatwg.org/#navigation-request
bool Request::is_navigation_request() const
{
    // A navigation request is a request whose destination is "document", "embed", "frame", "iframe", or "object".
    static constexpr Array navigation_request_destinations = {
        Destination::Document,
        Destination::Embed,
        Destination::Frame,
        Destination::IFrame,
        Destination::Object,
    };
    return any_of(navigation_request_destinations, [this](auto destination) {
        return m_destination == destination;
    });
}

// https://fetch.spec.whatwg.org/#concept-request-tainted-origin
bool Request::has_redirect_tainted_origin() const
{
    // A request request has a redirect-tainted origin if these steps return true:

    // 1. Let lastURL be null.
    Optional<URL::URL const&> last_url;

    // 2. For each url of request’s URL list:
    for (auto const& url : m_url_list) {
        // 1. If lastURL is null, then set lastURL to url and continue.
        if (!last_url.has_value()) {
            last_url = url;
            continue;
        }

        // 2. If url’s origin is not same origin with lastURL’s origin and request’s origin is not same origin with lastURL’s origin, then return true.
        auto const* request_origin = m_origin.get_pointer<URL::Origin>();
        if (!url.origin().is_same_origin(last_url->origin())
            && (request_origin == nullptr || !request_origin->is_same_origin(last_url->origin()))) {
            return true;
        }

        // 3. Set lastURL to url.
        last_url = url;
    }

    // 3. Return false.
    return false;
}

// https://fetch.spec.whatwg.org/#serializing-a-request-origin
String Request::serialize_origin() const
{
    // 1. If request has a redirect-tainted origin, then return "null".
    if (has_redirect_tainted_origin())
        return "null"_string;

    // 2. Return request’s origin, serialized.
    return MUST(String::from_byte_string(m_origin.get<URL::Origin>().serialize()));
}

// https://fetch.spec.whatwg.org/#byte-serializing-a-request-origin
ByteBuffer Request::byte_serialize_origin() const
{
    // Byte-serializing a request origin, given a request request, is to return the result of serializing a request origin with request, isomorphic encoded.
    return MUST(ByteBuffer::copy(serialize_origin().bytes()));
}

// https://fetch.spec.whatwg.org/#concept-request-clone
JS::NonnullGCPtr<Request> Request::clone(JS::Realm& realm) const
{
    // To clone a request request, run these steps:
    auto& vm = realm.vm();

    // 1. Let newRequest be a copy of request, except for its body.
    auto new_request = Infrastructure::Request::create(vm);
    new_request->set_method(m_method);
    new_request->set_local_urls_only(m_local_urls_only);
    for (auto const& header : *m_header_list)
        new_request->header_list()->append(header);
    new_request->set_unsafe_request(m_unsafe_request);
    new_request->set_client(m_client);
    new_request->set_reserved_client(m_reserved_client);
    new_request->set_replaces_client_id(m_replaces_client_id);
    new_request->set_window(m_window);
    new_request->set_keepalive(m_keepalive);
    new_request->set_initiator_type(m_initiator_type);
    new_request->set_service_workers_mode(m_service_workers_mode);
    new_request->set_initiator(m_initiator);
    new_request->set_destination(m_destination);
    new_request->set_priority(m_priority);
    new_request->set_origin(m_origin);
    new_request->set_policy_container(m_policy_container);
    new_request->set_referrer(m_referrer);
    new_request->set_referrer_policy(m_referrer_policy);
    new_request->set_mode(m_mode);
    new_request->set_use_cors_preflight(m_use_cors_preflight);
    new_request->set_credentials_mode(m_credentials_mode);
    new_request->set_use_url_credentials(m_use_url_credentials);
    new_request->set_cache_mode(m_cache_mode);
    new_request->set_redirect_mode(m_redirect_mode);
    new_request->set_integrity_metadata(m_integrity_metadata);
    new_request->set_cryptographic_nonce_metadata(m_cryptographic_nonce_metadata);
    new_request->set_parser_metadata(m_parser_metadata);
    new_request->set_reload_navigation(m_reload_navigation);
    new_request->set_history_navigation(m_history_navigation);
    new_request->set_user_activation(m_user_activation);
    new_request->set_render_blocking(m_render_blocking);
    new_request->set_url_list(m_url_list);
    new_request->set_redirect_count(m_redirect_count);
    new_request->set_response_tainting(m_response_tainting);
    new_request->set_prevent_no_cache_cache_control_header_modification(m_prevent_no_cache_cache_control_header_modification);
    new_request->set_done(m_done);
    new_request->set_timing_allow_failed(m_timing_allow_failed);
    new_request->set_buffer_policy(m_buffer_policy);

    // 2. If request’s body is non-null, set newRequest’s body to the result of cloning request’s body.
    if (auto const* body = m_body.get_pointer<JS::NonnullGCPtr<Body>>())
        new_request->set_body((*body)->clone(realm));

    // 3. Return newRequest.
    return new_request;
}

// https://fetch.spec.whatwg.org/#concept-request-add-range-header
void Request::add_range_header(u64 first, Optional<u64> const& last)
{
    // To add a range header to a request request, with an integer first, and an optional integer last, run these steps:

    // 1. Assert: last is not given, or first is less than or equal to last.
    VERIFY(!last.has_value() || first <= last.value());

    // 2. Let rangeValue be `bytes=`.
    auto range_value = MUST(ByteBuffer::copy("bytes"sv.bytes()));

    // 3. Serialize and isomorphic encode first, and append the result to rangeValue.
    range_value.append(String::number(first).bytes());

    // 4. Append 0x2D (-) to rangeValue.
    range_value.append('-');

    // 5. If last is given, then serialize and isomorphic encode it, and append the result to rangeValue.
    if (last.has_value())
        range_value.append(String::number(*last).bytes());

    // 6. Append (`Range`, rangeValue) to request’s header list.
    auto header = Header {
        .name = MUST(ByteBuffer::copy("Range"sv.bytes())),
        .value = move(range_value),
    };
    m_header_list->append(move(header));
}

// https://fetch.spec.whatwg.org/#append-a-request-origin-header
void Request::add_origin_header()
{
    // 1. Let serializedOrigin be the result of byte-serializing a request origin with request.
    auto serialized_origin = byte_serialize_origin();

    // 2. If request’s response tainting is "cors" or request’s mode is "websocket", then append (`Origin`, serializedOrigin) to request’s header list.
    if (m_response_tainting == ResponseTainting::CORS || m_mode == Mode::WebSocket) {
        auto header = Header {
            .name = MUST(ByteBuffer::copy("Origin"sv.bytes())),
            .value = move(serialized_origin),
        };
        m_header_list->append(move(header));
    }
    // 3. Otherwise, if request’s method is neither `GET` nor `HEAD`, then:
    else if (!StringView { m_method }.is_one_of("GET"sv, "HEAD"sv)) {
        // 1. If request’s mode is not "cors", then switch on request’s referrer policy:
        if (m_mode != Mode::CORS) {
            switch (m_referrer_policy) {
            // -> "no-referrer"
            case ReferrerPolicy::ReferrerPolicy::NoReferrer:
                // Set serializedOrigin to `null`.
                serialized_origin = MUST(ByteBuffer::copy("null"sv.bytes()));
                break;
            // -> "no-referrer-when-downgrade"
            // -> "strict-origin"
            // -> "strict-origin-when-cross-origin"
            case ReferrerPolicy::ReferrerPolicy::NoReferrerWhenDowngrade:
            case ReferrerPolicy::ReferrerPolicy::StrictOrigin:
            case ReferrerPolicy::ReferrerPolicy::StrictOriginWhenCrossOrigin:
                // If request’s origin is a tuple origin, its scheme is "https", and request’s current URL’s scheme is
                // not "https", then set serializedOrigin to `null`.
                if (m_origin.has<URL::Origin>() && m_origin.get<URL::Origin>().scheme() == "https"sv && current_url().scheme() != "https"sv)
                    serialized_origin = MUST(ByteBuffer::copy("null"sv.bytes()));
                break;
            // -> "same-origin"
            case ReferrerPolicy::ReferrerPolicy::SameOrigin:
                // If request’s origin is not same origin with request’s current URL’s origin, then set serializedOrigin
                // to `null`.
                if (m_origin.has<URL::Origin>() && !m_origin.get<URL::Origin>().is_same_origin(current_url().origin()))
                    serialized_origin = MUST(ByteBuffer::copy("null"sv.bytes()));
                break;
            // -> Otherwise
            default:
                // Do nothing.
                break;
            }
        }

        // 2. Append (`Origin`, serializedOrigin) to request’s header list.
        auto header = Header {
            .name = MUST(ByteBuffer::copy("Origin"sv.bytes())),
            .value = move(serialized_origin),
        };
        m_header_list->append(move(header));
    }
}

// https://fetch.spec.whatwg.org/#cross-origin-embedder-policy-allows-credentials
bool Request::cross_origin_embedder_policy_allows_credentials() const
{
    // 1. If request’s mode is not "no-cors", then return true.
    if (m_mode != Mode::NoCORS)
        return true;

    // 2. If request’s client is null, then return true.
    if (m_client == nullptr)
        return true;

    // 3. If request’s client’s policy container’s embedder policy’s value is not "credentialless", then return true.
    if (m_policy_container.has<HTML::PolicyContainer>() && m_policy_container.get<HTML::PolicyContainer>().embedder_policy.value != HTML::EmbedderPolicyValue::Credentialless)
        return true;

    // 4. If request’s origin is same origin with request’s current URL’s origin and request does not have a redirect-tainted origin, then return true.
    // 5. Return false.
    auto const* request_origin = m_origin.get_pointer<URL::Origin>();
    if (request_origin == nullptr)
        return false;

    return request_origin->is_same_origin(current_url().origin()) && !has_redirect_tainted_origin();
}

StringView request_destination_to_string(Request::Destination destination)
{
    switch (destination) {
    case Request::Destination::Audio:
        return "audio"sv;
    case Request::Destination::AudioWorklet:
        return "audioworklet"sv;
    case Request::Destination::Document:
        return "document"sv;
    case Request::Destination::Embed:
        return "embed"sv;
    case Request::Destination::Font:
        return "font"sv;
    case Request::Destination::Frame:
        return "frame"sv;
    case Request::Destination::IFrame:
        return "iframe"sv;
    case Request::Destination::Image:
        return "image"sv;
    case Request::Destination::JSON:
        return "json"sv;
    case Request::Destination::Manifest:
        return "manifest"sv;
    case Request::Destination::Object:
        return "object"sv;
    case Request::Destination::PaintWorklet:
        return "paintworklet"sv;
    case Request::Destination::Report:
        return "report"sv;
    case Request::Destination::Script:
        return "script"sv;
    case Request::Destination::ServiceWorker:
        return "serviceworker"sv;
    case Request::Destination::SharedWorker:
        return "sharedworker"sv;
    case Request::Destination::Style:
        return "style"sv;
    case Request::Destination::Track:
        return "track"sv;
    case Request::Destination::Video:
        return "video"sv;
    case Request::Destination::WebIdentity:
        return "webidentity"sv;
    case Request::Destination::Worker:
        return "worker"sv;
    case Request::Destination::XSLT:
        return "xslt"sv;
    }
    VERIFY_NOT_REACHED();
}

StringView request_mode_to_string(Request::Mode mode)
{
    switch (mode) {
    case Request::Mode::SameOrigin:
        return "same-origin"sv;
    case Request::Mode::CORS:
        return "cors"sv;
    case Request::Mode::NoCORS:
        return "no-cors"sv;
    case Request::Mode::Navigate:
        return "navigate"sv;
    case Request::Mode::WebSocket:
        return "websocket"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<Request::Priority> request_priority_from_string(StringView string)
{
    if (string.equals_ignoring_ascii_case("high"sv))
        return Request::Priority::High;
    if (string.equals_ignoring_ascii_case("low"sv))
        return Request::Priority::Low;
    if (string.equals_ignoring_ascii_case("auto"sv))
        return Request::Priority::Auto;
    return {};
}

}
