/*
 * Copyright (c) 2025-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::pattern::ErrorInfo;
use crate::pattern::PatternErrorOr;
use crate::url::BasicParseOptions;
use crate::url::State;
use crate::url::Url;
use crate::url::basic_parse;
use crate::url::basic_parse_into;

// https://urlpattern.spec.whatwg.org/#url-pattern-create-a-dummy-url
fn create_a_dummy_url() -> Url {
    // 1. Let dummyInput be "https://dummy.invalid/".
    // 2. Return the result of running the basic URL parser on dummyInput.
    basic_parse("https://dummy.invalid/", BasicParseOptions::new()).unwrap()
}

// https://urlpattern.spec.whatwg.org/#canonicalize-a-protocol
pub fn canonicalize_a_protocol(value: &str) -> PatternErrorOr<String> {
    // 1. If value is the empty string, return value.
    if value.is_empty() {
        return Ok(value.to_string());
    }

    // 2. Let parseResult be the result of running the basic URL parser given value followed by "://dummy.invalid/".
    // NOTE: Note, state override is not used here because it enforces restrictions that are only appropriate for the
    //       protocol setter. Instead we use the protocol to parse a dummy URL using the normal parsing entry point.
    let parse_result = basic_parse(&format!("{value}://dummy.invalid"), BasicParseOptions::new());

    // 4. If parseResult is failure, then throw a TypeError.
    let Some(parse_result) = parse_result else {
        return Err(ErrorInfo::new("Failed to canonicalize URL protocol string"));
    };

    // 5. Return parseResult’s scheme.
    Ok(parse_result.scheme)
}

// https://urlpattern.spec.whatwg.org/#canonicalize-a-username
pub fn canonicalize_a_username(value: &str) -> String {
    // 1. If value is the empty string, return value.
    if value.is_empty() {
        return value.to_string();
    }

    // 2. Let dummyURL be the result of creating a dummy URL.
    let mut dummy_url = create_a_dummy_url();

    // 3. Set the username given dummyURL and value.
    dummy_url.set_username(value);

    // 4. Return dummyURL’s username.
    dummy_url.username
}

// https://urlpattern.spec.whatwg.org/#canonicalize-a-password
pub fn canonicalize_a_password(value: &str) -> String {
    // 1. If value is the empty string, return value.
    if value.is_empty() {
        return value.to_string();
    }

    // 2. Let dummyURL be the result of creating a dummy URL.
    let mut dummy_url = create_a_dummy_url();

    // 3. Set the password given dummyURL and value.
    dummy_url.set_password(value);

    // 4. Return dummyURL’s password.
    dummy_url.password
}

// https://urlpattern.spec.whatwg.org/#canonicalize-a-hostname
pub fn canonicalize_a_hostname(value: &str) -> PatternErrorOr<String> {
    // 1. If value is the empty string, return value.
    if value.is_empty() {
        return Ok(value.to_string());
    }

    // 2. Let dummyURL be the result of creating a dummy URL.
    let mut dummy_url = create_a_dummy_url();

    // 3. Let parseResult be the result of running the basic URL parser given value with dummyURL
    //    as url and hostname state as state override.
    let parse_result = basic_parse_into(
        value,
        &mut dummy_url,
        &BasicParseOptions::new().state_override(State::Hostname),
    );

    // 4. If parseResult is failure, then throw a TypeError.
    if !parse_result {
        return Err(ErrorInfo::new("Failed to canonicalize URL hostname string"));
    }

    // 5. Return dummyURL’s host, serialized, or empty string if it is null.
    if dummy_url.host.is_none() {
        return Ok(String::new());
    }
    Ok(dummy_url.serialized_host())
}

// https://urlpattern.spec.whatwg.org/#canonicalize-an-ipv6-hostname
pub fn canonicalize_an_ipv6_hostname(value: &str) -> PatternErrorOr<String> {
    // 1. Let result be the empty string.
    let mut result = String::new();

    // 2. For each code point in value interpreted as a list of code points:
    for code_point in value.chars() {
        // 1. If all of the following are true:
        //     * code point is not an ASCII hex digit;
        //     * code point is not U+005B ([);
        //     * code point is not U+005D (]); and
        //     * code point is not U+003A (:),
        //    then throw a TypeError.
        if !code_point.is_ascii_hexdigit() && code_point != '[' && code_point != ']' && code_point != ':' {
            return Err(ErrorInfo::new("Failed to canonicalize IPv6 hostname string"));
        }

        // 2. Append the result of running ASCII lowercase given code point to the end of result.
        result.push(code_point.to_ascii_lowercase());
    }

    // 3. Return result.
    Ok(result)
}

// https://urlpattern.spec.whatwg.org/#canonicalize-a-port
pub fn canonicalize_a_port(port_value: &str, protocol_value: &Option<String>) -> PatternErrorOr<String> {
    // 1. If portValue is the empty string, return portValue.
    if port_value.is_empty() {
        return Ok(port_value.to_string());
    }

    // 2. Let dummyURL be the result of creating a dummy URL.
    let mut dummy_url = create_a_dummy_url();

    // 3. If protocolValue was given, then set dummyURL’s scheme to protocolValue.
    // NOTE: Note, we set the URL record's scheme in order for the basic URL parser to
    //       recognize and normalize default port values.
    if let Some(protocol_value) = protocol_value {
        dummy_url.set_scheme(protocol_value.clone());
    }

    // 4. Let parseResult be the result of running basic URL parser given portValue with dummyURL
    //    as url and port state as state override.
    let parse_result = basic_parse_into(
        port_value,
        &mut dummy_url,
        &BasicParseOptions::new().state_override(State::Port),
    );

    // 4. If parseResult is failure, then throw a TypeError.
    if !parse_result {
        return Err(ErrorInfo::new("Failed to canonicalize port string"));
    }

    // 5. Return dummyURL’s port, serialized, or empty string if it is null.
    if dummy_url.port.is_none() {
        return Ok(String::new());
    }
    Ok(dummy_url.port.unwrap().to_string())
}

// https://urlpattern.spec.whatwg.org/#canonicalize-a-pathname
pub fn canonicalize_a_pathname(value: &str) -> String {
    // 1. If value is the empty string, then return value.
    if value.is_empty() {
        return value.to_string();
    }

    // 2. Let leading slash be true if the first code point in value is U+002F (/) and otherwise false.
    let leading_slash = value.as_bytes()[0] == b'/';

    // 3. Let modified value be "/-" if leading slash is false and otherwise the empty string.
    let mut modified_value = String::new();
    if !leading_slash {
        modified_value.push_str("/-");
    }

    // 4. Append value to the end of modified value.
    modified_value.push_str(value);

    // 5. Let dummyURL be the result of creating a dummy URL.
    let mut dummy_url = create_a_dummy_url();

    // 6. Empty dummyURL’s path.
    dummy_url.set_paths(&[]);

    // 7. Run basic URL parser given modified value with dummyURL as url and path start state as state override.
    let _ = basic_parse_into(
        &modified_value,
        &mut dummy_url,
        &BasicParseOptions::new().state_override(State::PathStart),
    );

    // 8. Let result be the result of URL path serializing dummyURL.
    let mut result = dummy_url.serialize_path();

    // 9. If leading slash is false, then set result to the code point substring from 2 to the end of the string within result.
    if !leading_slash {
        result = result.chars().skip(2).collect();
    }

    // 10. Return result.
    result
}

// https://urlpattern.spec.whatwg.org/#canonicalize-an-opaque-pathname
pub fn canonicalize_an_opaque_pathname(value: &str) -> PatternErrorOr<String> {
    // 1. If value is the empty string, return value.
    if value.is_empty() {
        return Ok(value.to_string());
    }

    // 2. Let dummyURL be the result of creating a dummy URL.
    let mut dummy_url = create_a_dummy_url();

    // 3. Set dummyURL’s path to the empty string.
    dummy_url.set_paths(&[""]);
    dummy_url.set_has_an_opaque_path(true);

    // 4. Let parseResult be the result of running URL parsing given value with dummyURL as url and opaque path state as state override.
    let parse_result = basic_parse_into(
        value,
        &mut dummy_url,
        &BasicParseOptions::new().state_override(State::OpaquePath),
    );

    // 5. If parseResult is failure, then throw a TypeError.
    if !parse_result {
        return Err(ErrorInfo::new("Failed to canonicalize opaque pathname string"));
    }

    // 6. Return the result of URL path serializing dummyURL.
    Ok(dummy_url.serialize_path())
}

// https://urlpattern.spec.whatwg.org/#canonicalize-a-search
pub fn canonicalize_a_search(value: &str) -> String {
    // 1. If value is the empty string, return value.
    if value.is_empty() {
        return value.to_string();
    }

    // 2. Let dummyURL be the result of creating a dummy URL.
    let mut dummy_url = create_a_dummy_url();

    // 3. Set dummyURL’s query to the empty string.
    dummy_url.set_query(Some(String::new()));

    // 4. Run basic URL parser given value with dummyURL as url and query state as state override.
    let _ = basic_parse_into(
        value,
        &mut dummy_url,
        &BasicParseOptions::new().state_override(State::Query),
    );

    // 5. Return dummyURL’s query.
    dummy_url.query.expect("query should be present")
}

// https://urlpattern.spec.whatwg.org/#canonicalize-a-hash
pub fn canonicalize_a_hash(value: &str) -> String {
    // 1. If value is the empty string, return value.
    if value.is_empty() {
        return value.to_string();
    }

    // 2. Let dummyURL be the result of creating a dummy URL.
    let mut dummy_url = create_a_dummy_url();

    // 3. Set dummyURL’s fragment to the empty string.
    dummy_url.set_fragment(Some(String::new()));

    // 4. Run basic URL parser given value with dummyURL as url and fragment state as state override.
    let _ = basic_parse_into(
        value,
        &mut dummy_url,
        &BasicParseOptions::new().state_override(State::Fragment),
    );

    // 5. Return dummyURL’s fragment.
    dummy_url.fragment.expect("fragment should be present")
}
