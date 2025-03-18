/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Pattern/Canonicalization.h>
#include <LibURL/Pattern/ConstructorStringParser.h>
#include <LibURL/Pattern/Pattern.h>
#include <LibURL/URL.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#hostname-pattern-is-an-ipv6-address
static bool hostname_pattern_is_an_ipv6_address(String const& input)
{
    // 1. If input’s code point length is less than 2, then return false.
    if (input.bytes().size() < 2)
        return false;

    // 2. Let input code points be input interpreted as a list of code points.
    auto input_code_points = input.bytes();

    // 3. If input code points[0] is U+005B ([), then return true.
    if (input_code_points[0] == '[')
        return true;

    // 4. If input code points[0] is U+007B ({) and input code points[1] is U+005B ([), then return true.
    if (input_code_points[0] == '{' && input_code_points[1] == '[')
        return true;

    // 5. If input code points[0] is U+005C (\) and input code points[1] is U+005B ([), then return true.
    if (input_code_points[0] == '\\' && input_code_points[1] == '[')
        return true;

    // 6. Return false.
    return false;
}

// https://urlpattern.spec.whatwg.org/#url-pattern-create
PatternErrorOr<Pattern> Pattern::create(Input const& input, Optional<String> const& base_url, IgnoreCase ignore_case)
{
    // 1. Let init be null.
    Init init;

    // 2. If input is a scalar value string then:
    if (auto const* input_string = input.get_pointer<String>()) {
        // 1. Set init to the result of running parse a constructor string given input.
        init = TRY(ConstructorStringParser::parse(input_string->code_points()));

        // 2. If baseURL is null and init["protocol"] does not exist, then throw a TypeError.
        if (!base_url.has_value() && !init.protocol.has_value())
            return ErrorInfo { "Relative URLPattern constructor must provide one of baseURL or protocol"_string };

        // 3. If baseURL is not null, set init["baseURL"] to baseURL.
        if (base_url.has_value())
            init.base_url = base_url;
    }
    // 3. Otherwise:
    else {
        // 1. Assert: input is a URLPatternInit.
        VERIFY(input.has<Init>());

        // 2. If baseURL is not null, then throw a TypeError.
        if (base_url.has_value())
            return ErrorInfo { "Constructor with URLPatternInit should provide no baseURL"_string };

        // 3. Set init to input.
        init = input.get<Init>();
    }

    // 4. Let processedInit be the result of process a URLPatternInit given init, "pattern", null, null, null, null, null, null, null, and null.
    auto processed_init = TRY(process_a_url_pattern_init(init, PatternProcessType::Pattern, {}, {}, {}, {}, {}, {}, {}, {}));

    // 5. For each componentName of « "protocol", "username", "password", "hostname", "port", "pathname", "search", "hash" »:
    //     1. If processedInit[componentName] does not exist, then set processedInit[componentName] to "*".
    if (!processed_init.protocol.has_value())
        processed_init.protocol = "*"_string;
    if (!processed_init.username.has_value())
        processed_init.username = "*"_string;
    if (!processed_init.password.has_value())
        processed_init.password = "*"_string;
    if (!processed_init.hostname.has_value())
        processed_init.hostname = "*"_string;
    if (!processed_init.port.has_value())
        processed_init.port = "*"_string;
    if (!processed_init.pathname.has_value())
        processed_init.pathname = "*"_string;
    if (!processed_init.search.has_value())
        processed_init.search = "*"_string;
    if (!processed_init.hash.has_value())
        processed_init.hash = "*"_string;

    // 6. If processedInit["protocol"] is a special scheme and processedInit["port"] is a string which represents its
    //    corresponding default port in radix-10 using ASCII digits then set processedInit["port"] to the empty string.
    if (is_special_scheme(processed_init.protocol.value())) {
        auto maybe_port = processed_init.port->to_number<u16>();
        if (maybe_port.has_value() && *maybe_port == default_port_for_scheme(*processed_init.protocol).value())
            processed_init.port = String {};
    }

    // 7. Let urlPattern be a new URL pattern.
    Pattern url_pattern;

    // 8. Set urlPattern’s protocol component to the result of compiling a component given processedInit["protocol"],
    //    canonicalize a protocol, and default options.
    url_pattern.m_protocol_component = TRY(Component::compile(processed_init.protocol->code_points(), canonicalize_a_protocol, Options::default_()));

    // 9. Set urlPattern’s username component to the result of compiling a component given processedInit["username"],
    //    canonicalize a username, and default options.
    url_pattern.m_username_component = TRY(Component::compile(processed_init.username->code_points(), canonicalize_a_username, Options::default_()));

    // 10. Set urlPattern’s password component to the result of compiling a component given processedInit["password"],
    //     canonicalize a password, and default options.
    url_pattern.m_password_component = TRY(Component::compile(processed_init.password->code_points(), canonicalize_a_password, Options::default_()));

    // 11. If the result running hostname pattern is an IPv6 address given processedInit["hostname"] is true, then set
    //     urlPattern’s hostname component to the result of compiling a component given processedInit["hostname"],
    //     canonicalize an IPv6 hostname, and hostname options.
    if (hostname_pattern_is_an_ipv6_address(processed_init.hostname.value())) {
        url_pattern.m_hostname_component = TRY(Component::compile(processed_init.hostname->code_points(), canonicalize_an_ipv6_hostname, Options::hostname()));
    }
    // 12. Otherwise, set urlPattern’s hostname component to the result of compiling a component given
    //     processedInit["hostname"], canonicalize a hostname, and hostname options.
    else {
        url_pattern.m_hostname_component = TRY(Component::compile(processed_init.hostname->code_points(), canonicalize_a_hostname, Options::hostname()));
    }

    // 13. Set urlPattern’s port component to the result of compiling a component given processedInit["port"],
    //     canonicalize a port, and default options.
    url_pattern.m_port_component = TRY(Component::compile(processed_init.port->code_points(), [](String const& value) { return canonicalize_a_port(value); }, Options::default_()));

    // 14. Let compileOptions be a copy of the default options with the ignore case property set to options["ignoreCase"].
    auto compile_options = Options::default_();
    compile_options.ignore_case = ignore_case == IgnoreCase::Yes;

    // 15. If the result of running protocol component matches a special scheme given urlPattern’s protocol component is true, then:
    if (protocol_component_matches_a_special_scheme(url_pattern.m_protocol_component)) {
        // 1. Let pathCompileOptions be copy of the pathname options with the ignore case property set to options["ignoreCase"].
        auto path_compile_options = Options::pathname();
        path_compile_options.ignore_case = ignore_case == IgnoreCase::Yes;

        // 2. Set urlPattern’s pathname component to the result of compiling a component given processedInit["pathname"],
        //    canonicalize a pathname, and pathCompileOptions.
        url_pattern.m_pathname_component = TRY(Component::compile(processed_init.pathname->code_points(), canonicalize_a_pathname, path_compile_options));
    }
    // 16. Otherwise set urlPattern’s pathname component to the result of compiling a component given
    //     processedInit["pathname"], canonicalize an opaque pathname, and compileOptions.
    else {
        url_pattern.m_pathname_component = TRY(Component::compile(processed_init.pathname->code_points(), canonicalize_an_opaque_pathname, compile_options));
    }

    // 17. Set urlPattern’s search component to the result of compiling a component given processedInit["search"],
    //     canonicalize a search, and compileOptions.
    url_pattern.m_search_component = TRY(Component::compile(processed_init.search->code_points(), canonicalize_a_search, compile_options));

    // 18. Set urlPattern’s hash component to the result of compiling a component given processedInit["hash"],
    //     canonicalize a hash, and compileOptions.
    url_pattern.m_hash_component = TRY(Component::compile(processed_init.hash->code_points(), canonicalize_a_hash, compile_options));

    // 19. Return urlPattern.
    return url_pattern;
}

