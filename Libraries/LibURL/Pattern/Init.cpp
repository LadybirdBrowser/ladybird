/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibURL/Pattern/Canonicalization.h>
#include <LibURL/Pattern/Init.h>
#include <LibURL/Pattern/String.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#process-a-base-url-string
static String process_a_base_url_string(String const& input, PatternProcessType type)
{
    // 1. Assert: input is not null.
    // 2. If type is not "pattern" return input.
    if (type != PatternProcessType::Pattern)
        return input;

    // 3. Return the result of escaping a pattern string given input.
    return escape_a_pattern_string(input);
}

// https://urlpattern.spec.whatwg.org/#is-an-absolute-pathname
static bool is_an_absolute_pathname(String const& input, PatternProcessType type)
{
    // 1. If input is the empty string, then return false.
    if (input.is_empty())
        return false;

    // 2. If input[0] is U+002F (/), then return true.
    if (input.bytes()[0] == '/')
        return true;

    // 3. If type is "url", then return false.
    if (type == PatternProcessType::URL)
        return false;

    // 4. If input’s code point length is less than 2, then return false.
    if (input.bytes().size() < 2)
        return false;

    // 5. If input[0] is U+005C (\) and input[1] is U+002F (/), then return true.
    if (input.bytes()[0] == '\\' && input.bytes()[1] == '/')
        return true;

    // 6. If input[0] is U+007B ({) and input[1] is U+002F (/), then return true.
    if (input.bytes()[0] == '{' && input.bytes()[1] == '/')
        return true;

    // 7. Return false.
    return false;
}

// https://urlpattern.spec.whatwg.org/#process-protocol-for-init
static PatternErrorOr<String> process_protocol_for_init(String const& value, PatternProcessType type)
{
    // 1. Let strippedValue be the given value with a single trailing U+003A (:) removed, if any.
    auto stripped_value = value;
    if (stripped_value.ends_with(':'))
        stripped_value = MUST(stripped_value.substring_from_byte_offset(0, stripped_value.bytes().size() - 1));

    // 2. If type is "pattern" then return strippedValue.
    if (type == PatternProcessType::Pattern)
        return stripped_value;

    // 3. Return the result of running canonicalize a protocol given strippedValue.
    return canonicalize_a_protocol(stripped_value);
}

// https://urlpattern.spec.whatwg.org/#process-username-for-init
static String process_username_for_init(String const& value, PatternProcessType type)
{
    // 1. If type is "pattern" then return value.
    if (type == PatternProcessType::Pattern)
        return value;

    // 2. Return the result of running canonicalize a username given value.
    return canonicalize_a_username(value);
}

// https://urlpattern.spec.whatwg.org/#process-password-for-init
static String process_password_for_init(String const& value, PatternProcessType type)
{
    // 1. If type is "pattern" then return value.
    if (type == PatternProcessType::Pattern)
        return value;

    // 2. Return the result of running canonicalize a password given value.
    return canonicalize_a_password(value);
}

// https://urlpattern.spec.whatwg.org/#process-hostname-for-init
static PatternErrorOr<String> process_hostname_for_init(String const& value, PatternProcessType type)
{
    // 1. If type is "pattern" then return value.
    if (type == PatternProcessType::Pattern)
        return value;

    // 2. Return the result of running canonicalize a hostname given value.
    return canonicalize_a_hostname(value);
}

// https://urlpattern.spec.whatwg.org/#process-port-for-init
static PatternErrorOr<String> process_port_for_init(String const& port_value, String const& protocol_value, PatternProcessType type)
{
    // 1. If type is "pattern" then return portValue.
    if (type == PatternProcessType::Pattern)
        return port_value;

    // 2. Return the result of running canonicalize a port given portValue and protocolValue.
    return canonicalize_a_port(port_value, protocol_value);
}

// https://urlpattern.spec.whatwg.org/#process-pathname-for-init
static PatternErrorOr<String> process_pathname_for_init(String const& pathname_value, String const& protocol_value, PatternProcessType type)
{
    // 1. If type is "pattern" then return pathnameValue.
    if (type == PatternProcessType::Pattern)
        return pathname_value;

    // 2. If protocolValue is a special scheme or the empty string, then return the result of running canonicalize a
    //    pathname given pathnameValue.
    // NOTE: If the protocolValue is the empty string then no value was provided for protocol in the constructor
    //       dictionary. Normally we do not special case empty string dictionary values, but in this case we treat
    //       it as a special scheme in order to default to the most common pathname canonicalization.
    if (protocol_value.is_empty() || is_special_scheme(protocol_value))
        return canonicalize_a_pathname(pathname_value);

    // 3. Return the result of running canonicalize an opaque pathname given pathnameValue.
    return canonicalize_an_opaque_pathname(pathname_value);
}

