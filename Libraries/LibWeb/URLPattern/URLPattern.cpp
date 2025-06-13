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

URLPattern::URLPattern(JS::Realm& realm, URL::Pattern::Pattern pattern)
    : PlatformObject(realm)
    , m_url_pattern(move(pattern))
{
}

URLPattern::~URLPattern() = default;

void URLPattern::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(URLPattern);
    Base::initialize(realm);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-urlpattern
WebIDL::ExceptionOr<GC::Ref<URLPattern>> URLPattern::construct_impl(JS::Realm& realm, URLPatternInput const& input, String const& base_url, URLPatternOptions const& options)
{
    // 1. Run initialize given this, input, baseURL, and options.
    return create(realm, input, base_url, options);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-urlpattern-input-options
WebIDL::ExceptionOr<GC::Ref<URLPattern>> URLPattern::construct_impl(JS::Realm& realm, URLPatternInput const& input, URLPatternOptions const& options)
{
    // 1. Run initialize given this, input, null, and options.
    return create(realm, input, {}, options);
}

// https://urlpattern.spec.whatwg.org/#urlpattern-initialize
WebIDL::ExceptionOr<GC::Ref<URLPattern>> URLPattern::create(JS::Realm& realm, URLPatternInput const& input, Optional<String> const& base_url, URLPatternOptions const& options)
{
    // 1. Set this’s associated URL pattern to the result of create given input, baseURL, and options.
    auto pattern_or_error = URL::Pattern::Pattern::create(input, base_url, options.ignore_case ? URL::Pattern::IgnoreCase::Yes : URL::Pattern::IgnoreCase::No);
    if (pattern_or_error.is_error())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, pattern_or_error.error().message };
    return realm.create<URLPattern>(realm, pattern_or_error.release_value());
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-test
WebIDL::ExceptionOr<bool> URLPattern::test(URLPatternInput const& input, Optional<String> const& base_url) const
{
    // 1. Let result be the result of match given this's associated URL pattern, input, and baseURL if given.
    auto result_or_error = m_url_pattern.match(input, base_url);
    if (result_or_error.is_error())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, result_or_error.error().message };
    auto result = result_or_error.release_value();

    // 2. If result is null, return false.
    if (!result.has_value())
        return false;

    // 3. Return true.
    return true;
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-exec
WebIDL::ExceptionOr<Optional<URLPatternResult>> URLPattern::exec(URLPatternInput const& input, Optional<String> const& base_url) const
{
    // 1. Return the result of match given this's associated URL pattern, input, and baseURL if given.
    auto result_or_error = m_url_pattern.match(input, base_url);
    if (result_or_error.is_error())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, result_or_error.error().message };
    return result_or_error.release_value();
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
