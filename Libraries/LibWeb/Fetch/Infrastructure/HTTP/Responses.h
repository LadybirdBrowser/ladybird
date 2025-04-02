/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Headers.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Statuses.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#concept-response
class Response : public JS::Cell {
    GC_CELL(Response, JS::Cell);
    GC_DECLARE_ALLOCATOR(Response);

public:
    enum class CacheState {
        Local,
        Validated,
    };

    enum class Type {
        Basic,
        CORS,
        Default,
        Error,
        Opaque,
        OpaqueRedirect,
    };

    // https://fetch.spec.whatwg.org/#response-body-info
    struct BodyInfo {
        // https://fetch.spec.whatwg.org/#fetch-timing-info-encoded-body-size
        u64 encoded_size { 0 };

        // https://fetch.spec.whatwg.org/#fetch-timing-info-decoded-body-size
        u64 decoded_size { 0 };

        // https://fetch.spec.whatwg.org/#response-body-info-content-type
        String content_type {};

        bool operator==(BodyInfo const&) const = default;
    };

    [[nodiscard]] static GC::Ref<Response> create(JS::VM&);
    [[nodiscard]] static GC::Ref<Response> aborted_network_error(JS::VM&);
    [[nodiscard]] static GC::Ref<Response> network_error(JS::VM&, String message);
    [[nodiscard]] static GC::Ref<Response> appropriate_network_error(JS::VM&, FetchParams const&);

    virtual ~Response() = default;

    [[nodiscard]] virtual Type type() const { return m_type; }
    void set_type(Type type) { m_type = type; }

    [[nodiscard]] virtual bool aborted() const { return m_aborted; }
    virtual void set_aborted(bool aborted) { m_aborted = aborted; }

    [[nodiscard]] virtual Vector<URL::URL> const& url_list() const { return m_url_list; }
    [[nodiscard]] virtual Vector<URL::URL>& url_list() { return m_url_list; }
    virtual void set_url_list(Vector<URL::URL> url_list) { m_url_list = move(url_list); }

    [[nodiscard]] virtual Status status() const { return m_status; }
    virtual void set_status(Status status) { m_status = status; }

    [[nodiscard]] virtual ReadonlyBytes status_message() const { return m_status_message; }
    virtual void set_status_message(ByteBuffer status_message) { m_status_message = move(status_message); }

    [[nodiscard]] virtual GC::Ref<HeaderList> header_list() const { return m_header_list; }
    virtual void set_header_list(GC::Ref<HeaderList> header_list) { m_header_list = header_list; }

    [[nodiscard]] virtual GC::Ptr<Body> body() const { return m_body; }
    virtual void set_body(GC::Ptr<Body> body) { m_body = body; }

    [[nodiscard]] virtual Optional<CacheState> const& cache_state() const { return m_cache_state; }
    virtual void set_cache_state(Optional<CacheState> cache_state) { m_cache_state = move(cache_state); }

    [[nodiscard]] virtual Vector<ByteBuffer> const& cors_exposed_header_name_list() const { return m_cors_exposed_header_name_list; }
    virtual void set_cors_exposed_header_name_list(Vector<ByteBuffer> cors_exposed_header_name_list) { m_cors_exposed_header_name_list = move(cors_exposed_header_name_list); }

    [[nodiscard]] virtual bool range_requested() const { return m_range_requested; }
    virtual void set_range_requested(bool range_requested) { m_range_requested = range_requested; }

    [[nodiscard]] virtual bool request_includes_credentials() const { return m_request_includes_credentials; }
    virtual void set_request_includes_credentials(bool request_includes_credentials) { m_request_includes_credentials = request_includes_credentials; }

    [[nodiscard]] virtual bool timing_allow_passed() const { return m_timing_allow_passed; }
    virtual void set_timing_allow_passed(bool timing_allow_passed) { m_timing_allow_passed = timing_allow_passed; }

    [[nodiscard]] virtual BodyInfo const& body_info() const { return m_body_info; }
    virtual void set_body_info(BodyInfo body_info) { m_body_info = body_info; }

