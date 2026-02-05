/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
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
        auto maybe_port = processed_init.port->to_number<u16>(TrimWhitespace::No);
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
PatternErrorOr<Optional<Result>> Pattern::match(Variant<String, Init, URL> const& input, Optional<String> const& base_url_string) const
{
    // 1. Let protocol be the empty string.
    String protocol;

    // 2. Let username be the empty string.
    String username;

    // 3. Let password be the empty string.
    String password;

    // 4. Let hostname be the empty string.
    String hostname;

    // 5. Let port be the empty string.
    String port;

    // 6. Let pathname be the empty string.
    String pathname;

    // 7. Let search be the empty string.
    String search;

    // 8. Let hash be the empty string.
    String hash;

    // 9. Let inputs be an empty list.
    Vector<Input> inputs;

    // 10. If input is a URL, then append the serialization of input to inputs.
    if (auto const* input_url = input.get_pointer<URL>()) {
        inputs.append(input_url->serialize());
    }
    // 11. Otherwise, append input to inputs.
    else {
        inputs.append(input.downcast<Input>());
    }

    // 12. If input is a URLPatternInit then:
    if (auto const* input_init = input.get_pointer<Init>()) {
        // 1. If baseURLString was given, throw a TypeError.
        if (base_url_string.has_value())
            return ErrorInfo { "Base URL cannot be provided when URLPatternInput is provided"_string };

        // 2. Let applyResult be the result of process a URLPatternInit given input, "url", protocol, username, password,
        //    hostname, port, pathname, search, and hash. If this throws an exception, catch it, and return null.
        auto apply_result_or_error = process_a_url_pattern_init(*input_init, PatternProcessType::URL,
            protocol, username, password, hostname, port, pathname, search, hash);
        if (apply_result_or_error.is_error())
            return OptionalNone {};
        auto apply_result = apply_result_or_error.release_value();

        // 3. Set protocol to applyResult["protocol"].
        protocol = apply_result.protocol.value();

        // 4. Set username to applyResult["username"].
        username = apply_result.username.value();

        // 5. Set password to applyResult["password"].
        password = apply_result.password.value();

        // 6. Set hostname to applyResult["hostname"].
        hostname = apply_result.hostname.value();

        // 7. Set port to applyResult["port"].
        port = apply_result.port.value();

        // 8. Set pathname to applyResult["pathname"].
        pathname = apply_result.pathname.value();

        // 9. Set search to applyResult["search"].
        search = apply_result.search.value();

        // 10. Set hash to applyResult["hash"].
        hash = apply_result.hash.value();
    }
    // 13. Otherwise:
    else {
        // 1. Let url be input.
        auto url_or_string = input.downcast<URL, String>();

        // 2. If input is a USVString:
        if (auto const* input_string = input.get_pointer<String>()) {
            // 1. Let baseURL be null.
            Optional<URL> base_url;

            // 2. If baseURLString was given, then:
            if (base_url_string.has_value()) {
                // 1. Set baseURL to the result of running the basic URL parser on baseURLString.
                base_url = Parser::basic_parse(base_url_string.value());

                // 2. If baseURL is failure, return null.
                if (!base_url.has_value())
                    return OptionalNone {};

                // 3. Append baseURLString to inputs.
                inputs.append(base_url_string.value());
            }

            // 3. Set url to the result of running the basic URL parser on input with baseURL.
            // 4. If url is failure, return null.
            auto maybe_url = Parser::basic_parse(*input_string, base_url);
            if (!maybe_url.has_value())
                return OptionalNone {};
            url_or_string = maybe_url.release_value();
        }

        // 3. Assert: url is a URL.
        VERIFY(url_or_string.has<URL>());
        auto& url = url_or_string.get<URL>();

        // 4. Set protocol to url’s scheme.
        protocol = url.scheme();

        // 5. Set username to url’s username.
        username = url.username();

        // 6. Set password to url’s password.
        password = url.password();

        // 7. Set hostname to url’s host, serialized, or the empty string if the value is null.
        if (url.host().has_value())
            hostname = url.host()->serialize();
        else
            hostname = String {};

        // 8. Set port to url’s port, serialized, or the empty string if the value is null.
        if (url.port().has_value())
            port = String::number(url.port().value());
        else
            port = String {};

        // 9. Set pathname to the result of URL path serializing url.
        pathname = url.serialize_path();

        // 10. Set search to url’s query or the empty string if the value is null.
        search = url.query().value_or(String {});

        // 11. Set hash to url’s fragment or the empty string if the value is null.
        hash = url.fragment().value_or(String {});
    }

    // 14. Let protocolExecResult be RegExpBuiltinExec(urlPattern’s protocol component's regular expression, protocol).
    auto protocol_exec_result = m_protocol_component.regular_expression->match(protocol);
    if (!protocol_exec_result.success)
        return OptionalNone {};

    // 15. Let usernameExecResult be RegExpBuiltinExec(urlPattern’s username component's regular expression, username).
    auto username_exec_result = m_username_component.regular_expression->match(username);
    if (!username_exec_result.success)
        return OptionalNone {};

    // 16. Let passwordExecResult be RegExpBuiltinExec(urlPattern’s password component's regular expression, password).
    auto password_exec_result = m_password_component.regular_expression->match(password);
    if (!password_exec_result.success)
        return OptionalNone {};

    // 17. Let hostnameExecResult be RegExpBuiltinExec(urlPattern’s hostname component's regular expression, hostname).
    auto hostname_exec_result = m_hostname_component.regular_expression->match(hostname);
    if (!hostname_exec_result.success)
        return OptionalNone {};

    // 18. Let portExecResult be RegExpBuiltinExec(urlPattern’s port component's regular expression, port).
    auto port_exec_result = m_port_component.regular_expression->match(port);
    if (!port_exec_result.success)
        return OptionalNone {};

    // 19. Let pathnameExecResult be RegExpBuiltinExec(urlPattern’s pathname component's regular expression, pathname).
    auto pathname_exec_result = m_pathname_component.regular_expression->match(pathname);
    if (!pathname_exec_result.success)
        return OptionalNone {};

    // 20. Let searchExecResult be RegExpBuiltinExec(urlPattern’s search component's regular expression, search).
    auto search_exec_result = m_search_component.regular_expression->match(search);
    if (!search_exec_result.success)
        return OptionalNone {};

    // 21. Let hashExecResult be RegExpBuiltinExec(urlPattern’s hash component's regular expression, hash).
    auto hash_exec_result = m_hash_component.regular_expression->match(hash);
    if (!hash_exec_result.success)
        return OptionalNone {};

    // 22. If protocolExecResult, usernameExecResult, passwordExecResult, hostnameExecResult, portExecResult,
    //     pathnameExecResult, searchExecResult, or hashExecResult are null then return null.
    // NOTE: Done in steps above at point of exec.

    // 23. Let result be a new URLPatternResult.
    Result result;

    // 24. Set result["inputs"] to inputs.
    result.inputs = move(inputs);

    // 25. Set result["protocol"] to the result of creating a component match result given urlPattern’s protocol
    //     component, protocol, and protocolExecResult.
    result.protocol = m_protocol_component.create_match_result(protocol, protocol_exec_result);

    // 26. Set result["username"] to the result of creating a component match result given urlPattern’s username
    //     component, username, and usernameExecResult.
    result.username = m_username_component.create_match_result(username, username_exec_result);

    // 27. Set result["password"] to the result of creating a component match result given urlPattern’s password
    //     component, password, and passwordExecResult.
    result.password = m_password_component.create_match_result(password, password_exec_result);

    // 28. Set result["hostname"] to the result of creating a component match result given urlPattern’s hostname
    //     component, hostname, and hostnameExecResult.
    result.hostname = m_hostname_component.create_match_result(hostname, hostname_exec_result);

    // 29. Set result["port"] to the result of creating a component match result given urlPattern’s port component,
    //     port, and portExecResult.
    result.port = m_port_component.create_match_result(port, port_exec_result);

    // 30. Set result["pathname"] to the result of creating a component match result given urlPattern’s pathname
    //     component, pathname, and pathnameExecResult.
    result.pathname = m_pathname_component.create_match_result(pathname, pathname_exec_result);

    // 31. Set result["search"] to the result of creating a component match result given urlPattern’s search component,
    //     search, and searchExecResult.
    result.search = m_search_component.create_match_result(search, search_exec_result);

    // 32. Set result["hash"] to the result of creating a component match result given urlPattern’s hash component,
    //     hash, and hashExecResult.
    result.hash = m_hash_component.create_match_result(hash, hash_exec_result);

    // 33. Return result.
    return result;
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
