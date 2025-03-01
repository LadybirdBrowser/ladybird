/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Pattern/Pattern.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#url-pattern-create
PatternErrorOr<Pattern> Pattern::create(Input const& input, Optional<String> const& base_url, Options const&)
{
    // 1. Let init be null.
    Init init;

    // 2. If input is a scalar value string then:
    if (input.has<String>()) {
        // FIXME: 1. Set init to the result of running parse a constructor string given input.

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

    // FIXME: 4. Let processedInit be the result of process a URLPatternInit given init, "pattern", null, null, null, null, null, null, null, and null.

    // FIXME: 5. For each componentName of « "protocol", "username", "password", "hostname", "port", "pathname", "search", "hash" »:
    //     FIXME: 1. If processedInit[componentName] does not exist, then set processedInit[componentName] to "*".

    // FIXME: 6. If processedInit["protocol"] is a special scheme and processedInit["port"] is a string which represents its
    //    corresponding default port in radix-10 using ASCII digits then set processedInit["port"] to the empty string.

    // 7. Let urlPattern be a new URL pattern.
    Pattern url_pattern;

    // FIXME: 8. Set urlPattern’s protocol component to the result of compiling a component given processedInit["protocol"],
    //    canonicalize a protocol, and default options.

    // FIXME: 9. Set urlPattern’s username component to the result of compiling a component given processedInit["username"],
    //    canonicalize a username, and default options.

    // FIXME: 10. Set urlPattern’s password component to the result of compiling a component given processedInit["password"],
    //     canonicalize a password, and default options.

    // FIXME: 11. If the result running hostname pattern is an IPv6 address given processedInit["hostname"] is true, then set
    //     urlPattern’s hostname component to the result of compiling a component given processedInit["hostname"],
    //     canonicalize an IPv6 hostname, and hostname options.

    // FIXME: 12. Otherwise, set urlPattern’s hostname component to the result of compiling a component given
    //     processedInit["hostname"], canonicalize a hostname, and hostname options.

    // FIXME: 13. Set urlPattern’s port component to the result of compiling a component given processedInit["port"],
    //     canonicalize a port, and default options.

    // FIXME: 14. Let compileOptions be a copy of the default options with the ignore case property set to options["ignoreCase"].

    // FIXME: 15. If the result of running protocol component matches a special scheme given urlPattern’s protocol component is true, then:
    if (false) {
        // FIXME: 1. Let pathCompileOptions be copy of the pathname options with the ignore case property set to options["ignoreCase"].

        // FIXME: 2. Set urlPattern’s pathname component to the result of compiling a component given processedInit["pathname"],
        //    canonicalize a pathname, and pathCompileOptions.
    }
    // FIXME: 16. Otherwise set urlPattern’s pathname component to the result of compiling a component given
    //     processedInit["pathname"], canonicalize an opaque pathname, and compileOptions.

    // FIXME: 17. Set urlPattern’s search component to the result of compiling a component given processedInit["search"],
    //     canonicalize a search, and compileOptions.

    // FIXME: 18. Set urlPattern’s hash component to the result of compiling a component given processedInit["hash"],
    //     canonicalize a hash, and compileOptions.

    // 19. Return urlPattern.
    return url_pattern;
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
