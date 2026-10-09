/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>

namespace Web::Fetch::Infrastructure {

GC_DEFINE_ALLOCATOR(ResponseView);

GC::Ref<ResponseView> ResponseView::create(NonnullRefPtr<Response const> record, GC::Ptr<Body> body_view)
{
    VERIFY(record->unsafe_response()->body().has_value() == (body_view != nullptr));
    return GC::Heap::the().allocate<ResponseView>(move(record), body_view);
}

GC::Ref<ResponseView> ResponseView::create(JS::Realm& realm, NonnullRefPtr<Response const> record)
{
    auto body = record->unsafe_response()->body();
    if (!body.has_value())
        return create(record, nullptr);
    return create(record, Body::create_over_bytes(realm, body->bytes().get<Core::ImmutableBytes>()));
}

GC::Ref<ResponseView> ResponseView::network_error(String message)
{
    return create(Response::network_error(move(message)), nullptr);
}

GC::Ref<ResponseView> ResponseView::aborted_network_error()
{
    return create(Response::aborted_network_error(), nullptr);
}

ResponseView::ResponseView(NonnullRefPtr<Response const> record, GC::Ptr<Body> body_view)
    : m_record(move(record))
    , m_body_view(body_view)
{
}

void ResponseView::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_body_view);
}

Optional<u64> ResponseView::body_length() const
{
    if (auto body = m_record->body(); body.has_value())
        return body->length();
    return {};
}

Optional<Core::ImmutableBytes> ResponseView::body_source() const
{
    auto body = m_record->body();
    if (!body.has_value())
        return {};
    if (auto source = body->source(); source.has_value())
        return *source;
    return {};
}

// https://html.spec.whatwg.org/multipage/urls-and-fetching.html#unsafe-response
GC::Ref<ResponseView> ResponseView::unsafe_response()
{
    auto unsafe_response = m_record->unsafe_response();
    if (unsafe_response == m_record)
        return *this;
    return create(unsafe_response, m_body_view);
}

// https://fetch.spec.whatwg.org/#concept-response-clone
GC::Ref<ResponseView> ResponseView::clone(JS::Realm& realm) const
{
    auto body_view = m_body_view ? m_body_view->clone(realm) : GC::Ptr<Body> {};
    return create(m_record->clone(), body_view);
}

}
