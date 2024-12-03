/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWeb/Bindings/WorkerLocationPrototype.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HTML/WorkerLocation.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WorkerLocation);

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-href
String WorkerLocation::href() const
{
    // The href getter steps are to return this's WorkerGlobalScope object's url, serialized.
    return m_global_scope->url().serialize();
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-origin
String WorkerLocation::origin() const
{
    // The origin getter steps are to return the serialization of this's WorkerGlobalScope object's url's origin.
    return m_global_scope->url().origin().serialize();
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-protocol
String WorkerLocation::protocol() const
{
    // The protocol getter steps are to return this's WorkerGlobalScope object's url's scheme, followed by ":".
    return MUST(String::formatted("{}:", m_global_scope->url().scheme()));
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-host
String WorkerLocation::host() const
{
    // The host getter steps are:
    // 1. Let url be this's WorkerGlobalScope object's url.
    auto const& url = m_global_scope->url();

    // 2. If url's host is null, return the empty string.
    if (!url.host().has_value())
        return String {};

    // 3. If url's port is null, return url's host, serialized.
    if (!url.port().has_value())
        return url.serialized_host();

    // 4. Return url's host, serialized, followed by ":" and url's port, serialized.
    return MUST(String::formatted("{}:{}", url.serialized_host(), url.port().value()));
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-hostname
String WorkerLocation::hostname() const
{
    // The hostname getter steps are:
    // 1. Let host be this's WorkerGlobalScope object's url's host.
    auto const& host = m_global_scope->url().host();

    // 2. If host is null, return the empty string.
    if (!host.has_value())
        return String {};

    // 3. Return host, serialized.
    return host->serialize();
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-port
String WorkerLocation::port() const
{
    // The port getter steps are:
    // 1. Let port be this's WorkerGlobalScope object's url's port.
    auto const& port = m_global_scope->url().port();

    // 2. If port is null, return the empty string.
    if (!port.has_value())
        return String {};

    // 3. Return port, serialized.
    return String::number(port.value());
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-pathname
String WorkerLocation::pathname() const
{
    // The pathname getter steps are to return the result of URL path serializing this's WorkerGlobalScope object's url.
    return m_global_scope->url().serialize_path();
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-search
String WorkerLocation::search() const
{
    // The search getter steps are:
    // 1. Let query be this's WorkerGlobalScope object's url's query.
    auto const& query = m_global_scope->url().query();

    // 2. If query is either null or the empty string, return the empty string.
    if (!query.has_value() || query->is_empty())
        return String {};

    // 3. Return "?", followed by query.
    return MUST(String::formatted("?{}", *query));
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-hash
String WorkerLocation::hash() const
{
    // The hash getter steps are:
    // 1. Let fragment be this's WorkerGlobalScope object's url's fragment.
    auto const& fragment = m_global_scope->url().fragment();

    // 2. If fragment is either null or the empty string, return the empty string.
    if (!fragment.has_value() || fragment->is_empty())
        return String {};

    // 3. Return "#", followed by fragment.
    return MUST(String::formatted("#{}", *fragment));
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
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WorkerLocation);
}

void WorkerLocation::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_global_scope);
}

}