    [[nodiscard]] bool has_cross_origin_redirects() const { return m_has_cross_origin_redirects; }
    void set_has_cross_origin_redirects(bool has_cross_origin_redirects) { m_has_cross_origin_redirects = has_cross_origin_redirects; }

    [[nodiscard]] bool is_aborted_network_error() const;
    [[nodiscard]] bool is_network_error() const;

    [[nodiscard]] Optional<URL::URL const&> url() const;
    [[nodiscard]] ErrorOr<Optional<URL::URL>> location_url(Optional<String> const& request_fragment) const;

    [[nodiscard]] GC::Ref<Response> clone(JS::Realm&) const;

    [[nodiscard]] GC::Ref<Response> unsafe_response();

    [[nodiscard]] bool is_cors_same_origin() const;
    [[nodiscard]] bool is_cors_cross_origin() const;

    [[nodiscard]] bool is_fresh() const;
    [[nodiscard]] bool is_stale_while_revalidate() const;
    [[nodiscard]] bool is_stale() const;

    // Non-standard
    [[nodiscard]] Optional<String> const& network_error_message() const { return m_network_error_message; }
    MonotonicTime response_time() const { return m_response_time; }

protected:
    explicit Response(GC::Ref<HeaderList>);

    virtual void visit_edges(JS::Cell::Visitor&) override;

private:
    // https://fetch.spec.whatwg.org/#concept-response-type
    // A response has an associated type which is "basic", "cors", "default", "error", "opaque", or "opaqueredirect". Unless stated otherwise, it is "default".
    Type m_type { Type::Default };

    // https://fetch.spec.whatwg.org/#concept-response-aborted
    // A response can have an associated aborted flag, which is initially unset.
    bool m_aborted { false };

    // https://fetch.spec.whatwg.org/#concept-response-url-list
    // A response has an associated URL list (a list of zero or more URLs). Unless stated otherwise, it is the empty list.
    Vector<URL::URL> m_url_list;

    // https://fetch.spec.whatwg.org/#concept-response-status
    // A response has an associated status, which is a status. Unless stated otherwise it is 200.
    Status m_status { 200 };

    // https://fetch.spec.whatwg.org/#concept-response-status-message
    // A response has an associated status message. Unless stated otherwise it is the empty byte sequence.
    ByteBuffer m_status_message;

    // https://fetch.spec.whatwg.org/#concept-response-header-list
    // A response has an associated header list (a header list). Unless stated otherwise it is empty.
    GC::Ref<HeaderList> m_header_list;

    // https://fetch.spec.whatwg.org/#concept-response-body
    // A response has an associated body (null or a body). Unless stated otherwise it is null.
    GC::Ptr<Body> m_body;

    // https://fetch.spec.whatwg.org/#concept-response-cache-state
    // A response has an associated cache state (the empty string, "local", or "validated"). Unless stated otherwise, it is the empty string.
    Optional<CacheState> m_cache_state;

    // https://fetch.spec.whatwg.org/#concept-response-cors-exposed-header-name-list
    // A response has an associated CORS-exposed header-name list (a list of zero or more header names). The list is empty unless otherwise specified.
    Vector<ByteBuffer> m_cors_exposed_header_name_list;

    // https://fetch.spec.whatwg.org/#concept-response-range-requested-flag
    // A response has an associated range-requested flag, which is initially unset.
    bool m_range_requested { false };

    // https://fetch.spec.whatwg.org/#response-request-includes-credentials
    // A response has an associated request-includes-credentials (a boolean), which is initially true.
    bool m_request_includes_credentials { true };

    // https://fetch.spec.whatwg.org/#concept-response-timing-allow-passed
    // A response has an associated timing allow passed flag, which is initially unset.
    bool m_timing_allow_passed { false };

    // https://fetch.spec.whatwg.org/#concept-response-body-info
    // A response has an associated body info (a response body info). Unless stated otherwise, it is a new response body info.
    BodyInfo m_body_info;

    // https://fetch.spec.whatwg.org/#response-service-worker-timing-info
    // FIXME: A response has an associated service worker timing info (null or a service worker timing info), which is initially null.

