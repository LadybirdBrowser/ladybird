/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, networkException <networkexception@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibURL/Origin.h>
#include <LibURL/URL.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Headers.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#concept-request
class Request final : public JS::Cell {
    JS_CELL(Request, JS::Cell);
    JS_DECLARE_ALLOCATOR(Request);

public:
    enum class CacheMode {
        Default,
        NoStore,
        Reload,
        NoCache,
        ForceCache,
        OnlyIfCached,
    };

    enum class CredentialsMode {
        Omit,
        SameOrigin,
        Include,
    };

    enum class Destination {
        Audio,
        AudioWorklet,
        Document,
        Embed,
        Font,
        Frame,
        IFrame,
        Image,
        JSON,
        Manifest,
        Object,
        PaintWorklet,
        Report,
        Script,
        ServiceWorker,
        SharedWorker,
        Style,
        Track,
        Video,
        WebIdentity,
        Worker,
        XSLT,
    };

    enum class Initiator {
        Download,
        ImageSet,
        Manifest,
        Prefetch,
        Prerender,
        XSLT,
    };

    enum class InitiatorType {
        Audio,
        Beacon,
        Body,
        CSS,
        EarlyHint,
        Embed,
        Fetch,
        Font,
        Frame,
        IFrame,
        Image,
        IMG,
        Input,
        Link,
        Object,
        Ping,
        Script,
        Track,
        Video,
        XMLHttpRequest,
        Other,
    };

    enum class Mode {
        SameOrigin,
        CORS,
        NoCORS,
        Navigate,
        WebSocket,
    };

    enum class Origin {
        Client,
    };

    enum class ParserMetadata {
        ParserInserted,
        NotParserInserted,
    };

    enum class PolicyContainer {
        Client,
    };

    enum class RedirectMode {
        Follow,
        Error,
        Manual,
    };

    enum class Referrer {
        NoReferrer,
        Client,
    };

    enum class ResponseTainting {
        Basic,
        CORS,
        Opaque,
    };

    enum class ServiceWorkersMode {
        All,
        None,
    };

    enum class Window {
        NoWindow,
        Client,
    };

    enum class Priority {
        High,
        Low,
        Auto
    };

    // AD-HOC: Some web features need to receive data as it arrives, rather than when the response is fully complete
    //         or when enough data has been buffered. Use this buffer policy to inform fetch of that requirement.
    enum class BufferPolicy {
        BufferResponse,
        DoNotBufferResponse,
    };

    // Members are implementation-defined
    struct InternalPriority { };

    using BodyType = Variant<Empty, ByteBuffer, JS::NonnullGCPtr<Body>>;
    using OriginType = Variant<Origin, URL::Origin>;
    using PolicyContainerType = Variant<PolicyContainer, HTML::PolicyContainer>;
    using ReferrerType = Variant<Referrer, URL::URL>;
    using ReservedClientType = JS::GCPtr<HTML::Environment>;
    using WindowType = Variant<Window, JS::GCPtr<HTML::EnvironmentSettingsObject>>;

    [[nodiscard]] static JS::NonnullGCPtr<Request> create(JS::VM&);

    [[nodiscard]] ReadonlyBytes method() const { return m_method; }
    void set_method(ByteBuffer method) { m_method = move(method); }

    [[nodiscard]] bool local_urls_only() const { return m_local_urls_only; }
    void set_local_urls_only(bool local_urls_only) { m_local_urls_only = local_urls_only; }

    [[nodiscard]] JS::NonnullGCPtr<HeaderList> header_list() const { return m_header_list; }
    void set_header_list(JS::NonnullGCPtr<HeaderList> header_list) { m_header_list = header_list; }

    [[nodiscard]] bool unsafe_request() const { return m_unsafe_request; }
    void set_unsafe_request(bool unsafe_request) { m_unsafe_request = unsafe_request; }

    [[nodiscard]] BodyType const& body() const { return m_body; }
    [[nodiscard]] BodyType& body() { return m_body; }
    void set_body(BodyType body) { m_body = move(body); }

    [[nodiscard]] JS::GCPtr<HTML::EnvironmentSettingsObject const> client() const { return m_client; }
    [[nodiscard]] JS::GCPtr<HTML::EnvironmentSettingsObject> client() { return m_client; }
    void set_client(HTML::EnvironmentSettingsObject* client) { m_client = client; }

    [[nodiscard]] ReservedClientType const& reserved_client() const { return m_reserved_client; }
    [[nodiscard]] ReservedClientType& reserved_client() { return m_reserved_client; }
    void set_reserved_client(ReservedClientType reserved_client) { m_reserved_client = move(reserved_client); }

