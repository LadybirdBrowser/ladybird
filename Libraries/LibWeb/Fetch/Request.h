/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Bindings/Request.h>
#include <LibWeb/Fetch/Body.h>
#include <LibWeb/Fetch/BodyInit.h>
#include <LibWeb/Fetch/Headers.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch {

// https://fetch.spec.whatwg.org/#requestinfo
using RequestInfo = Variant<GC::Root<Request>, String>;

// https://fetch.spec.whatwg.org/#request
class Request final
    : public Bindings::PlatformObject
    , public BodyMixin {
    WEB_PLATFORM_OBJECT(Request, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Request);

public:
    [[nodiscard]] static GC::Ref<Request> create(JS::Realm&, GC::Ref<Infrastructure::Request>, Headers::Guard, GC::Ref<DOM::AbortSignal>);
    static WebIDL::ExceptionOr<GC::Ref<Request>> construct_impl(JS::Realm&, RequestInfo const& input, Bindings::RequestInit const& init = {});

    virtual ~Request() override;

    // ^BodyMixin
    virtual Optional<MimeSniff::MimeType> mime_type_impl() const override;
    virtual GC::Ptr<Infrastructure::Body> body_impl() override;
    virtual GC::Ptr<Infrastructure::Body const> body_impl() const override;
    virtual Bindings::PlatformObject& as_platform_object() override { return *this; }
    virtual Bindings::PlatformObject const& as_platform_object() const override { return *this; }

    [[nodiscard]] GC::Ref<Infrastructure::Request> request() const { return m_request; }

    // JS API functions
    [[nodiscard]] String method() const;
    [[nodiscard]] String url() const;
    [[nodiscard]] GC::Ref<Headers> headers() const;
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
    [[nodiscard]] GC::Ref<DOM::AbortSignal> signal() const;
    [[nodiscard]] Bindings::RequestDuplex duplex() const;
    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<Request>> clone() const;

private:
    Request(JS::Realm&, GC::Ref<Infrastructure::Request>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // https://fetch.spec.whatwg.org/#concept-request-request
    // A Request object has an associated request (a request).
    GC::Ref<Infrastructure::Request> m_request;

    // https://fetch.spec.whatwg.org/#request-headers
    // A Request object also has an associated headers (null or a Headers object), initially null.
    GC::Ptr<Headers> m_headers;

    // https://fetch.spec.whatwg.org/#request-signal
    // A Request object has an associated signal (null or an AbortSignal object), initially null.
    GC::Ptr<DOM::AbortSignal> m_signal;
};

}