    // https://fetch.spec.whatwg.org/#response-has-cross-origin-redirects
    // A response has an associated has-cross-origin-redirects (a boolean), which is initially false.
    bool m_has_cross_origin_redirects { false };

    // FIXME is the type correct?
    u64 current_age() const;
    u64 freshness_lifetime() const;
    u64 stale_while_revalidate_lifetime() const;

    // Non-standard
    ByteBuffer m_method;
    MonotonicTime m_response_time;

    Optional<String> m_network_error_message;

public:
    [[nodiscard]] ByteBuffer const& method() const { return m_method; }
    void set_method(ByteBuffer method) { m_method = move(method); }
};

// https://fetch.spec.whatwg.org/#concept-filtered-response
class FilteredResponse : public Response {
    GC_CELL(FilteredResponse, Response);

public:
    FilteredResponse(GC::Ref<Response>, GC::Ref<HeaderList>);
    virtual ~FilteredResponse() = 0;

    [[nodiscard]] virtual Type type() const override { return m_internal_response->type(); }

    [[nodiscard]] virtual bool aborted() const override { return m_internal_response->aborted(); }
    virtual void set_aborted(bool aborted) override { m_internal_response->set_aborted(aborted); }

    [[nodiscard]] virtual Vector<URL::URL> const& url_list() const override { return m_internal_response->url_list(); }
    [[nodiscard]] virtual Vector<URL::URL>& url_list() override { return m_internal_response->url_list(); }
    virtual void set_url_list(Vector<URL::URL> url_list) override { m_internal_response->set_url_list(move(url_list)); }

    [[nodiscard]] virtual Status status() const override { return m_internal_response->status(); }
    virtual void set_status(Status status) override { m_internal_response->set_status(status); }

    [[nodiscard]] virtual ReadonlyBytes status_message() const override { return m_internal_response->status_message(); }
    virtual void set_status_message(ByteBuffer status_message) override { m_internal_response->set_status_message(move(status_message)); }

    [[nodiscard]] virtual GC::Ref<HeaderList> header_list() const override { return m_internal_response->header_list(); }
    virtual void set_header_list(GC::Ref<HeaderList> header_list) override { m_internal_response->set_header_list(header_list); }

    [[nodiscard]] virtual GC::Ptr<Body> body() const override { return m_internal_response->body(); }
    virtual void set_body(GC::Ptr<Body> body) override { m_internal_response->set_body(body); }

    [[nodiscard]] virtual Optional<CacheState> const& cache_state() const override { return m_internal_response->cache_state(); }
    virtual void set_cache_state(Optional<CacheState> cache_state) override { m_internal_response->set_cache_state(move(cache_state)); }

    [[nodiscard]] virtual Vector<ByteBuffer> const& cors_exposed_header_name_list() const override { return m_internal_response->cors_exposed_header_name_list(); }
    virtual void set_cors_exposed_header_name_list(Vector<ByteBuffer> cors_exposed_header_name_list) override { m_internal_response->set_cors_exposed_header_name_list(move(cors_exposed_header_name_list)); }

    [[nodiscard]] virtual bool range_requested() const override { return m_internal_response->range_requested(); }
    virtual void set_range_requested(bool range_requested) override { m_internal_response->set_range_requested(range_requested); }

    [[nodiscard]] virtual bool request_includes_credentials() const override { return m_internal_response->request_includes_credentials(); }
    virtual void set_request_includes_credentials(bool request_includes_credentials) override { m_internal_response->set_request_includes_credentials(request_includes_credentials); }

    [[nodiscard]] virtual bool timing_allow_passed() const override { return m_internal_response->timing_allow_passed(); }
    virtual void set_timing_allow_passed(bool timing_allow_passed) override { m_internal_response->set_timing_allow_passed(timing_allow_passed); }

    [[nodiscard]] virtual BodyInfo const& body_info() const override { return m_internal_response->body_info(); }
    virtual void set_body_info(BodyInfo body_info) override { m_internal_response->set_body_info(move(body_info)); }

