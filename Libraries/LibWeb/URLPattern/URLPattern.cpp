/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/URLPattern.h>
#include <LibWeb/URLPattern/URLPattern.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::URLPattern {

GC_DEFINE_ALLOCATOR(URLPattern);

static WebIDL::ExceptionOr<Optional<String>> to_internal_optional_string(JS::VM& vm, Optional<Utf16String> const& string)
{
    if (!string.has_value())
        return Optional<String> {};
    return TRY_OR_THROW_OOM(vm, string->utf16_view().to_utf8());
}

static Utf16String to_binding_string(String const& string)
{
    return Utf16String::from_utf8(string);
}

static Optional<Utf16String> to_binding_optional_string(Optional<String> const& string)
{
    if (!string.has_value())
        return {};
    return to_binding_string(*string);
}

static WebIDL::ExceptionOr<URL::RustIntegration::URLPattern::Init> to_internal_url_pattern_init(JS::VM& vm, Bindings::URLPatternInit const& init)
{
    URL::RustIntegration::URLPattern::Init internal_init;
    internal_init.protocol = TRY(to_internal_optional_string(vm, init.protocol));
    internal_init.username = TRY(to_internal_optional_string(vm, init.username));
    internal_init.password = TRY(to_internal_optional_string(vm, init.password));
    internal_init.hostname = TRY(to_internal_optional_string(vm, init.hostname));
    internal_init.port = TRY(to_internal_optional_string(vm, init.port));
    internal_init.pathname = TRY(to_internal_optional_string(vm, init.pathname));
    internal_init.search = TRY(to_internal_optional_string(vm, init.search));
    internal_init.hash = TRY(to_internal_optional_string(vm, init.hash));
    internal_init.base_url = TRY(to_internal_optional_string(vm, init.base_url));
    return internal_init;
}

static Bindings::URLPatternInit to_bindings_url_pattern_init(URL::RustIntegration::URLPattern::Init const& init)
{
    return {
        .base_url = to_binding_optional_string(init.base_url),
        .hash = to_binding_optional_string(init.hash),
        .hostname = to_binding_optional_string(init.hostname),
        .password = to_binding_optional_string(init.password),
        .pathname = to_binding_optional_string(init.pathname),
        .port = to_binding_optional_string(init.port),
        .protocol = to_binding_optional_string(init.protocol),
        .search = to_binding_optional_string(init.search),
        .username = to_binding_optional_string(init.username),
    };
}

static WebIDL::ExceptionOr<URL::RustIntegration::URLPattern::Input> to_internal_url_pattern_input(JS::VM& vm, URLPatternInput const& input)
{
    return input.visit(
        [&](Utf16String const& input_string) -> WebIDL::ExceptionOr<URL::RustIntegration::URLPattern::Input> {
            return TRY_OR_THROW_OOM(vm, input_string.utf16_view().to_utf8());
        },
        [&](Bindings::URLPatternInit const& input_init) -> WebIDL::ExceptionOr<URL::RustIntegration::URLPattern::Input> {
            return TRY(to_internal_url_pattern_init(vm, input_init));
        });
}

static Bindings::URLPatternComponentResult to_bindings_url_pattern_component_result(URL::RustIntegration::URLPattern::Component::Result const& result)
{
    OrderedHashMap<Utf16String, Variant<Utf16String, Empty, Empty>> groups;
    for (auto const& [key, value] : result.groups) {
        groups.set(to_binding_string(key), value.visit([](String const& string_value) -> Variant<Utf16String, Empty, Empty> { return to_binding_string(string_value); }, [](Empty) -> Variant<Utf16String, Empty, Empty> { return Empty {}; }));
    }

    return {
        .groups = move(groups),
        .input = to_binding_string(result.input),
    };
}

static Bindings::URLPatternResult to_bindings_url_pattern_result(URL::RustIntegration::URLPattern::Result const& result)
{
    Vector<Variant<Utf16String, Bindings::URLPatternInit>> inputs;
    inputs.ensure_capacity(result.inputs.size());
    for (auto const& input : result.inputs) {
        inputs.unchecked_append(input.visit(
            [](String const& string_value) -> Variant<Utf16String, Bindings::URLPatternInit> { return to_binding_string(string_value); },
            [](URL::RustIntegration::URLPattern::Init const& init_value) -> Variant<Utf16String, Bindings::URLPatternInit> { return to_bindings_url_pattern_init(init_value); }));
    }

    return {
        .hash = to_bindings_url_pattern_component_result(result.hash),
        .hostname = to_bindings_url_pattern_component_result(result.hostname),
        .inputs = move(inputs),
        .password = to_bindings_url_pattern_component_result(result.password),
        .pathname = to_bindings_url_pattern_component_result(result.pathname),
        .port = to_bindings_url_pattern_component_result(result.port),
        .protocol = to_bindings_url_pattern_component_result(result.protocol),
        .search = to_bindings_url_pattern_component_result(result.search),
        .username = to_bindings_url_pattern_component_result(result.username),
    };
}