    [[nodiscard]] String const& replaces_client_id() const { return m_replaces_client_id; }
    void set_replaces_client_id(String replaces_client_id) { m_replaces_client_id = move(replaces_client_id); }

    [[nodiscard]] WindowType const& window() const { return m_window; }
    void set_window(WindowType window) { m_window = move(window); }

    [[nodiscard]] bool keepalive() const { return m_keepalive; }
    void set_keepalive(bool keepalive) { m_keepalive = keepalive; }

    [[nodiscard]] Optional<InitiatorType> const& initiator_type() const { return m_initiator_type; }
    void set_initiator_type(Optional<InitiatorType> initiator_type) { m_initiator_type = move(initiator_type); }

    [[nodiscard]] ServiceWorkersMode service_workers_mode() const { return m_service_workers_mode; }
    void set_service_workers_mode(ServiceWorkersMode service_workers_mode) { m_service_workers_mode = service_workers_mode; }

    [[nodiscard]] Optional<Initiator> const& initiator() const { return m_initiator; }
    void set_initiator(Optional<Initiator> initiator) { m_initiator = move(initiator); }

    [[nodiscard]] Optional<Destination> const& destination() const { return m_destination; }
    void set_destination(Optional<Destination> destination) { m_destination = move(destination); }

    [[nodiscard]] Priority const& priority() const { return m_priority; }
    void set_priority(Priority priority) { m_priority = priority; }

    [[nodiscard]] OriginType const& origin() const { return m_origin; }
    void set_origin(OriginType origin) { m_origin = move(origin); }

    [[nodiscard]] PolicyContainerType const& policy_container() const { return m_policy_container; }
    void set_policy_container(PolicyContainerType policy_container) { m_policy_container = move(policy_container); }

    [[nodiscard]] Mode mode() const { return m_mode; }
    void set_mode(Mode mode) { m_mode = mode; }

    [[nodiscard]] bool use_cors_preflight() const { return m_use_cors_preflight; }
    void set_use_cors_preflight(bool use_cors_preflight) { m_use_cors_preflight = use_cors_preflight; }

    [[nodiscard]] CredentialsMode credentials_mode() const { return m_credentials_mode; }
    void set_credentials_mode(CredentialsMode credentials_mode) { m_credentials_mode = credentials_mode; }

    [[nodiscard]] bool use_url_credentials() const { return m_use_url_credentials; }
    void set_use_url_credentials(bool use_url_credentials) { m_use_url_credentials = use_url_credentials; }

    [[nodiscard]] CacheMode cache_mode() const { return m_cache_mode; }
    void set_cache_mode(CacheMode cache_mode) { m_cache_mode = cache_mode; }

    [[nodiscard]] RedirectMode redirect_mode() const { return m_redirect_mode; }
    void set_redirect_mode(RedirectMode redirect_mode) { m_redirect_mode = redirect_mode; }

    [[nodiscard]] String const& integrity_metadata() const { return m_integrity_metadata; }
    void set_integrity_metadata(String integrity_metadata) { m_integrity_metadata = move(integrity_metadata); }

    [[nodiscard]] String const& cryptographic_nonce_metadata() const { return m_cryptographic_nonce_metadata; }
    void set_cryptographic_nonce_metadata(String cryptographic_nonce_metadata) { m_cryptographic_nonce_metadata = move(cryptographic_nonce_metadata); }

    [[nodiscard]] Optional<ParserMetadata> const& parser_metadata() const { return m_parser_metadata; }
    void set_parser_metadata(Optional<ParserMetadata> parser_metadata) { m_parser_metadata = move(parser_metadata); }

    [[nodiscard]] bool reload_navigation() const { return m_reload_navigation; }
    void set_reload_navigation(bool reload_navigation) { m_reload_navigation = reload_navigation; }

    [[nodiscard]] bool history_navigation() const { return m_history_navigation; }
    void set_history_navigation(bool history_navigation) { m_history_navigation = history_navigation; }

    [[nodiscard]] bool user_activation() const { return m_user_activation; }
    void set_user_activation(bool user_activation) { m_user_activation = user_activation; }

    [[nodiscard]] bool render_blocking() const { return m_render_blocking; }
    void set_render_blocking(bool render_blocking) { m_render_blocking = render_blocking; }

    [[nodiscard]] Vector<URL::URL> const& url_list() const { return m_url_list; }
    [[nodiscard]] Vector<URL::URL>& url_list() { return m_url_list; }
    void set_url_list(Vector<URL::URL> url_list) { m_url_list = move(url_list); }

