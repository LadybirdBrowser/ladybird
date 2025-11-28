/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/TypeCasts.h>
#include <LibGC/Heap.h>
#include <LibHTTP/Cache/Utilities.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOMURL/DOMURL.h>
#include <LibWeb/Fetch/Infrastructure/FetchParams.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/CORS.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::Fetch::Infrastructure {

GC_DEFINE_ALLOCATOR(Response);
GC_DEFINE_ALLOCATOR(BasicFilteredResponse);
GC_DEFINE_ALLOCATOR(CORSFilteredResponse);
GC_DEFINE_ALLOCATOR(OpaqueFilteredResponse);
GC_DEFINE_ALLOCATOR(OpaqueRedirectFilteredResponse);

GC::Ref<Response> Response::create(JS::VM& vm)
{
    return vm.heap().allocate<Response>(HTTP::HeaderList::create());
}

Response::Response(NonnullRefPtr<HTTP::HeaderList> header_list)
    : m_header_list(move(header_list))
    , m_response_time(UnixDateTime::now())
    , m_monotonic_response_time(MonotonicTime::now())
{
}

void Response::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_body);
}

// https://fetch.spec.whatwg.org/#ref-for-concept-network-error%E2%91%A3
// A network error is a response whose status is always 0, status message is always
// the empty byte sequence, header list is always empty, and body is always null.

GC::Ref<Response> Response::aborted_network_error(JS::VM& vm)
{
    auto response = network_error(vm, "Fetch has been aborted"_string);
    response->set_aborted(true);
    return response;
}

GC::Ref<Response> Response::network_error(JS::VM& vm, String message)
{
    dbgln_if(WEB_FETCH_DEBUG, "Fetch: Creating network error response with message: {}", message);
    auto response = Response::create(vm);
    response->set_status(0);
    response->set_type(Type::Error);
    VERIFY(!response->body());
    response->m_network_error_message = move(message);
    return response;
}

// https://fetch.spec.whatwg.org/#appropriate-network-error
GC::Ref<Response> Response::appropriate_network_error(JS::VM& vm, FetchParams const& fetch_params)
{
    // 1. Assert: fetchParams is canceled.
    VERIFY(fetch_params.is_canceled());

    // 2. Return an aborted network error if fetchParams is aborted; otherwise return a network error.
    return fetch_params.is_aborted()
        ? aborted_network_error(vm)
        : network_error(vm, "Fetch has been terminated"_string);
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
    if (body())
        return false;
    if (body_info() != BodyInfo {})
        return false;
    return true;
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
ErrorOr<Optional<URL::URL>> Response::location_url(Optional<String> const& request_fragment) const
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
    auto location = DOMURL::parse(location_values->first(), url());
    if (!location.has_value())
        return Error::from_string_literal("Invalid 'Location' header URL");

    // 4. If location is a URL whose fragment is null, then set location’s fragment to requestFragment.
    if (!location->fragment().has_value())
        location->set_fragment(request_fragment);

    // 5. Return location.
    return location;
}

