/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibHTTP/HeaderList.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/MIME.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>

namespace Web::Fetch::Infrastructure {

extern "C" {

// Called by Bindings/Rust/src/fetch/infrastructure/http/response_view.rs.
bool web_fetch_response_is_network_error(ResponseView*);
bool web_fetch_response_aborted(ResponseView*);
u16 web_fetch_response_status(ResponseView*);
void web_fetch_response_status_message(ResponseView*, void* bytes, void (*on_bytes)(void*, ReadonlyBytes));
bool web_fetch_response_get_header(ResponseView*, ReadonlyBytes name, void* bytes, void (*on_bytes)(void*, ReadonlyBytes));
void web_fetch_response_sort_and_combine(ResponseView*, void* headers, void (*on_header)(void*, ReadonlyBytes name, ReadonlyBytes value));
bool web_fetch_response_extract_length(ResponseView*, u64* out_length);
FlatPtr web_fetch_response_extract_mime_type(ResponseView*);
FlatPtr web_fetch_response_url(ResponseView*, bool exclude_fragment);
bool web_fetch_response_has_body(ResponseView*);
FlatPtr web_fetch_response_network_error_message(ResponseView*);
}

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

bool web_fetch_response_is_network_error(ResponseView* response)
{
    return response->is_network_error();
}

bool web_fetch_response_aborted(ResponseView* response)
{
    return response->aborted();
}

u16 web_fetch_response_status(ResponseView* response)
{
    return response->status();
}

void web_fetch_response_status_message(ResponseView* response, void* bytes, void (*on_bytes)(void*, ReadonlyBytes))
{
    on_bytes(bytes, response->status_message().bytes());
}

bool web_fetch_response_get_header(ResponseView* response, ReadonlyBytes name, void* bytes, void (*on_bytes)(void*, ReadonlyBytes))
{
    auto value = response->header_list()->get(StringView { name });
    if (!value.has_value())
        return false;
    on_bytes(bytes, value->bytes());
    return true;
}

void web_fetch_response_sort_and_combine(ResponseView* response, void* headers, void (*on_header)(void*, ReadonlyBytes name, ReadonlyBytes value))
{
    for (auto const& header : response->header_list()->sort_and_combine())
        on_header(headers, header.name.bytes(), header.value.bytes());
}

bool web_fetch_response_extract_length(ResponseView* response, u64* out_length)
{
    auto length = response->header_list()->extract_length();
    if (!length.has<u64>())
        return false;
    *out_length = length.get<u64>();
    return true;
}

FlatPtr web_fetch_response_extract_mime_type(ResponseView* response)
{
    auto mime_type = extract_mime_type(response->header_list());
    if (!mime_type.has_value())
        return 0;
    return Utf16String::from_utf8(mime_type->serialized()).into_raw();
}

FlatPtr web_fetch_response_url(ResponseView* response, bool exclude_fragment)
{
    auto url = response->url();
    if (!url.has_value())
        return 0;
    return Utf16String::from_utf8(url->serialize(exclude_fragment ? URL::ExcludeFragment::Yes : URL::ExcludeFragment::No)).into_raw();
}

bool web_fetch_response_has_body(ResponseView* response)
{
    return response->body() != nullptr;
}

FlatPtr web_fetch_response_network_error_message(ResponseView* response)
{
    auto const& message = response->network_error_message();
    if (!message.has_value())
        return 0;
    return Utf16String::from_utf8(*message).into_raw();
}

}