    [[nodiscard]] u8 redirect_count() const { return m_redirect_count; }
    void set_redirect_count(u8 redirect_count) { m_redirect_count = redirect_count; }

    [[nodiscard]] ReferrerType const& referrer() const { return m_referrer; }
    void set_referrer(ReferrerType referrer) { m_referrer = move(referrer); }

    [[nodiscard]] ReferrerPolicy::ReferrerPolicy const& referrer_policy() const { return m_referrer_policy; }
    void set_referrer_policy(ReferrerPolicy::ReferrerPolicy referrer_policy) { m_referrer_policy = move(referrer_policy); }

    [[nodiscard]] ResponseTainting response_tainting() const { return m_response_tainting; }
    void set_response_tainting(ResponseTainting response_tainting) { m_response_tainting = response_tainting; }

    [[nodiscard]] bool prevent_no_cache_cache_control_header_modification() const { return m_prevent_no_cache_cache_control_header_modification; }
    void set_prevent_no_cache_cache_control_header_modification(bool prevent_no_cache_cache_control_header_modification) { m_prevent_no_cache_cache_control_header_modification = prevent_no_cache_cache_control_header_modification; }

    [[nodiscard]] bool done() const { return m_done; }
    void set_done(bool done) { m_done = done; }

    [[nodiscard]] bool timing_allow_failed() const { return m_timing_allow_failed; }
    void set_timing_allow_failed(bool timing_allow_failed) { m_timing_allow_failed = timing_allow_failed; }

    [[nodiscard]] URL::URL& url();
    [[nodiscard]] URL::URL const& url() const;
    [[nodiscard]] URL::URL& current_url();
    [[nodiscard]] URL::URL const& current_url() const;
    void set_url(URL::URL url);

    [[nodiscard]] bool destination_is_script_like() const;

    [[nodiscard]] bool is_subresource_request() const;
    [[nodiscard]] bool is_non_subresource_request() const;
    [[nodiscard]] bool is_navigation_request() const;

    [[nodiscard]] bool has_redirect_tainted_origin() const;

    [[nodiscard]] String serialize_origin() const;
    [[nodiscard]] ByteBuffer byte_serialize_origin() const;

    [[nodiscard]] JS::NonnullGCPtr<Request> clone(JS::Realm&) const;

    void add_range_header(u64 first, Optional<u64> const& last);
    void add_origin_header();

    [[nodiscard]] bool cross_origin_embedder_policy_allows_credentials() const;

    // Non-standard
    void add_pending_response(Badge<Fetching::PendingResponse>, JS::NonnullGCPtr<Fetching::PendingResponse> pending_response)
    {
        VERIFY(!m_pending_responses.contains_slow(pending_response));
        m_pending_responses.append(pending_response);
    }

    void remove_pending_response(Badge<Fetching::PendingResponse>, JS::NonnullGCPtr<Fetching::PendingResponse> pending_response)
    {
        m_pending_responses.remove_first_matching([&](auto gc_ptr) { return gc_ptr == pending_response; });
    }

    [[nodiscard]] BufferPolicy buffer_policy() const { return m_buffer_policy; }
    void set_buffer_policy(BufferPolicy buffer_policy) { m_buffer_policy = buffer_policy; }

private:
    explicit Request(JS::NonnullGCPtr<HeaderList>);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    // https://fetch.spec.whatwg.org/#concept-request-method
    // A request has an associated method (a method). Unless stated otherwise it is `GET`.
    ByteBuffer m_method { ByteBuffer::copy("GET"sv.bytes()).release_value() };

    // https://fetch.spec.whatwg.org/#local-urls-only-flag
    // A request has an associated local-URLs-only flag. Unless stated otherwise it is unset.
    bool m_local_urls_only { false };

    // https://fetch.spec.whatwg.org/#concept-request-header-list
    // A request has an associated header list (a header list). Unless stated otherwise it is empty.
    JS::NonnullGCPtr<HeaderList> m_header_list;

    // https://fetch.spec.whatwg.org/#unsafe-request-flag
    // A request has an associated unsafe-request flag. Unless stated otherwise it is unset.
    bool m_unsafe_request { false };

    // https://fetch.spec.whatwg.org/#concept-request-body
    // A request has an associated body (null, a byte sequence, or a body). Unless stated otherwise it is null.
    BodyType m_body;

    // https://fetch.spec.whatwg.org/#concept-request-client
    // A request has an associated client (null or an environment settings object).
    JS::GCPtr<HTML::EnvironmentSettingsObject> m_client;

