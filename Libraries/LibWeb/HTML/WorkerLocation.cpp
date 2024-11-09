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

JS_DEFINE_ALLOCATOR(WorkerLocation);

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-href
WebIDL::ExceptionOr<String> WorkerLocation::href() const
{
    auto& vm = realm().vm();
    // The href getter steps are to return this's WorkerGlobalScope object's url, serialized.
    return TRY_OR_THROW_OOM(vm, String::from_byte_string(m_global_scope->url().serialize()));
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-origin
WebIDL::ExceptionOr<String> WorkerLocation::origin() const
{
    auto& vm = realm().vm();
    // The origin getter steps are to return the serialization of this's WorkerGlobalScope object's url's origin.
    return TRY_OR_THROW_OOM(vm, String::from_byte_string(m_global_scope->url().origin().serialize()));
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-protocol
WebIDL::ExceptionOr<String> WorkerLocation::protocol() const
{
    auto& vm = realm().vm();
    // The protocol getter steps are to return this's WorkerGlobalScope object's url's scheme, followed by ":".
    return TRY_OR_THROW_OOM(vm, String::formatted("{}:", m_global_scope->url().scheme()));
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-host
WebIDL::ExceptionOr<String> WorkerLocation::host() const
{
    auto& vm = realm().vm();

    // The host getter steps are:
    // 1. Let url be this's WorkerGlobalScope object's url.
    auto const& url = m_global_scope->url();

    // 2. If url's host is null, return the empty string.
    if (url.host().has<Empty>())
        return String {};

    // 3. If url's port is null, return url's host, serialized.
    if (!url.port().has_value())
        return TRY_OR_THROW_OOM(vm, url.serialized_host());

    // 4. Return url's host, serialized, followed by ":" and url's port, serialized.
    return TRY_OR_THROW_OOM(vm, String::formatted("{}:{}", TRY_OR_THROW_OOM(vm, url.serialized_host()), url.port().value()));
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-hostname
WebIDL::ExceptionOr<String> WorkerLocation::hostname() const
{
    auto& vm = realm().vm();

    // The hostname getter steps are:
    // 1. Let host be this's WorkerGlobalScope object's url's host.
    auto const& host = m_global_scope->url().host();

    // 2. If host is null, return the empty string.
    if (host.has<Empty>())
        return String {};

    // 3. Return host, serialized.
    return TRY_OR_THROW_OOM(vm, URL::Parser::serialize_host(host));
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-port
WebIDL::ExceptionOr<String> WorkerLocation::port() const
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
WebIDL::ExceptionOr<String> WorkerLocation::search() const
{
    auto& vm = realm().vm();

    // The search getter steps are:
    // 1. Let query be this's WorkerGlobalScope object's url's query.
    auto const& query = m_global_scope->url().query();

    // 2. If query is either null or the empty string, return the empty string.
    if (!query.has_value() || query->is_empty())
        return String {};

    // 3. Return "?", followed by query.
    return TRY_OR_THROW_OOM(vm, String::formatted("?{}", *query));
}

// https://html.spec.whatwg.org/multipage/workers.html#dom-workerlocation-hash
WebIDL::ExceptionOr<String> WorkerLocation::hash() const
{
    auto& vm = realm().vm();

    // The hash getter steps are:
    // 1. Let fragment be this's WorkerGlobalScope object's url's fragment.
    auto const& fragment = m_global_scope->url().fragment();

    // 2. If fragment is either null or the empty string, return the empty string.
    if (!fragment.has_value() || fragment->is_empty())
        return String {};

    // 3. Return "#", followed by fragment.
    return TRY_OR_THROW_OOM(vm, String::formatted("#{}", *fragment));
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
