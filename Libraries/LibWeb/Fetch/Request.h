/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/RequestPrototype.h>
#include <LibWeb/Fetch/Body.h>
#include <LibWeb/Fetch/BodyInit.h>
#include <LibWeb/Fetch/Headers.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch {

// https://fetch.spec.whatwg.org/#requestinfo
using RequestInfo = Variant<JS::Handle<Request>, String>;

// https://fetch.spec.whatwg.org/#requestinit
struct RequestInit {
    Optional<String> method;
    Optional<HeadersInit> headers;
    Optional<Optional<BodyInit>> body;
    Optional<String> referrer;
    Optional<Bindings::ReferrerPolicy> referrer_policy;
    Optional<Bindings::RequestMode> mode;
    Optional<Bindings::RequestCredentials> credentials;
    Optional<Bindings::RequestCache> cache;
    Optional<Bindings::RequestRedirect> redirect;
    Optional<String> integrity;
    Optional<bool> keepalive;
    Optional<JS::GCPtr<DOM::AbortSignal>> signal;
    Optional<Bindings::RequestDuplex> duplex;
    Optional<Bindings::RequestPriority> priority;
    Optional<JS::Value> window;

    // https://infra.spec.whatwg.org/#map-is-empty
    bool is_empty() const
    {
        return !(method.has_value()
            || headers.has_value()
            || body.has_value()
            || referrer.has_value()
            || referrer_policy.has_value()
            || mode.has_value()
            || credentials.has_value()
            || cache.has_value()
            || redirect.has_value()
            || integrity.has_value()
            || keepalive.has_value()
            || signal.has_value()
            || duplex.has_value()
            || priority.has_value()
            || window.has_value());
    }
};

// https://fetch.spec.whatwg.org/#request
class Request final
    : public Bindings::PlatformObject
    , public BodyMixin {
    WEB_PLATFORM_OBJECT(Request, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(Request);

public:
    [[nodiscard]] static JS::NonnullGCPtr<Request> create(JS::Realm&, JS::NonnullGCPtr<Infrastructure::Request>, Headers::Guard, JS::NonnullGCPtr<DOM::AbortSignal>);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<Request>> construct_impl(JS::Realm&, RequestInfo const& input, RequestInit const& init = {});

    virtual ~Request() override;

    // ^BodyMixin
    virtual Optional<MimeSniff::MimeType> mime_type_impl() const override;
    virtual JS::GCPtr<Infrastructure::Body> body_impl() override;
    virtual JS::GCPtr<Infrastructure::Body const> body_impl() const override;
    virtual Bindings::PlatformObject& as_platform_object() override { return *this; }
    virtual Bindings::PlatformObject const& as_platform_object() const override { return *this; }

    [[nodiscard]] JS::NonnullGCPtr<Infrastructure::Request> request() const { return m_request; }

    // JS API functions
    [[nodiscard]] String method() const;
    [[nodiscard]] String url() const;
    [[nodiscard]] JS::NonnullGCPtr<Headers> headers() const;
    [[nodiscard]] Bindings::RequestDestination destination() const;
    [[nodiscard]] String referrer() const;
    [[nodiscard]] Bindings::ReferrerPolicy referrer_policy() const;
    [[nodiscard]] Bindings::RequestMode mode() const;
    [[nodiscard]] Bindings::RequestCredentials credentials() const;
    [[nodiscard]] Bindings::RequestCache cache() const;
    [[nodiscard]] Bindings::RequestRedirect redirect() const;
    [[nodiscard]] String integrity() const;
    [[nodiscard]] bool keepalive() const;
    [[nodiscard]] bool is_reload_navigation() const;
    [[nodiscard]] bool is_history_navigation() const;
    [[nodiscard]] JS::NonnullGCPtr<DOM::AbortSignal> signal() const;
    [[nodiscard]] Bindings::RequestDuplex duplex() const;
    [[nodiscard]] WebIDL::ExceptionOr<JS::NonnullGCPtr<Request>> clone() const;

private:
    Request(JS::Realm&, JS::NonnullGCPtr<Infrastructure::Request>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // https://fetch.spec.whatwg.org/#concept-request-request
    // A Request object has an associated request (a request).
    JS::NonnullGCPtr<Infrastructure::Request> m_request;

    // https://fetch.spec.whatwg.org/#request-headers
    // A Request object also has an associated headers (null or a Headers object), initially null.
    JS::GCPtr<Headers> m_headers;

    // https://fetch.spec.whatwg.org/#request-signal
    // A Request object has an associated signal (null or an AbortSignal object), initially null.
    JS::GCPtr<DOM::AbortSignal> m_signal;
};

}