// https://urlpattern.spec.whatwg.org/#process-search-for-init
static String process_search_for_init(String const& value, PatternProcessType type)
{
    // 1. Let strippedValue be the given value with a single leading U+003F (?) removed, if any.
    auto stripped_value = value;
    if (stripped_value.starts_with('?'))
        stripped_value = MUST(stripped_value.substring_from_byte_offset(1));

    // 2. If type is "pattern" then return strippedValue.
    if (type == PatternProcessType::Pattern)
        return stripped_value;

    // 3. Return the result of running canonicalize a search given strippedValue.
    return canonicalize_a_search(stripped_value);
}

// https://urlpattern.spec.whatwg.org/#process-hash-for-init
static String process_hash_for_init(String const& value, PatternProcessType type)
{
    // 1. Let strippedValue be the given value with a single leading U+0023 (#) removed, if any.
    auto stripped_value = value;
    if (stripped_value.starts_with('#'))
        stripped_value = MUST(stripped_value.substring_from_byte_offset(1));

    // 2. If type is "pattern" then return strippedValue.
    if (type == PatternProcessType::Pattern)
        return stripped_value;

    // 3. Return the result of running canonicalize a hash given strippedValue.
    return canonicalize_a_hash(stripped_value);
}

// https://urlpattern.spec.whatwg.org/#process-a-urlpatterninit
PatternErrorOr<Init> process_a_url_pattern_init(Init const& init, PatternProcessType type,
    Optional<String> const& protocol, Optional<String> const& username, Optional<String> const& password,
    Optional<String> const& hostname, Optional<String> const& port, Optional<String> const& pathname,
    Optional<String> const& search, Optional<String> const& hash)
{
    // 1. Let result be the result of creating a new URLPatternInit.
    Init result;

    // 2. If protocol is not null, set result["protocol"] to protocol.
    if (protocol.has_value())
        result.protocol = protocol;

    // 3. If username is not null, set result["username"] to username.
    if (username.has_value())
        result.username = username;

    // 4. If password is not null, set result["password"] to password.
    if (password.has_value())
        result.password = password;

    // 5. If hostname is not null, set result["hostname"] to hostname.
    if (hostname.has_value())
        result.hostname = hostname;

    // 6. If port is not null, set result["port"] to port.
    if (port.has_value())
        result.port = port;

    // 7. If pathname is not null, set result["pathname"] to pathname.
    if (pathname.has_value())
        result.pathname = pathname;

    // 8. If search is not null, set result["search"] to search.
    if (search.has_value())
        result.search = search;

    // 9. If hash is not null, set result["hash"] to hash.
    if (hash.has_value())
        result.hash = hash;

    // 10. Let baseURL be null.
    Optional<URL> base_url;

    // 11. If init["baseURL"] exists:
    if (init.base_url.has_value()) {
        // 1. Set baseURL to the result of running the basic URL parser on init["baseURL"].
        base_url = Parser::basic_parse(init.base_url.value());

        // 2. If baseURL is failure, then throw a TypeError.
        if (!base_url.has_value())
            return ErrorInfo { MUST(String::formatted("Invalid base URL '{}' provided for URLPattern", init.base_url.value())) };

        // 3. If init["protocol"] does not exist, then set result["protocol"] to the result of processing a base URL
        //    string given baseURL’s scheme and type.
        if (!init.protocol.has_value())
            result.protocol = process_a_base_url_string(base_url->scheme(), type);

        // 4. If type is not "pattern" and init contains none of "protocol", "hostname", "port" and "username", then
        //    set result["username"] to the result of processing a base URL string given baseURL’s username and type.
        if (type != PatternProcessType::Pattern && !init.protocol.has_value() && !init.hostname.has_value()
            && !init.port.has_value() && !init.username.has_value()) {
            result.username = process_a_base_url_string(base_url->username(), type);
        }

        // 5. If type is not "pattern" and init contains none of "protocol", "hostname", "port", "username" and
        //    "password", then set result["password"] to the result of processing a base URL string given baseURL’s
        //    password and type.
        if (type != PatternProcessType::Pattern && !init.protocol.has_value() && !init.hostname.has_value()
            && !init.port.has_value() && !init.username.has_value() && !init.password.has_value()) {
            result.password = process_a_base_url_string(base_url->password(), type);
        }

        // 6. If init contains neither "protocol" nor "hostname", then:
        if (!init.protocol.has_value() && !init.hostname.has_value()) {
            // 1. Let baseHost be the serialization of baseURL's host, if it is not null, and the empty string otherwise.
            String base_host = base_url->host().has_value() ? base_url->host()->serialize() : String {};

            // 2. Set result["hostname"] to the result of processing a base URL string given baseHost and type.
            result.hostname = process_a_base_url_string(base_host, type);
        }

        // 7. If init contains none of "protocol", "hostname", and "port", then:
        if (!init.protocol.has_value() && !init.hostname.has_value() && !init.port.has_value()) {
            // 1. If baseURL’s port is null, then set result["port"] to the empty string.
            if (!base_url->port().has_value()) {
                result.port = String {};
            }
            // 2. Otherwise, set result["port"] to baseURL’s port, serialized.
            else {
                result.port = String::number(*base_url->port());
            }
        }

        // 8. If init contains none of "protocol", "hostname", "port", and "pathname", then set result["pathname"] to
        //    the result of processing a base URL string given the result of URL path serializing baseURL and type.
        if (!init.protocol.has_value() && !init.hostname.has_value() && !init.port.has_value() && !init.pathname.has_value())
            result.pathname = process_a_base_url_string(base_url->serialize_path(), type);

        // 9. If init contains none of "protocol", "hostname", "port", "pathname", and "search", then:
        if (!init.protocol.has_value() && !init.hostname.has_value() && !init.port.has_value() && !init.pathname.has_value() && !init.search.has_value()) {
            // 1. Let baseQuery be baseURL’s query.
            auto const& base_query = base_url->query();

            // 2. If baseQuery is null, then set baseQuery to the empty string.
            // 3. Set result["search"] to the result of processing a base URL string given baseQuery and type.
            result.search = process_a_base_url_string(base_query.value_or(String {}), type);
        }

        // 10. If init contains none of "protocol", "hostname", "port", "pathname", "search", and "hash", then:
        if (!init.protocol.has_value() && !init.hostname.has_value() && !init.port.has_value() && !init.pathname.has_value()
            && !init.search.has_value() && !init.hash.has_value()) {
            // 1. Let baseFragment be baseURL’s fragment.
            auto const& base_fragment = base_url->fragment();

            // 2. If baseFragment is null, then set baseFragment to the empty string.
            // 3. Set result["hash"] to the result of processing a base URL string given baseFragment and type.
            result.hash = process_a_base_url_string(base_fragment.value_or(String {}), type);
        }
    }

    // 12. If init["protocol"] exists, then set result["protocol"] to the result of process protocol for init given init["protocol"] and type.
    if (init.protocol.has_value())
        result.protocol = TRY(process_protocol_for_init(init.protocol.value(), type));

    // 13. If init["username"] exists, then set result["username"] to the result of process username for init given init["username"] and type.
    if (init.username.has_value())
        result.username = process_username_for_init(init.username.value(), type);

    // 14. If init["password"] exists, then set result["password"] to the result of process password for init given init["password"] and type.
    if (init.password.has_value())
        result.password = process_password_for_init(init.password.value(), type);

    // 15. If init["hostname"] exists, then set result["hostname"] to the result of process hostname for init given init["hostname"] and type.
    if (init.hostname.has_value())
        result.hostname = TRY(process_hostname_for_init(init.hostname.value(), type));

    // 16. Let resultProtocolString be result["protocol"] if it exists; otherwise the empty string.
    auto result_protocol_string = result.protocol.value_or(String {});

    // 17. If init["port"] exists, then set result["port"] to the result of process port for init given init["port"], resultProtocolString, and type.
    if (init.port.has_value())
        result.port = TRY(process_port_for_init(init.port.value(), result_protocol_string, type));

    // 18. If init["pathname"] exists:
    if (init.pathname.has_value()) {
        // 1. Set result["pathname"] to init["pathname"].
        result.pathname = init.pathname.value();

        // 2. If the following are all true:
        //     * baseURL is not null;
        //     * baseURL does not have an opaque path; and
        //     * the result of running is an absolute pathname given result["pathname"] and type is false,
        //    then:
        if (base_url.has_value()
            && !base_url->has_an_opaque_path()
            && !is_an_absolute_pathname(result.pathname.value(), type)) {
            // 1. Let baseURLPath be the result of running process a base URL string given the result of URL path
            //    serializing baseURL and type.
            auto base_url_path = process_a_base_url_string(base_url->serialize_path(), type);

            // 2. Let slash index be the index of the last U+002F (/) code point found in baseURLPath, interpreted as a
            //    sequence of code points, or null if there are no instances of the code point.
            auto slash_index = base_url_path.bytes_as_string_view().find_last('/');

            // 3. If slash index is not null:
            if (slash_index.has_value()) {
                // 1. Let new pathname be the code point substring from 0 to slash index + 1 within baseURLPath.
                auto new_pathname = base_url_path.bytes_as_string_view().substring_view(0, *slash_index + 1);

                // 2. Append result["pathname"] to the end of new pathname.
                // 3. Set result["pathname"] to new pathname.
                result.pathname = MUST(String::formatted("{}{}", new_pathname, *result.pathname));
            }
        }

        // 3. Set result["pathname"] to the result of process pathname for init given result["pathname"], resultProtocolString, and type.
        result.pathname = TRY(process_pathname_for_init(result.pathname.value(), result_protocol_string, type));
    }

    // 19. If init["search"] exists then set result["search"] to the result of process search for init given init["search"] and type.
    if (init.search.has_value())
        result.search = process_search_for_init(init.search.value(), type);

    // 20. If init["hash"] exists then set result["hash"] to the result of process hash for init given init["hash"] and type.
    if (init.hash.has_value())
        result.hash = process_hash_for_init(init.hash.value(), type);

    // 21. Return result.
    return result;
}

}