// https://urlpattern.spec.whatwg.org/#url-pattern-match
PatternErrorOr<Optional<Result>> Pattern::match(Input const&, Optional<String> const&) const
{
    dbgln("FIXME: Implement URL::Pattern::match");
    return OptionalNone {};
}

// https://urlpattern.spec.whatwg.org/#url-pattern-has-regexp-groups
bool Pattern::has_regexp_groups() const
{
    // 1. If urlPattern’s protocol component has regexp groups is true, then return true.
    if (m_protocol_component.has_regexp_groups)
        return true;

    // 2. If urlPattern’s username component has regexp groups is true, then return true.
    if (m_username_component.has_regexp_groups)
        return true;

    // 3. If urlPattern’s password component has regexp groups is true, then return true.
    if (m_password_component.has_regexp_groups)
        return true;

    // 4. If urlPattern’s hostname component has regexp groups is true, then return true.
    if (m_hostname_component.has_regexp_groups)
        return true;

    // 5. If urlPattern’s port component has regexp groups is true, then return true.
    if (m_port_component.has_regexp_groups)
        return true;

    // 6. If urlPattern’s pathname component has regexp groups is true, then return true.
    if (m_pathname_component.has_regexp_groups)
        return true;

    // 7. If urlPattern’s search component has regexp groups is true, then return true.
    if (m_search_component.has_regexp_groups)
        return true;

    // 8. If urlPattern’s hash component has regexp groups is true, then return true.
    if (m_hash_component.has_regexp_groups)
        return true;

    // 9. Return false.
    return false;
}

}