    [[nodiscard]] GC::Ref<Response> internal_response() const { return m_internal_response; }

protected:
    virtual void visit_edges(JS::Cell::Visitor&) override;

private:
    // https://fetch.spec.whatwg.org/#concept-internal-response
    GC::Ref<Response> m_internal_response;
};

// https://fetch.spec.whatwg.org/#concept-filtered-response-basic
class BasicFilteredResponse final : public FilteredResponse {
    GC_CELL(BasicFilteredResponse, FilteredResponse);
    GC_DECLARE_ALLOCATOR(BasicFilteredResponse);

public:
    [[nodiscard]] static GC::Ref<BasicFilteredResponse> create(JS::VM&, GC::Ref<Response>);

    [[nodiscard]] virtual Type type() const override { return Type::Basic; }
    [[nodiscard]] virtual GC::Ref<HeaderList> header_list() const override { return m_header_list; }

private:
    BasicFilteredResponse(GC::Ref<Response>, GC::Ref<HeaderList>);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    GC::Ref<HeaderList> m_header_list;
};

// https://fetch.spec.whatwg.org/#concept-filtered-response-cors
class CORSFilteredResponse final : public FilteredResponse {
    GC_CELL(CORSFilteredResponse, FilteredResponse);
    GC_DECLARE_ALLOCATOR(CORSFilteredResponse);

public:
    [[nodiscard]] static GC::Ref<CORSFilteredResponse> create(JS::VM&, GC::Ref<Response>);

    [[nodiscard]] virtual Type type() const override { return Type::CORS; }
    [[nodiscard]] virtual GC::Ref<HeaderList> header_list() const override { return m_header_list; }

private:
    CORSFilteredResponse(GC::Ref<Response>, GC::Ref<HeaderList>);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    GC::Ref<HeaderList> m_header_list;
};

// https://fetch.spec.whatwg.org/#concept-filtered-response-opaque
class OpaqueFilteredResponse final : public FilteredResponse {
    GC_CELL(OpaqueFilteredResponse, FilteredResponse);
    GC_DECLARE_ALLOCATOR(OpaqueFilteredResponse);

public:
    [[nodiscard]] static GC::Ref<OpaqueFilteredResponse> create(JS::VM&, GC::Ref<Response>);

    [[nodiscard]] virtual Type type() const override { return Type::Opaque; }
    [[nodiscard]] virtual Vector<URL::URL> const& url_list() const override { return m_url_list; }
    [[nodiscard]] virtual Vector<URL::URL>& url_list() override { return m_url_list; }
    [[nodiscard]] virtual Status status() const override { return 0; }
    [[nodiscard]] virtual ReadonlyBytes status_message() const override { return {}; }
    [[nodiscard]] virtual GC::Ref<HeaderList> header_list() const override { return m_header_list; }
    [[nodiscard]] virtual GC::Ptr<Body> body() const override { return nullptr; }

private:
    OpaqueFilteredResponse(GC::Ref<Response>, GC::Ref<HeaderList>);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    Vector<URL::URL> m_url_list;
    GC::Ref<HeaderList> m_header_list;
};

// https://fetch.spec.whatwg.org/#concept-filtered-response-opaque-redirect
class OpaqueRedirectFilteredResponse final : public FilteredResponse {
    GC_CELL(OpaqueRedirectFilteredResponse, FilteredResponse);
    GC_DECLARE_ALLOCATOR(OpaqueRedirectFilteredResponse);

public:
    [[nodiscard]] static GC::Ref<OpaqueRedirectFilteredResponse> create(JS::VM&, GC::Ref<Response>);

    [[nodiscard]] virtual Type type() const override { return Type::OpaqueRedirect; }
    [[nodiscard]] virtual Status status() const override { return 0; }
    [[nodiscard]] virtual ReadonlyBytes status_message() const override { return {}; }
    [[nodiscard]] virtual GC::Ref<HeaderList> header_list() const override { return m_header_list; }
    [[nodiscard]] virtual GC::Ptr<Body> body() const override { return nullptr; }

private:
    OpaqueRedirectFilteredResponse(GC::Ref<Response>, GC::Ref<HeaderList>);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    GC::Ref<HeaderList> m_header_list;
};
}
