/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/URLPattern/URLPattern.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::URLPattern {

GC_DEFINE_ALLOCATOR(URLPattern);

static URL::RustIntegration::URLPattern::Init to_internal_url_pattern_init(Bindings::URLPatternInit const& init)
{
    return {
        .protocol = init.protocol.map([](Utf16String const& value) { return value.to_utf8_but_should_be_ported_to_utf16(); }),
        .username = init.username.map([](Utf16String const& value) { return value.to_utf8_but_should_be_ported_to_utf16(); }),
        .password = init.password.map([](Utf16String const& value) { return value.to_utf8_but_should_be_ported_to_utf16(); }),
        .hostname = init.hostname.map([](Utf16String const& value) { return value.to_utf8_but_should_be_ported_to_utf16(); }),
        .port = init.port.map([](Utf16String const& value) { return value.to_utf8_but_should_be_ported_to_utf16(); }),
        .pathname = init.pathname.map([](Utf16String const& value) { return value.to_utf8_but_should_be_ported_to_utf16(); }),
        .search = init.search.map([](Utf16String const& value) { return value.to_utf8_but_should_be_ported_to_utf16(); }),
        .hash = init.hash.map([](Utf16String const& value) { return value.to_utf8_but_should_be_ported_to_utf16(); }),
        .base_url = init.base_url.map([](Utf16String const& value) { return value.to_utf8_but_should_be_ported_to_utf16(); }),
    };
}

static Optional<String> to_internal_optional_string(Optional<Utf16String> const& value)
{
    return value.map([](Utf16String const& string) { return string.to_utf8_but_should_be_ported_to_utf16(); });
}

static URL::RustIntegration::URLPattern::Input to_internal_url_pattern_input(URLPatternInput const& input)
{
    return input.visit(
        [](Utf16String const& input_string) -> URL::RustIntegration::URLPattern::Input { return input_string.to_utf8_but_should_be_ported_to_utf16(); },
        [](Bindings::URLPatternInit const& input_init) -> URL::RustIntegration::URLPattern::Input { return to_internal_url_pattern_init(input_init); });
}

static URL::FFI::IgnoreCase to_internal_ignore_case(Bindings::URLPatternOptions const& options)
{
    return options.ignore_case ? URL::FFI::IgnoreCase::Yes : URL::FFI::IgnoreCase::No;
}

static Bindings::URLPatternInit to_bindings_url_pattern_init(URL::RustIntegration::URLPattern::Init const& init)
{
    return {
        .base_url = init.base_url.map([](String const& value) { return Utf16String::from_utf8(value); }),
        .hash = init.hash.map([](String const& value) { return Utf16String::from_utf8(value); }),
        .hostname = init.hostname.map([](String const& value) { return Utf16String::from_utf8(value); }),
        .password = init.password.map([](String const& value) { return Utf16String::from_utf8(value); }),
        .pathname = init.pathname.map([](String const& value) { return Utf16String::from_utf8(value); }),
        .port = init.port.map([](String const& value) { return Utf16String::from_utf8(value); }),
        .protocol = init.protocol.map([](String const& value) { return Utf16String::from_utf8(value); }),
        .search = init.search.map([](String const& value) { return Utf16String::from_utf8(value); }),
        .username = init.username.map([](String const& value) { return Utf16String::from_utf8(value); }),
    };
}

static Bindings::URLPatternComponentResult to_bindings_url_pattern_component_result(URL::RustIntegration::URLPattern::Component::Result const& result)
{
    OrderedHashMap<Utf16String, Variant<Utf16String, Empty, Empty>> groups;
    for (auto const& [key, value] : result.groups) {
        groups.set(Utf16String::from_utf8(key), value.visit([](String const& string_value) -> Variant<Utf16String, Empty, Empty> { return Utf16String::from_utf8(string_value); }, [](Empty) -> Variant<Utf16String, Empty, Empty> { return Empty {}; }));
    }

    return {
        .groups = Optional<OrderedHashMap<Utf16String, Variant<Utf16String, Empty, Empty>>> { move(groups) },
        .input = Optional<Utf16String> { Utf16String::from_utf8(result.input) },
    };
}

static Bindings::URLPatternResult to_bindings_url_pattern_result(URL::RustIntegration::URLPattern::Result const& result)
{
    Vector<URLPatternInput> inputs;
    inputs.ensure_capacity(result.inputs.size());
    for (auto const& input : result.inputs) {
        inputs.unchecked_append(input.visit(
            [](String const& string_value) -> URLPatternInput { return Utf16String::from_utf8(string_value); },
            [](URL::RustIntegration::URLPattern::Init const& init_value) -> URLPatternInput { return to_bindings_url_pattern_init(init_value); }));
    }

    return {
        .hash = Optional<Bindings::URLPatternComponentResult> { to_bindings_url_pattern_component_result(result.hash) },
        .hostname = Optional<Bindings::URLPatternComponentResult> { to_bindings_url_pattern_component_result(result.hostname) },
        .inputs = Optional<Vector<URLPatternInput>> { move(inputs) },
        .password = Optional<Bindings::URLPatternComponentResult> { to_bindings_url_pattern_component_result(result.password) },
        .pathname = Optional<Bindings::URLPatternComponentResult> { to_bindings_url_pattern_component_result(result.pathname) },
        .port = Optional<Bindings::URLPatternComponentResult> { to_bindings_url_pattern_component_result(result.port) },
        .protocol = Optional<Bindings::URLPatternComponentResult> { to_bindings_url_pattern_component_result(result.protocol) },
        .search = Optional<Bindings::URLPatternComponentResult> { to_bindings_url_pattern_component_result(result.search) },
        .username = Optional<Bindings::URLPatternComponentResult> { to_bindings_url_pattern_component_result(result.username) },
    };
}

static WebIDL::ExceptionOr<GC::Ref<URLPattern>> create_url_pattern(URLPatternInput const& input, Optional<Utf16String> const& base_url, Bindings::URLPatternOptions const& options)
{
    // 1. Set this’s associated URL pattern to the result of create given input, baseURL, and options.
    auto pattern_or_error = URL::RustIntegration::URLPattern::create(to_internal_url_pattern_input(input), to_internal_optional_string(base_url), to_internal_ignore_case(options));
    if (pattern_or_error.is_error())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::from_utf8(pattern_or_error.error().message) };
    return GC::Heap::the().allocate<URLPattern>(pattern_or_error.release_value());
}