    // https://fetch.spec.whatwg.org/#concept-request-reserved-client
    // A request has an associated reserved client (null, an environment, or an environment settings object). Unless
    // stated otherwise it is null.
    ReservedClientType m_reserved_client;

    // https://fetch.spec.whatwg.org/#concept-request-replaces-client-id
    // A request has an associated replaces client id (a string). Unless stated otherwise it is the empty string.
    String m_replaces_client_id;

    // https://fetch.spec.whatwg.org/#concept-request-window
    // A request has an associated window ("no-window", "client", or an environment settings object whose global object
    // is a Window object). Unless stated otherwise it is "client".
    WindowType m_window { Window::Client };

    // https://fetch.spec.whatwg.org/#request-keepalive-flag
    // A request has an associated boolean keepalive. Unless stated otherwise it is false.
    bool m_keepalive { false };

    // https://fetch.spec.whatwg.org/#request-initiator-type
    // A request has an associated initiator type, which is null, "audio", "beacon", "body", "css", "early-hint",
    // "embed", "fetch", "font", "frame", "iframe", "image", "img", "input", "link", "object", "ping", "script",
    // "track", "video", "xmlhttprequest", or "other". Unless stated otherwise it is null. [RESOURCE-TIMING]
    Optional<InitiatorType> m_initiator_type;

    // https://fetch.spec.whatwg.org/#request-service-workers-mode
    // A request has an associated service-workers mode, that is "all" or "none". Unless stated otherwise it is "all".
    ServiceWorkersMode m_service_workers_mode { ServiceWorkersMode::All };

    // https://fetch.spec.whatwg.org/#concept-request-initiator
    // A request has an associated initiator, which is the empty string, "download", "imageset", "manifest",
    // "prefetch", "prerender", or "xslt". Unless stated otherwise it is the empty string.
    Optional<Initiator> m_initiator;

    // https://fetch.spec.whatwg.org/#concept-request-destination
    // A request has an associated destination, which is the empty string, "audio", "audioworklet", "document",
    // "embed", "font", "frame", "iframe", "image", "json", "manifest", "object", "paintworklet", "report", "script",
    // "serviceworker", "sharedworker", "style", "track", "video", "webidentity", "worker", or "xslt". Unless stated
    // otherwise it is the empty string.
    // NOTE: These are reflected on RequestDestination except for "serviceworker" and "webidentity" as fetches with
    //       those destinations skip service workers.
    Optional<Destination> m_destination;

    // https://fetch.spec.whatwg.org/#request-priority
    // A request has an associated priority, which is "high", "low", or "auto". Unless stated otherwise it is "auto".
    Priority m_priority { Priority::Auto };

    // https://fetch.spec.whatwg.org/#request-internal-priority
    // A request has an associated internal priority (null or an implementation-defined object). Unless otherwise stated it is null.
    Optional<InternalPriority> m_internal_priority;

    // https://fetch.spec.whatwg.org/#concept-request-origin
    // A request has an associated origin, which is "client" or an origin. Unless stated otherwise it is "client".
    OriginType m_origin { Origin::Client };

    // https://fetch.spec.whatwg.org/#concept-request-policy-container
    // A request has an associated policy container, which is "client" or a policy container. Unless stated otherwise
    // it is "client".
    PolicyContainerType m_policy_container { PolicyContainer::Client };

    // https://fetch.spec.whatwg.org/#concept-request-referrer
    // A request has an associated referrer, which is "no-referrer", "client", or a URL. Unless stated otherwise it is
    // "client".
    ReferrerType m_referrer { Referrer::Client };

    // https://fetch.spec.whatwg.org/#concept-request-referrer-policy
    // A request has an associated referrer policy, which is a referrer policy. Unless stated otherwise it is the empty
    // string.
    ReferrerPolicy::ReferrerPolicy m_referrer_policy { ReferrerPolicy::ReferrerPolicy::EmptyString };

    // https://fetch.spec.whatwg.org/#concept-request-mode
    // A request has an associated mode, which is "same-origin", "cors", "no-cors", "navigate", or "websocket". Unless
    // stated otherwise, it is "no-cors".
    Mode m_mode { Mode::NoCORS };

    // https://fetch.spec.whatwg.org/#use-cors-preflight-flag
    // A request has an associated use-CORS-preflight flag. Unless stated otherwise, it is unset.
    bool m_use_cors_preflight { false };