URLPattern::URLPattern(JS::Realm& realm, URL::RustIntegration::URLPattern pattern)
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
WebIDL::ExceptionOr<GC::Ref<URLPattern>> URLPattern::construct_impl(JS::Realm& realm, URLPatternInput const& input, Utf16String const& base_url, URLPatternOptions const& options)
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
WebIDL::ExceptionOr<GC::Ref<URLPattern>> URLPattern::create(JS::Realm& realm, URLPatternInput const& input, Optional<Utf16String> const& base_url, URLPatternOptions const& options)
{
    // 1. Set this’s associated URL pattern to the result of create given input, baseURL, and options.
    auto internal_input = TRY(to_internal_url_pattern_input(realm.vm(), input));
    auto internal_base_url = TRY(to_internal_optional_string(realm.vm(), base_url));
    auto pattern_or_error = URL::RustIntegration::URLPattern::create(internal_input, internal_base_url, options.ignore_case ? URL::FFI::IgnoreCase::Yes : URL::FFI::IgnoreCase::No);
    if (pattern_or_error.is_error())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::from_utf8(pattern_or_error.error().message) };
    return realm.create<URLPattern>(realm, pattern_or_error.release_value());
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-test
WebIDL::ExceptionOr<bool> URLPattern::test(URLPatternInput const& input, Optional<Utf16String> const& base_url) const
{
    // 1. Let result be the result of match given this's associated URL pattern, input, and baseURL if given.
    auto internal_input = TRY(to_internal_url_pattern_input(vm(), input));
    auto internal_base_url = TRY(to_internal_optional_string(vm(), base_url));
    auto result_or_error = m_url_pattern.match(internal_input, internal_base_url);
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
WebIDL::ExceptionOr<Optional<URLPatternResult>> URLPattern::exec(URLPatternInput const& input, Optional<Utf16String> const& base_url) const
{
    // 1. Return the result of match given this's associated URL pattern, input, and baseURL if given.
    auto internal_input = TRY(to_internal_url_pattern_input(vm(), input));
    auto internal_base_url = TRY(to_internal_optional_string(vm(), base_url));
    auto result_or_error = m_url_pattern.match(internal_input, internal_base_url);
    if (result_or_error.is_error())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, Utf16String::from_utf8(result_or_error.error().message) };
    auto result = result_or_error.release_value();
    if (!result.has_value())
        return Optional<URLPatternResult> {};
    return to_bindings_url_pattern_result(*result);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-protocol
Utf16String URLPattern::protocol() const
{
    // 1. Return this's associated URL pattern's protocol component's pattern string.
    return to_binding_string(m_url_pattern.protocol_component().pattern_string);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-username
Utf16String URLPattern::username() const
{
    // 1. Return this's associated URL pattern's username component's pattern string.
    return to_binding_string(m_url_pattern.username_component().pattern_string);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-password
Utf16String URLPattern::password() const
{
    // 1. Return this's associated URL pattern's password component's pattern string.
    return to_binding_string(m_url_pattern.password_component().pattern_string);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-hostname
Utf16String URLPattern::hostname() const
{
    // 1. Return this's associated URL pattern's hostname component's pattern string.
    return to_binding_string(m_url_pattern.hostname_component().pattern_string);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-port
Utf16String URLPattern::port() const
{
    // 1. Return this's associated URL pattern's port component's pattern string.
    return to_binding_string(m_url_pattern.port_component().pattern_string);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-pathname
Utf16String URLPattern::pathname() const
{
    // 1. Return this's associated URL pattern's pathname component's pattern string.
    return to_binding_string(m_url_pattern.pathname_component().pattern_string);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-search
Utf16String URLPattern::search() const
{
    // 1. Return this's associated URL pattern's search component's pattern string.
    return to_binding_string(m_url_pattern.search_component().pattern_string);
}

// https://urlpattern.spec.whatwg.org/#dom-urlpattern-hash
Utf16String URLPattern::hash() const
{
    // 1. Return this's associated URL pattern's hash component's pattern string.
    return to_binding_string(m_url_pattern.hash_component().pattern_string);
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
