/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/Response.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Fetch/Body.h>
#include <LibWeb/Fetch/BodyInit.h>
#include <LibWeb/Fetch/Headers.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::Fetch {

using ResponseInit = Bindings::ResponseInit;

// https://fetch.spec.whatwg.org/#response
class Response final
    : public Bindings::GCAllocatedWrappable
    , public BodyMixin {
    WEB_WRAPPABLE(Response, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(Response);

public:
    [[nodiscard]] static GC::Ref<Response> create(GC::Ref<Infrastructure::ResponseView>);
    [[nodiscard]] static GC::Ref<Response> create(GC::Ref<Infrastructure::ResponseView>, Headers::Guard);

    virtual ~Response() override;

    // ^BodyMixin
    virtual Optional<MimeSniff::MimeType> mime_type_impl() const override;
    virtual GC::Ptr<Infrastructure::Body> body_impl() override;
    virtual GC::Ptr<Infrastructure::Body const> body_impl() const override;
    using BodyMixin::array_buffer;
    using BodyMixin::blob;
    using BodyMixin::bytes;
    using BodyMixin::form_data;
    using BodyMixin::json;
    using BodyMixin::text;

    [[nodiscard]] GC::Ref<Infrastructure::ResponseView> response() const { return m_response; }

    [[nodiscard]] static GC::Ref<Response> error();
    static WebIDL::ExceptionOr<GC::Ref<Response>> redirect(Utf16String const& url, u16 status);
    static WebIDL::ExceptionOr<GC::Ref<Response>> create_for_constructor(JS::Object&, NullableBodyInit const& body, ResponseInit const& init);
    static WebIDL::ExceptionOr<GC::Ref<Response>> create_with_body(ResponseInit const& init, Optional<Infrastructure::BodyWithType> const&);
    static WebIDL::ExceptionOr<GC::Ref<Response>> json(JS::Realm&, JS::Value data, ResponseInit const& init);
    [[nodiscard]] Utf16String url() const;
    [[nodiscard]] Bindings::ResponseType type() const;
    [[nodiscard]] bool redirected() const;
    [[nodiscard]] u16 status() const;
    [[nodiscard]] bool ok() const;
    [[nodiscard]] String status_text() const;
    [[nodiscard]] GC::Ref<Headers> headers() const;
    [[nodiscard]] WebIDL::ExceptionOr<GC::Ref<Response>> clone(JS::Realm&) const;

private:
    explicit Response(GC::Ref<Infrastructure::ResponseView>);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    // https://fetch.spec.whatwg.org/#concept-response-response
    // A Response object has an associated response (a response).
    GC::Ref<Infrastructure::ResponseView> m_response;

    // https://fetch.spec.whatwg.org/#response-headers
    // A Response object also has an associated headers (null or a Headers object), initially null.
    GC::Ptr<Headers> m_headers;
};

}