// https://fetch.spec.whatwg.org/#concept-response-clone
GC::Ref<Response> Response::clone(JS::Realm& realm) const
{
    // To clone a response response, run these steps:
    auto& vm = realm.vm();

    // 1. If response is a filtered response, then return a new identical filtered response whose internal response is a clone of response’s internal response.
    if (is<FilteredResponse>(*this)) {
        auto internal_response = static_cast<FilteredResponse const&>(*this).internal_response()->clone(realm);
        if (is<BasicFilteredResponse>(*this))
            return BasicFilteredResponse::create(vm, internal_response);
        if (is<CORSFilteredResponse>(*this))
            return CORSFilteredResponse::create(vm, internal_response);
        if (is<OpaqueFilteredResponse>(*this))
            return OpaqueFilteredResponse::create(vm, internal_response);
        if (is<OpaqueRedirectFilteredResponse>(*this))
            return OpaqueRedirectFilteredResponse::create(vm, internal_response);
        VERIFY_NOT_REACHED();
    }

    // 2. Let newResponse be a copy of response, except for its body.
    auto new_response = Infrastructure::Response::create(vm);
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
    new_response->set_body_info(m_body_info);
    // FIXME: service worker timing info

    // 3. If response’s body is non-null, then set newResponse’s body to the result of cloning response’s body.
    if (m_body)
        new_response->set_body(m_body->clone(realm));

    // 4. Return newResponse.
    return new_response;
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#unsafe-response
GC::Ref<Response> Response::unsafe_response()
{
    // A response's unsafe response is its internal response if it has one, and the response itself otherwise.
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

// https://fetch.spec.whatwg.org/#concept-fresh-response
bool Response::is_fresh() const
{
    // A fresh response is a response whose current age is within its freshness lifetime.
    return current_age() < freshness_lifetime();
}

// https://fetch.spec.whatwg.org/#concept-stale-while-revalidate-response
bool Response::is_stale_while_revalidate() const
{
    // A stale-while-revalidate response is a response that is not a fresh response and whose current age is within the stale-while-revalidate lifetime.
    return !is_fresh() && current_age() < stale_while_revalidate_lifetime();
}

// https://fetch.spec.whatwg.org/#concept-stale-response
bool Response::is_stale() const
{
    // A stale response is a response that is not a fresh response or a stale-while-revalidate response.
    return !is_fresh() && !is_stale_while_revalidate();
}

AK::Duration Response::current_age() const
{
    // FIXME: Let's get the correct time.
    auto const request_time = UnixDateTime::now() - AK::Duration::from_seconds(5);

    return HTTP::calculate_age(m_header_list, request_time, m_response_time);
}

AK::Duration Response::freshness_lifetime() const
{
    return HTTP::calculate_freshness_lifetime(m_status, m_header_list);
}

// https://httpwg.org/specs/rfc5861.html#n-the-stale-while-revalidate-cache-control-extension
AK::Duration Response::stale_while_revalidate_lifetime() const
{
    auto const elem = header_list()->get_decode_and_split("Cache-Control"sv);
    if (!elem.has_value())
        return {};

    for (auto const& directive : *elem) {
        if (directive.starts_with_bytes("stale-while-revalidate"sv)) {
            auto equal_offset = directive.find_byte_offset('=');
            if (!equal_offset.has_value()) {
                dbgln("Bogus directive: '{}'", directive);
                continue;
            }
            auto const value_string = directive.bytes_as_string_view().substring_view(equal_offset.value() + 1);
            auto maybe_value = value_string.to_number<i64>();
            if (!maybe_value.has_value()) {
                dbgln("Bogus directive: '{}'", directive);
                continue;
            }
            return AK::Duration::from_seconds(*maybe_value);
        }
    }

    return {};
}

// Non-standard

FilteredResponse::FilteredResponse(GC::Ref<Response> internal_response, NonnullRefPtr<HTTP::HeaderList> header_list)
    : Response(move(header_list))
    , m_internal_response(internal_response)
{
}

FilteredResponse::~FilteredResponse()
{
}

void FilteredResponse::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_internal_response);
}

GC::Ref<BasicFilteredResponse> BasicFilteredResponse::create(JS::VM& vm, GC::Ref<Response> internal_response)
{
    // A basic filtered response is a filtered response whose type is "basic" and header list excludes
    // any headers in internal response’s header list whose name is a forbidden response-header name.
    auto header_list = HTTP::HeaderList::create();

    for (auto const& header : *internal_response->header_list()) {
        if (!HTTP::is_forbidden_response_header_name(header.name))
            header_list->append(header);
    }

    return vm.heap().allocate<BasicFilteredResponse>(internal_response, move(header_list));
}

BasicFilteredResponse::BasicFilteredResponse(GC::Ref<Response> internal_response, NonnullRefPtr<HTTP::HeaderList> header_list)
    : FilteredResponse(internal_response, header_list)
    , m_header_list(move(header_list))
{
}

GC::Ref<CORSFilteredResponse> CORSFilteredResponse::create(JS::VM& vm, GC::Ref<Response> internal_response)
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

    return vm.heap().allocate<CORSFilteredResponse>(internal_response, header_list);
}

CORSFilteredResponse::CORSFilteredResponse(GC::Ref<Response> internal_response, NonnullRefPtr<HTTP::HeaderList> header_list)
    : FilteredResponse(internal_response, header_list)
    , m_header_list(move(header_list))
{
}

GC::Ref<OpaqueFilteredResponse> OpaqueFilteredResponse::create(JS::VM& vm, GC::Ref<Response> internal_response)
{
    // An opaque filtered response is a filtered response whose type is "opaque", URL list is the empty list,
    // status is 0, status message is the empty byte sequence, header list is empty, and body is null.
    return vm.heap().allocate<OpaqueFilteredResponse>(internal_response, HTTP::HeaderList::create());
}

OpaqueFilteredResponse::OpaqueFilteredResponse(GC::Ref<Response> internal_response, NonnullRefPtr<HTTP::HeaderList> header_list)
    : FilteredResponse(internal_response, header_list)
    , m_header_list(move(header_list))
{
}

GC::Ref<OpaqueRedirectFilteredResponse> OpaqueRedirectFilteredResponse::create(JS::VM& vm, GC::Ref<Response> internal_response)
{
    // An opaque-redirect filtered response is a filtered response whose type is "opaqueredirect",
    // status is 0, status message is the empty byte sequence, header list is empty, and body is null.
    return vm.heap().allocate<OpaqueRedirectFilteredResponse>(internal_response, HTTP::HeaderList::create());
}

OpaqueRedirectFilteredResponse::OpaqueRedirectFilteredResponse(GC::Ref<Response> internal_response, NonnullRefPtr<HTTP::HeaderList> header_list)
    : FilteredResponse(internal_response, header_list)
    , m_header_list(move(header_list))
{
}

}