    // https://fetch.spec.whatwg.org/#concept-request-credentials-mode
    // A request has an associated credentials mode, which is "omit", "same-origin", or "include". Unless stated
    // otherwise, it is "same-origin".
    CredentialsMode m_credentials_mode { CredentialsMode::SameOrigin };

    // https://fetch.spec.whatwg.org/#concept-request-use-url-credentials-flag
    // A request has an associated use-URL-credentials flag. Unless stated otherwise, it is unset.
    // NOTE: When this flag is set, when a request’s URL has a username and password, and there is an available
    //       authentication entry for the request, then the URL’s credentials are preferred over that of the
    //       authentication entry. Modern specifications avoid setting this flag, since putting credentials in URLs is
    //       discouraged, but some older features set it for compatibility reasons.
    bool m_use_url_credentials { false };

    // https://fetch.spec.whatwg.org/#concept-request-cache-mode
    // A request has an associated cache mode, which is "default", "no-store", "reload", "no-cache", "force-cache", or
    // "only-if-cached". Unless stated otherwise, it is "default".
    CacheMode m_cache_mode { CacheMode::Default };

    // https://fetch.spec.whatwg.org/#concept-request-redirect-mode
    // A request has an associated redirect mode, which is "follow", "error", or "manual". Unless stated otherwise, it
    // is "follow".
    RedirectMode m_redirect_mode { RedirectMode::Follow };

    // https://fetch.spec.whatwg.org/#concept-request-integrity-metadata
    // A request has associated integrity metadata (a string). Unless stated otherwise, it is the empty string.
    String m_integrity_metadata;

    // https://fetch.spec.whatwg.org/#concept-request-nonce-metadata
    // A request has associated cryptographic nonce metadata (a string). Unless stated otherwise, it is the empty
    // string.
    String m_cryptographic_nonce_metadata;

    // https://fetch.spec.whatwg.org/#concept-request-parser-metadata
    // A request has associated parser metadata which is the empty string, "parser-inserted", or
    // "not-parser-inserted". Unless otherwise stated, it is the empty string.
    Optional<ParserMetadata> m_parser_metadata;

    // https://fetch.spec.whatwg.org/#concept-request-reload-navigation-flag
    // A request has an associated reload-navigation flag. Unless stated otherwise, it is unset.
    bool m_reload_navigation { false };

    // https://fetch.spec.whatwg.org/#concept-request-history-navigation-flag
    // A request has an associated history-navigation flag. Unless stated otherwise, it is unset.
    bool m_history_navigation { false };

    // https://fetch.spec.whatwg.org/#request-user-activation
    // A request has an associated boolean user-activation. Unless stated otherwise, it is false.
    bool m_user_activation { false };

    // https://fetch.spec.whatwg.org/#request-render-blocking
    // A request has an associated boolean render-blocking. Unless stated otherwise, it is false.
    bool m_render_blocking { false };

    // https://fetch.spec.whatwg.org/#concept-request-url-list
    // A request has an associated URL list (a list of one or more URLs). Unless stated otherwise, it is a list
    // containing a copy of request’s URL.
    Vector<URL::URL> m_url_list;

    // https://fetch.spec.whatwg.org/#concept-request-redirect-count
    // A request has an associated redirect count. Unless stated otherwise, it is zero.
    // NOTE: '4.4. HTTP-redirect fetch' infers a limit of 20.
    u8 m_redirect_count { 0 };

    // https://fetch.spec.whatwg.org/#concept-request-response-tainting
    // A request has an associated response tainting, which is "basic", "cors", or "opaque". Unless stated otherwise,
    // it is "basic".
    ResponseTainting m_response_tainting { ResponseTainting::Basic };

    // https://fetch.spec.whatwg.org/#no-cache-prevent-cache-control
    // A request has an associated prevent no-cache cache-control header modification flag. Unless stated otherwise, it
    // is unset.
    bool m_prevent_no_cache_cache_control_header_modification { false };

    // https://fetch.spec.whatwg.org/#done-flag
    // A request has an associated done flag. Unless stated otherwise, it is unset.
    bool m_done { false };

    // https://fetch.spec.whatwg.org/#timing-allow-failed
    // A request has an associated timing allow failed flag. Unless stated otherwise, it is unset.
    bool m_timing_allow_failed { false };

    // Non-standard
    Vector<JS::NonnullGCPtr<Fetching::PendingResponse>> m_pending_responses;

    BufferPolicy m_buffer_policy { BufferPolicy::BufferResponse };
};

StringView request_destination_to_string(Request::Destination);
StringView request_mode_to_string(Request::Mode);

Optional<Request::Priority> request_priority_from_string(StringView);

}
