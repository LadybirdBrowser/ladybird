/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/TypeCasts.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/CORS.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWebCommon/MimeSniff/MimeType.h>

namespace Web::Fetch::Infrastructure {

NonnullRefPtr<Response> Response::create()
{
    return adopt_ref(*new Response(HTTP::HeaderList::create()));
}

Response::Response(NonnullRefPtr<HTTP::HeaderList> header_list)
    : m_header_list(move(header_list))
    , m_monotonic_response_time(MonotonicTime::now())
{
}

// https://fetch.spec.whatwg.org/#ref-for-concept-network-error%E2%91%A3
// A network error is a response whose status is always 0, status message is always
// the empty byte sequence, header list is always empty, and body is always null.

NonnullRefPtr<Response> Response::aborted_network_error()
{
    auto response = network_error("Fetch has been aborted"_string);
    response->set_aborted(true);
    return response;
}

NonnullRefPtr<Response> Response::network_error(String message)
{
    dbgln_if(WEB_FETCH_DEBUG, "Fetch: Creating network error response with message: {}", message);
    auto response = Response::create();
    response->set_status(0);
    response->set_type(Type::Error);
    VERIFY(!response->body().has_value());
    response->m_network_error_message = move(message);
    return response;
}

// https://fetch.spec.whatwg.org/#appropriate-network-error
NonnullRefPtr<Response> Response::appropriate_network_error(FetchParams const& fetch_params)
{
    // 1. Assert: fetchParams is canceled.
    VERIFY(fetch_params.is_canceled());

    // 2. Return an aborted network error if fetchParams is aborted; otherwise return a network error.
    return fetch_params.is_aborted()
        ? aborted_network_error()
        : network_error("Fetch has been terminated"_string);
}

// https://fetch.spec.whatwg.org/#concept-aborted-network-error
bool Response::is_aborted_network_error() const
{
    // A response whose type is "error" and aborted flag is set is known as an aborted network error.
    // NOTE: We have to use the virtual getter here to not bypass filtered responses.
    return type() == Type::Error && aborted();
}

// https://fetch.spec.whatwg.org/#concept-network-error
bool Response::is_network_error() const
{
    // A network error is a response whose type is "error", status is 0, status message is the empty byte sequence,
    // header list is « », body is null, and body info is a new response body info.
    // NOTE: We have to use the virtual getter here to not bypass filtered responses.
    if (type() != Type::Error)
        return false;
    if (status() != 0)
        return false;
    if (!status_message().is_empty())
        return false;
    if (!header_list()->is_empty())
        return false;
    if (body().has_value())
        return false;
    if (body_info() != BodyInfo {})
        return false;
    return true;
}

void Response::release_request_transfer_lease() const
{
    if (m_network_transport.has_value())
        m_network_transport->handle->release_transfer_lease();
}

void Response::resume_body_delivery() const
{
    release_request_transfer_lease();
    if (m_network_transport.has_value())
        m_network_transport->body_delivery_gate->release_hold();
}

void Response::resume_body_delivery_up_to(size_t byte_count) const
{
    if (m_network_transport.has_value())
        m_network_transport->body_delivery_gate->release_hold_up_to(byte_count);
}

Vector<Core::ImmutableBytes> Response::release_body_for_transfer() const
{
    VERIFY(m_network_transport.has_value());
    m_network_transport->body_delivery_gate->release_transport();
    auto prefix = m_network_transport->body_channel->transfer_out();

    // A file-backed body reaches the next process whole, with the bytes this one already received.
    if (m_network_transport->handle->has_file_backed_response_body())
        return {};
    return prefix;
}

// https://fetch.spec.whatwg.org/#concept-response-url
Optional<URL::URL const&> Response::url() const
{
    // A response has an associated URL. It is a pointer to the last URL in response’s URL list and null if response’s URL list is empty.
    // NOTE: We have to use the virtual getter here to not bypass filtered responses.
    if (url_list().is_empty())
        return {};
    return url_list().last();
}

// https://fetch.spec.whatwg.org/#concept-response-location-url
ErrorOr<Optional<URL::URL>> Response::location_url(Optional<StringView> request_fragment) const
{
    // The location URL of a response response, given null or an ASCII string requestFragment, is the value returned by the following steps. They return null, failure, or a URL.

    // 1. If response’s status is not a redirect status, then return null.
    // NOTE: We have to use the virtual getter here to not bypass filtered responses.
    if (!is_redirect_status(status()))
        return Optional<URL::URL> {};

    // 2. Let location be the result of extracting header list values given `Location` and response’s header list.
    auto location_values_or_failure = m_header_list->extract_header_list_values("Location"sv);
    auto const* location_values = location_values_or_failure.get_pointer<Vector<ByteString>>();

    if (!location_values || location_values->size() != 1)
        return OptionalNone {};

    // 3. If location is a header value, then set location to the result of parsing location with response’s URL.
    auto location = DOMURL::parse_from_byte_string(location_values->first(), url());
    if (!location.has_value())
        return Error::from_string_literal("Invalid 'Location' header URL");

    // 4. If location is a URL whose fragment is null, then set location’s fragment to requestFragment.
    if (!location->fragment().has_value())
        location->set_fragment(request_fragment);

    // 5. Return location.
    return location;
}

// https://fetch.spec.whatwg.org/#concept-response-clone
NonnullRefPtr<Response> Response::clone() const
{
    // To clone a response response, run these steps:
    // 1. If response is a filtered response, then return a new identical filtered response whose internal response is a clone of response’s internal response.
    if (is<FilteredResponse>(*this)) {
        auto internal_response = static_cast<FilteredResponse const&>(*this).internal_response()->clone();
        if (is<BasicFilteredResponse>(*this))
            return BasicFilteredResponse::create(internal_response);
        if (is<CORSFilteredResponse>(*this))
            return CORSFilteredResponse::create(internal_response);
        if (is<OpaqueFilteredResponse>(*this))
            return OpaqueFilteredResponse::create(internal_response);
        if (is<OpaqueRedirectFilteredResponse>(*this))
            return OpaqueRedirectFilteredResponse::create(internal_response);
        VERIFY_NOT_REACHED();
    }

    // 2. Let newResponse be a copy of response, except for its body.
    auto new_response = Infrastructure::Response::create();
    new_response->set_type(m_type);
    new_response->set_aborted(m_aborted);
    new_response->set_url_list(m_url_list);
    new_response->set_status(m_status);
    new_response->set_status_message(m_status_message);
    for (auto const& header : *m_header_list)
        new_response->header_list()->append(header);
    new_response->set_cache_state(m_cache_state);
    new_response->set_cors_exposed_header_name_list(m_cors_exposed_header_name_list);
    new_response->set_range_requested(m_range_requested);
    new_response->set_request_includes_credentials(m_request_includes_credentials);
    new_response->set_timing_allow_passed(m_timing_allow_passed);
    new_response->set_navigation_timing_allow_values_list(m_navigation_timing_allow_values_list);
    new_response->set_body_info(m_body_info);
    new_response->set_redirect_taint(m_redirect_taint);
    new_response->set_javascript_bytecode_cache(m_javascript_bytecode_cache);
    new_response->set_javascript_bytecode_cache_vary_key(m_javascript_bytecode_cache_vary_key);
    if (m_network_transport.has_value())
        new_response->set_network_transport(*m_network_transport);
    if (m_javascript_bytecode_cache_memory_cache_request_headers.has_value())
        new_response->set_javascript_bytecode_cache_memory_cache_request_headers(HTTP::HeaderList::create((*m_javascript_bytecode_cache_memory_cache_request_headers)->headers()));
    // FIXME: service worker timing info

    // 3. If response’s body is non-null, then set newResponse’s body to the result of cloning response’s body.
    // NB: ResponseView::clone() tees the body's stream, and a response whose bytes are still in a channel is never
    //     cloned.
    if (m_body.has_value()) {
        VERIFY(!m_body->bytes().has<NonnullRefPtr<Engine::FetchByteChannel>>());
        new_response->set_body(*m_body);
    }

    // 4. Return newResponse.
    return new_response;
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#unsafe-response
NonnullRefPtr<Response const> Response::unsafe_response() const
{
    // A response's unsafe response is its internal response if it has one, and the response itself otherwise.
    if (is<FilteredResponse>(this))
        return static_cast<FilteredResponse const&>(*this).internal_response();

    return *this;
}

NonnullRefPtr<Response> Response::unsafe_response()
{
    if (is<FilteredResponse>(this))
        return static_cast<FilteredResponse&>(*this).internal_response();

    return *this;
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#cors-same-origin
bool Response::is_cors_same_origin() const
{
    // A response whose type is "basic", "cors", or "default" is CORS-same-origin. [FETCH]
    switch (type()) {
    case Type::Basic:
    case Type::CORS:
    case Type::Default:
        return true;
    default:
        return false;
    }
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#cors-cross-origin
bool Response::is_cors_cross_origin() const
{
    // A response whose type is "opaque" or "opaqueredirect" is CORS-cross-origin.
    return type() == Type::Opaque || type() == Type::OpaqueRedirect;
}

FilteredResponse::FilteredResponse(NonnullRefPtr<Response> internal_response, NonnullRefPtr<HTTP::HeaderList> header_list)
    : Response(move(header_list))
    , m_internal_response(move(internal_response))
{
}

FilteredResponse::~FilteredResponse()
{
}

NonnullRefPtr<BasicFilteredResponse> BasicFilteredResponse::create(NonnullRefPtr<Response> internal_response)
{
    // A basic filtered response is a filtered response whose type is "basic" and header list excludes
    // any headers in internal response’s header list whose name is a forbidden response-header name.
    auto header_list = HTTP::HeaderList::create();

    for (auto const& header : *internal_response->header_list()) {
        if (!HTTP::is_forbidden_response_header_name(header.name))
            header_list->append(header);
    }

    return adopt_ref(*new BasicFilteredResponse(move(internal_response), move(header_list)));
}

BasicFilteredResponse::BasicFilteredResponse(NonnullRefPtr<Response> internal_response, NonnullRefPtr<HTTP::HeaderList> header_list)
    : FilteredResponse(move(internal_response), header_list)
    , m_header_list(move(header_list))
{
}

NonnullRefPtr<CORSFilteredResponse> CORSFilteredResponse::create(NonnullRefPtr<Response> internal_response)
{
    // A CORS filtered response is a filtered response whose type is "cors" and header list excludes
    // any headers in internal response’s header list whose name is not a CORS-safelisted response-header
    // name, given internal response’s CORS-exposed header-name list.
    Vector<StringView> cors_exposed_header_name_list;
    cors_exposed_header_name_list.ensure_capacity(internal_response->cors_exposed_header_name_list().size());

    for (auto const& header_name : internal_response->cors_exposed_header_name_list())
        cors_exposed_header_name_list.unchecked_append(header_name);

    auto header_list = HTTP::HeaderList::create();
    for (auto const& header : *internal_response->header_list()) {
        if (is_cors_safelisted_response_header_name(header.name, cors_exposed_header_name_list))
            header_list->append(header);
    }

    return adopt_ref(*new CORSFilteredResponse(move(internal_response), move(header_list)));
}

CORSFilteredResponse::CORSFilteredResponse(NonnullRefPtr<Response> internal_response, NonnullRefPtr<HTTP::HeaderList> header_list)
    : FilteredResponse(move(internal_response), header_list)
    , m_header_list(move(header_list))
{
}

NonnullRefPtr<OpaqueFilteredResponse> OpaqueFilteredResponse::create(NonnullRefPtr<Response> internal_response)
{
    // An opaque filtered response is a filtered response whose type is "opaque", URL list is the empty list,
    // status is 0, status message is the empty byte sequence, header list is empty, and body is null.
    return adopt_ref(*new OpaqueFilteredResponse(move(internal_response), HTTP::HeaderList::create()));
}

OpaqueFilteredResponse::OpaqueFilteredResponse(NonnullRefPtr<Response> internal_response, NonnullRefPtr<HTTP::HeaderList> header_list)
    : FilteredResponse(move(internal_response), header_list)
    , m_header_list(move(header_list))
{
}

NonnullRefPtr<OpaqueRedirectFilteredResponse> OpaqueRedirectFilteredResponse::create(NonnullRefPtr<Response> internal_response)
{
    // An opaque-redirect filtered response is a filtered response whose type is "opaqueredirect",
    // status is 0, status message is the empty byte sequence, header list is empty, and body is null.
    return adopt_ref(*new OpaqueRedirectFilteredResponse(move(internal_response), HTTP::HeaderList::create()));
}

OpaqueRedirectFilteredResponse::OpaqueRedirectFilteredResponse(NonnullRefPtr<Response> internal_response, NonnullRefPtr<HTTP::HeaderList> header_list)
    : FilteredResponse(move(internal_response), header_list)
    , m_header_list(move(header_list))
{
}

}
