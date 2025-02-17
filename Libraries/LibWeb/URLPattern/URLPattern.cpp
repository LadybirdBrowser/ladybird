/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/URLPatternPrototype.h>
#include <LibWeb/URLPattern/URLPattern.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::URLPattern {

GC_DEFINE_ALLOCATOR(URLPattern);

URLPattern::URLPattern(JS::Realm& realm)
    : PlatformObject(realm)
{
}

URLPattern::~URLPattern() = default;

void URLPattern::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(URLPattern);
}

WebIDL::ExceptionOr<GC::Ref<URLPattern>> URLPattern::construct_impl(JS::Realm& realm, URLPatternInput const&, String const&, URLPatternOptions const&)
{
    return realm.create<URLPattern>(realm);
}

WebIDL::ExceptionOr<GC::Ref<URLPattern>> URLPattern::construct_impl(JS::Realm& realm, URLPatternInput const&, URLPatternOptions const&)
{
    return realm.create<URLPattern>(realm);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-exec
Optional<URLPatternResult> URLPattern::exec(URLPatternInput const&, Optional<String> const&) const
{
    dbgln("FIXME: Implement URLPattern::match");
    return {};
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-protocol
String const& URLPattern::protocol() const
{
    // 1. Return this's associated URL pattern's protocol component's pattern string.
    return m_url_pattern.protocol_component().pattern_string;
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-username
String const& URLPattern::username() const
{
    // 1. Return this's associated URL pattern's username component's pattern string.
    return m_url_pattern.username_component().pattern_string;
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-password
String const& URLPattern::password() const
{
    // 1. Return this's associated URL pattern's password component's pattern string.
    return m_url_pattern.password_component().pattern_string;
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-hostname
String const& URLPattern::hostname() const
{
    // 1. Return this's associated URL pattern's hostname component's pattern string.
    return m_url_pattern.hostname_component().pattern_string;
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-port
String const& URLPattern::port() const
{
    // 1. Return this's associated URL pattern's port component's pattern string.
    return m_url_pattern.port_component().pattern_string;
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-pathname
String const& URLPattern::pathname() const
{
    // 1. Return this's associated URL pattern's pathname component's pattern string.
    return m_url_pattern.pathname_component().pattern_string;
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-search
String const& URLPattern::search() const
{
    // 1. Return this's associated URL pattern's search component's pattern string.
    return m_url_pattern.search_component().pattern_string;
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-hash
String const& URLPattern::hash() const
{
    // 1. Return this's associated URL pattern's hash component's pattern string.
    return m_url_pattern.hash_component().pattern_string;
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-hasregexpgroups
bool URLPattern::has_reg_exp_groups() const
{
    // 1. If this's associated URL pattern's has regexp groups, then return true.
    if (m_url_pattern.has_regexp_groups())
        return true;

    // 2. Return false.
    return false;
}

}