URLPattern::URLPattern(URL::RustIntegration::URLPattern pattern)
    : m_url_pattern(move(pattern))
{
}

URLPattern::~URLPattern() = default;

// https://urlpattern.spec.whatwg.org/#urlpattern-initialize
WebIDL::ExceptionOr<GC::Ref<URLPattern>> URLPattern::create(URLPatternInput const& input, Utf16String const& base_url, Bindings::URLPatternOptions const& options)
{
    return create_url_pattern(input, Optional<Utf16String> { base_url }, options);
}

// https://urlpattern.spec.whatwg.org/#urlpattern-initialize
WebIDL::ExceptionOr<GC::Ref<URLPattern>> URLPattern::create(URLPatternInput const& input, Bindings::URLPatternOptions const& options)
{
    return create_url_pattern(input, Optional<Utf16String> {}, options);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-test
WebIDL::ExceptionOr<bool> URLPattern::test(URLPatternInput const& input, Optional<Utf16String> const& base_url) const
{
    // 1. Let result be the result of match given this's associated URL pattern, input, and baseURL if given.
    auto result_or_error = m_url_pattern.match(to_internal_url_pattern_input(input), to_internal_optional_string(base_url));
    if (result_or_error.is_error())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::from_utf8(result_or_error.error().message) };
    auto result = result_or_error.release_value();

    // 2. If result is null, return false.
    if (!result.has_value())
        return false;

    // 3. Return true.
    return true;
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-exec
WebIDL::ExceptionOr<Optional<Bindings::URLPatternResult>> URLPattern::exec(URLPatternInput const& input, Optional<Utf16String> const& base_url) const
{
    // 1. Return the result of match given this's associated URL pattern, input, and baseURL if given.
    auto result_or_error = m_url_pattern.match(to_internal_url_pattern_input(input), to_internal_optional_string(base_url));
    if (result_or_error.is_error())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::from_utf8(result_or_error.error().message) };
    auto result = result_or_error.release_value();
    if (!result.has_value())
        return Optional<Bindings::URLPatternResult> {};
    return to_bindings_url_pattern_result(*result);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-protocol
Utf16String URLPattern::protocol() const
{
    // 1. Return this's associated URL pattern's protocol component's pattern string.
    return Utf16String::from_utf8(m_url_pattern.protocol_component().pattern_string);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-username
Utf16String URLPattern::username() const
{
    // 1. Return this's associated URL pattern's username component's pattern string.
    return Utf16String::from_utf8(m_url_pattern.username_component().pattern_string);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-password
Utf16String URLPattern::password() const
{
    // 1. Return this's associated URL pattern's password component's pattern string.
    return Utf16String::from_utf8(m_url_pattern.password_component().pattern_string);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-hostname
Utf16String URLPattern::hostname() const
{
    // 1. Return this's associated URL pattern's hostname component's pattern string.
    return Utf16String::from_utf8(m_url_pattern.hostname_component().pattern_string);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-port
Utf16String URLPattern::port() const
{
    // 1. Return this's associated URL pattern's port component's pattern string.
    return Utf16String::from_utf8(m_url_pattern.port_component().pattern_string);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-pathname
Utf16String URLPattern::pathname() const
{
    // 1. Return this's associated URL pattern's pathname component's pattern string.
    return Utf16String::from_utf8(m_url_pattern.pathname_component().pattern_string);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-search
Utf16String URLPattern::search() const
{
    // 1. Return this's associated URL pattern's search component's pattern string.
    return Utf16String::from_utf8(m_url_pattern.search_component().pattern_string);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-hash
Utf16String URLPattern::hash() const
{
    // 1. Return this's associated URL pattern's hash component's pattern string.
    return Utf16String::from_utf8(m_url_pattern.hash_component().pattern_string);
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
