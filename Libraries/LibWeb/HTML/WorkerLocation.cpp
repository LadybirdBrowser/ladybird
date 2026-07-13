/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWeb/Bindings/WorkerLocation.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HTML/WorkerLocation.h>
#include <LibWeb/Infra/SerializedURL.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WorkerLocation);

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-href
Utf16String WorkerLocation::href() const
{
    // The href getter steps are to return this's WorkerGlobalScope object's url, serialized.
    return utf16_string_from_url_ascii(m_global_scope->url().serialize());
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-origin
Utf16String WorkerLocation::origin() const
{
    // The origin getter steps are to return the serialization of this's WorkerGlobalScope object's url's origin.
    return utf16_string_from_url_ascii(m_global_scope->url().origin().serialize());
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-protocol
Utf16String WorkerLocation::protocol() const
{
    // The protocol getter steps are to return this's WorkerGlobalScope object's url's scheme, followed by ":".
    return Utf16String::formatted("{}:", m_global_scope->url().scheme());
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-host
Utf16String WorkerLocation::host() const
{
    // The host getter steps are:
    // 1. Let url be this's WorkerGlobalScope object's url.
    auto const& url = m_global_scope->url();

    // 2. If url's host is null, return the empty string.
    if (!url.host().has_value())
        return {};

    // 3. If url's port is null, return url's host, serialized.
    if (!url.port().has_value())
        return utf16_string_from_url_ascii(url.serialized_host());

    // 4. Return url's host, serialized, followed by ":" and url's port, serialized.
    return utf16_string_from_url_ascii_host_and_port(url.serialized_host(), url.port().value());
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-hostname
Utf16String WorkerLocation::hostname() const
{
    // The hostname getter steps are:
    // 1. Let host be this's WorkerGlobalScope object's url's host.
    auto const& host = m_global_scope->url().host();

    // 2. If host is null, return the empty string.
    if (!host.has_value())
        return {};

    // 3. Return host, serialized.
    return utf16_string_from_url_ascii(host->serialize());
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-port
Utf16String WorkerLocation::port() const
{
    // The port getter steps are:
    // 1. Let port be this's WorkerGlobalScope object's url's port.
    auto const& port = m_global_scope->url().port();

    // 2. If port is null, return the empty string.
    if (!port.has_value())
        return {};

    // 3. Return port, serialized.
    return Utf16String::number(port.value());
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-pathname
Utf16String WorkerLocation::pathname() const
{
    // The pathname getter steps are to return the result of URL path serializing this's WorkerGlobalScope object's url.
    return utf16_string_from_url_ascii(m_global_scope->url().serialize_path());
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-search
Utf16String WorkerLocation::search() const
{
    // The search getter steps are:
    // 1. Let query be this's WorkerGlobalScope object's url's query.
    auto const& query = m_global_scope->url().query();

    // 2. If query is either null or the empty string, return the empty string.
    if (!query.has_value() || query->is_empty())
        return {};

    // 3. Return "?", followed by query.
    return utf16_string_from_url_ascii_with_prefix('?', *query);
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-hash
Utf16String WorkerLocation::hash() const
{
    // The hash getter steps are:
    // 1. Let fragment be this's WorkerGlobalScope object's url's fragment.
    auto const& fragment = m_global_scope->url().fragment();

    // 2. If fragment is either null or the empty string, return the empty string.
    if (!fragment.has_value() || fragment->is_empty())
        return {};

    // 3. Return "#", followed by fragment.
    return utf16_string_from_url_ascii_with_prefix('#', *fragment);
}

WorkerLocation::WorkerLocation(WorkerGlobalScope& global_scope)
    : PlatformObject(global_scope.realm())
    , m_global_scope(global_scope)
{
    // FIXME: Set prototype once we can get to worker scope prototypes.
}

WorkerLocation::~WorkerLocation() = default;

void WorkerLocation::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WorkerLocation);
    Base::initialize(realm);
}

void WorkerLocation::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_global_scope);
}

}
