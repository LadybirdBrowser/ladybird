/*
 * Copyright (c) 2025-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::pattern::ErrorInfo;
use crate::pattern::PatternErrorOr;
use crate::pattern::canonicalize_a_hash;
use crate::pattern::canonicalize_a_hostname;
use crate::pattern::canonicalize_a_password;
use crate::pattern::canonicalize_a_pathname;
use crate::pattern::canonicalize_a_port;
use crate::pattern::canonicalize_a_protocol;
use crate::pattern::canonicalize_a_search;
use crate::pattern::canonicalize_a_username;
use crate::pattern::canonicalize_an_opaque_pathname;
use crate::pattern::escape_a_pattern_string;
use crate::url::BasicParseOptions;
use crate::url::basic_parse;
use crate::url::is_special_scheme;

// https://urlpattern.spec.whatwg.org/#dictdef-urlpatterninit
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct Init {
    pub protocol: Option<String>,
    pub username: Option<String>,
    pub password: Option<String>,
    pub hostname: Option<String>,
    pub port: Option<String>,
    pub pathname: Option<String>,
    pub search: Option<String>,
    pub hash: Option<String>,
    pub base_url: Option<String>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum PatternProcessType {
    Pattern,
    Url,
}

// https://urlpattern.spec.whatwg.org/#process-a-base-url-string
fn process_a_base_url_string(input: &str, r#type: PatternProcessType) -> String {
    // 1. Assert: input is not null.
    // 2. If type is not "pattern" return input.
    if r#type != PatternProcessType::Pattern {
        return input.to_string();
    }

    // 3. Return the result of escaping a pattern string given input.
    escape_a_pattern_string(input)
}

// https://urlpattern.spec.whatwg.org/#is-an-absolute-pathname
fn is_an_absolute_pathname(input: &str, r#type: PatternProcessType) -> bool {
    // 1. If input is the empty string, then return false.
    if input.is_empty() {
        return false;
    }

    // 2. If input[0] is U+002F (/), then return true.
    if input.as_bytes()[0] == b'/' {
        return true;
    }

    // 3. If type is "url", then return false.
    if r#type == PatternProcessType::Url {
        return false;
    }

    // 4. If input’s code point length is less than 2, then return false.
    if input.len() < 2 {
        return false;
    }

    // 5. If input[0] is U+005C (\) and input[1] is U+002F (/), then return true.
    if input.as_bytes()[0] == b'\\' && input.as_bytes()[1] == b'/' {
        return true;
    }

    // 6. If input[0] is U+007B ({) and input[1] is U+002F (/), then return true.
    if input.as_bytes()[0] == b'{' && input.as_bytes()[1] == b'/' {
        return true;
    }

    // 7. Return false.
    false
}

// https://urlpattern.spec.whatwg.org/#process-protocol-for-init
fn process_protocol_for_init(value: &str, r#type: PatternProcessType) -> PatternErrorOr<String> {
    // 1. Let strippedValue be the given value with a single trailing U+003A (:) removed, if any.
    let mut stripped_value = value.to_string();
    if stripped_value.ends_with(':') {
        stripped_value.pop();
    }

    // 2. If type is "pattern" then return strippedValue.
    if r#type == PatternProcessType::Pattern {
        return Ok(stripped_value);
    }

    // 3. Return the result of running canonicalize a protocol given strippedValue.
    canonicalize_a_protocol(&stripped_value)
}

// https://urlpattern.spec.whatwg.org/#process-username-for-init
fn process_username_for_init(value: &str, r#type: PatternProcessType) -> String {
    // 1. If type is "pattern" then return value.
    if r#type == PatternProcessType::Pattern {
        return value.to_string();
    }

    // 2. Return the result of running canonicalize a username given value.
    canonicalize_a_username(value)
}

// https://urlpattern.spec.whatwg.org/#process-password-for-init
fn process_password_for_init(value: &str, r#type: PatternProcessType) -> String {
    // 1. If type is "pattern" then return value.
    if r#type == PatternProcessType::Pattern {
        return value.to_string();
    }

    // 2. Return the result of running canonicalize a password given value.
    canonicalize_a_password(value)
}

// https://urlpattern.spec.whatwg.org/#process-hostname-for-init
fn process_hostname_for_init(value: &str, r#type: PatternProcessType) -> PatternErrorOr<String> {
    // 1. If type is "pattern" then return value.
    if r#type == PatternProcessType::Pattern {
        return Ok(value.to_string());
    }

    // 2. Return the result of running canonicalize a hostname given value.
    canonicalize_a_hostname(value)
}

// https://urlpattern.spec.whatwg.org/#process-port-for-init
fn process_port_for_init(port_value: &str, protocol_value: &str, r#type: PatternProcessType) -> PatternErrorOr<String> {
    // 1. If type is "pattern" then return portValue.
    if r#type == PatternProcessType::Pattern {
        return Ok(port_value.to_string());
    }

    // 2. Return the result of running canonicalize a port given portValue and protocolValue.
    canonicalize_a_port(port_value, &Some(protocol_value.to_string()))
}

// https://urlpattern.spec.whatwg.org/#process-pathname-for-init
fn process_pathname_for_init(
    pathname_value: &str,
    protocol_value: &str,
    r#type: PatternProcessType,
) -> PatternErrorOr<String> {
    // 1. If type is "pattern" then return pathnameValue.
    if r#type == PatternProcessType::Pattern {
        return Ok(pathname_value.to_string());
    }

    // 2. If protocolValue is a special scheme or the empty string, then return the result of running canonicalize a
    //    pathname given pathnameValue.
    // NOTE: If the protocolValue is the empty string then no value was provided for protocol in the constructor
    //       dictionary. Normally we do not special case empty string dictionary values, but in this case we treat
    //       it as a special scheme in order to default to the most common pathname canonicalization.
    if protocol_value.is_empty() || is_special_scheme(protocol_value.as_bytes()) {
        return Ok(canonicalize_a_pathname(pathname_value));
    }

    // 3. Return the result of running canonicalize an opaque pathname given pathnameValue.
    canonicalize_an_opaque_pathname(pathname_value)
}

// https://urlpattern.spec.whatwg.org/#process-search-for-init
fn process_search_for_init(value: &str, r#type: PatternProcessType) -> String {
    // 1. Let strippedValue be the given value with a single leading U+003F (?) removed, if any.
    let stripped_value = value.strip_prefix('?').unwrap_or(value);

    // 2. If type is "pattern" then return strippedValue.
    if r#type == PatternProcessType::Pattern {
        return stripped_value.to_string();
    }

    // 3. Return the result of running canonicalize a search given strippedValue.
    canonicalize_a_search(stripped_value)
}

// https://urlpattern.spec.whatwg.org/#process-hash-for-init
fn process_hash_for_init(value: &str, r#type: PatternProcessType) -> String {
    // 1. Let strippedValue be the given value with a single leading U+0023 (#) removed, if any.
    let stripped_value = value.strip_prefix('#').unwrap_or(value);

    // 2. If type is "pattern" then return strippedValue.
    if r#type == PatternProcessType::Pattern {
        return stripped_value.to_string();
    }

    // 3. Return the result of running canonicalize a hash given strippedValue.
    canonicalize_a_hash(stripped_value)
}

// https://urlpattern.spec.whatwg.org/#process-a-urlpatterninit
#[allow(clippy::too_many_arguments)]
pub fn process_a_url_pattern_init(
    init: &Init,
    r#type: PatternProcessType,
    protocol: &Option<String>,
    username: &Option<String>,
    password: &Option<String>,
    hostname: &Option<String>,
    port: &Option<String>,
    pathname: &Option<String>,
    search: &Option<String>,
    hash: &Option<String>,
) -> PatternErrorOr<Init> {
    // 1. Let result be the result of creating a new URLPatternInit.
    let mut result = Init::default();

    // 2. If protocol is not null, set result["protocol"] to protocol.
    if let Some(protocol) = protocol {
        result.protocol = Some(protocol.clone());
    }

    // 3. If username is not null, set result["username"] to username.
    if let Some(username) = username {
        result.username = Some(username.clone());
    }

    // 4. If password is not null, set result["password"] to password.
    if let Some(password) = password {
        result.password = Some(password.clone());
    }

    // 5. If hostname is not null, set result["hostname"] to hostname.
    if let Some(hostname) = hostname {
        result.hostname = Some(hostname.clone());
    }

    // 6. If port is not null, set result["port"] to port.
    if let Some(port) = port {
        result.port = Some(port.clone());
    }

    // 7. If pathname is not null, set result["pathname"] to pathname.
    if let Some(pathname) = pathname {
        result.pathname = Some(pathname.clone());
    }

    // 8. If search is not null, set result["search"] to search.
    if let Some(search) = search {
        result.search = Some(search.clone());
    }

    // 9. If hash is not null, set result["hash"] to hash.
    if let Some(hash) = hash {
        result.hash = Some(hash.clone());
    }

    // 10. Let baseURL be null.
    let mut base_url = None;

    // 11. If init["baseURL"] exists:
    if let Some(init_base_url) = &init.base_url {
        // 1. Set baseURL to the result of running the basic URL parser on init["baseURL"].
        base_url = basic_parse(init_base_url, BasicParseOptions::new());

        // 2. If baseURL is failure, then throw a TypeError.
        let Some(base_url_ref) = base_url.as_ref() else {
            return Err(ErrorInfo::new(format!(
                "Invalid base URL '{init_base_url}' provided for URLPattern"
            )));
        };

        // 3. If init["protocol"] does not exist, then set result["protocol"] to the result of processing a base URL
        //    string given baseURL’s scheme and type.
        if init.protocol.is_none() {
            result.protocol = Some(process_a_base_url_string(&base_url_ref.scheme, r#type));
        }

        // 4. If type is not "pattern" and init contains none of "protocol", "hostname", "port" and "username", then
        //    set result["username"] to the result of processing a base URL string given baseURL’s username and type.
        if r#type != PatternProcessType::Pattern
            && init.protocol.is_none()
            && init.hostname.is_none()
            && init.port.is_none()
            && init.username.is_none()
        {
            result.username = Some(process_a_base_url_string(&base_url_ref.username, r#type));
        }

        // 5. If type is not "pattern" and init contains none of "protocol", "hostname", "port", "username" and
        //    "password", then set result["password"] to the result of processing a base URL string given baseURL’s
        //    password and type.
        if r#type != PatternProcessType::Pattern
            && init.protocol.is_none()
            && init.hostname.is_none()
            && init.port.is_none()
            && init.username.is_none()
            && init.password.is_none()
        {
            result.password = Some(process_a_base_url_string(&base_url_ref.password, r#type));
        }

        // 6. If init contains neither "protocol" nor "hostname", then:
        if init.protocol.is_none() && init.hostname.is_none() {
            // 1. Let baseHost be the serialization of baseURL's host, if it is not null, and the empty string otherwise.
            let base_host = if base_url_ref.host.is_some() {
                base_url_ref.serialized_host()
            } else {
                String::new()
            };

            // 2. Set result["hostname"] to the result of processing a base URL string given baseHost and type.
            result.hostname = Some(process_a_base_url_string(&base_host, r#type));
        }

        // 7. If init contains none of "protocol", "hostname", and "port", then:
        if init.protocol.is_none() && init.hostname.is_none() && init.port.is_none() {
            // 1. If baseURL’s port is null, then set result["port"] to the empty string.
            if base_url_ref.port.is_none() {
                result.port = Some(String::new());
            }
            // 2. Otherwise, set result["port"] to baseURL’s port, serialized.
            else if let Some(base_url_port) = base_url_ref.port {
                result.port = Some(base_url_port.to_string());
            }
        }

        // 8. If init contains none of "protocol", "hostname", "port", and "pathname", then set result["pathname"] to
        //    the result of processing a base URL string given the result of URL path serializing baseURL and type.
        if init.protocol.is_none() && init.hostname.is_none() && init.port.is_none() && init.pathname.is_none() {
            result.pathname = Some(process_a_base_url_string(&base_url_ref.serialize_path(), r#type));
        }

        // 9. If init contains none of "protocol", "hostname", "port", "pathname", and "search", then:
        if init.protocol.is_none()
            && init.hostname.is_none()
            && init.port.is_none()
            && init.pathname.is_none()
            && init.search.is_none()
        {
            // 1. Let baseQuery be baseURL’s query.
            let base_query = &base_url_ref.query;

            // 2. If baseQuery is null, then set baseQuery to the empty string.
            // 3. Set result["search"] to the result of processing a base URL string given baseQuery and type.
            result.search = Some(process_a_base_url_string(base_query.as_deref().unwrap_or(""), r#type));
        }

        // 10. If init contains none of "protocol", "hostname", "port", "pathname", "search", and "hash", then:
        if init.protocol.is_none()
            && init.hostname.is_none()
            && init.port.is_none()
            && init.pathname.is_none()
            && init.search.is_none()
            && init.hash.is_none()
        {
            // 1. Let baseFragment be baseURL’s fragment.
            let base_fragment = &base_url_ref.fragment;

            // 2. If baseFragment is null, then set baseFragment to the empty string.
            // 3. Set result["hash"] to the result of processing a base URL string given baseFragment and type.
            result.hash = Some(process_a_base_url_string(
                base_fragment.as_deref().unwrap_or(""),
                r#type,
            ));
        }
    }

    // 12. If init["protocol"] exists, then set result["protocol"] to the result of process protocol for init given init["protocol"] and type.
    if let Some(protocol) = &init.protocol {
        result.protocol = Some(process_protocol_for_init(protocol, r#type)?);
    }

    // 13. If init["username"] exists, then set result["username"] to the result of process username for init given init["username"] and type.
    if let Some(username) = &init.username {
        result.username = Some(process_username_for_init(username, r#type));
    }

    // 14. If init["password"] exists, then set result["password"] to the result of process password for init given init["password"] and type.
    if let Some(password) = &init.password {
        result.password = Some(process_password_for_init(password, r#type));
    }

    // 15. If init["hostname"] exists, then set result["hostname"] to the result of process hostname for init given init["hostname"] and type.
    if let Some(hostname) = &init.hostname {
        result.hostname = Some(process_hostname_for_init(hostname, r#type)?);
    }

    // 16. Let resultProtocolString be result["protocol"] if it exists; otherwise the empty string.
    let result_protocol_string = result.protocol.clone().unwrap_or_default();

    // 17. If init["port"] exists, then set result["port"] to the result of process port for init given init["port"], resultProtocolString, and type.
    if let Some(port) = &init.port {
        result.port = Some(process_port_for_init(port, &result_protocol_string, r#type)?);
    }

    // 18. If init["pathname"] exists:
    if let Some(init_pathname) = &init.pathname {
        // 1. Set result["pathname"] to init["pathname"].
        result.pathname = Some(init_pathname.clone());

        // 2. If the following are all true:
        //     * baseURL is not null;
        //     * baseURL does not have an opaque path; and
        //     * the result of running is an absolute pathname given result["pathname"] and type is false,
        //    then:
        if let Some(base_url) = base_url.as_ref()
            && !base_url.has_opaque_path
            && !is_an_absolute_pathname(result.pathname.as_deref().unwrap(), r#type)
        {
            // 1. Let baseURLPath be the result of running process a base URL string given the result of URL path
            //    serializing baseURL and type.
            let base_url_path = process_a_base_url_string(&base_url.serialize_path(), r#type);

            // 2. Let slash index be the index of the last U+002F (/) code point found in baseURLPath, interpreted as a
            //    sequence of code points, or null if there are no instances of the code point.
            let slash_index = base_url_path.rfind('/');

            // 3. If slash index is not null:
            if let Some(slash_index) = slash_index {
                // 1. Let new pathname be the code point substring from 0 to slash index + 1 within baseURLPath.
                let mut new_pathname = base_url_path[..slash_index + 1].to_string();

                // 2. Append result["pathname"] to the end of new pathname.
                // 3. Set result["pathname"] to new pathname.
                new_pathname.push_str(result.pathname.as_deref().unwrap());
                result.pathname = Some(new_pathname);
            }
        }

        // 3. Set result["pathname"] to the result of process pathname for init given result["pathname"], resultProtocolString, and type.
        result.pathname = Some(process_pathname_for_init(
            result.pathname.as_deref().unwrap(),
            &result_protocol_string,
            r#type,
        )?);
    }

    // 19. If init["search"] exists then set result["search"] to the result of process search for init given init["search"] and type.
    if let Some(search) = &init.search {
        result.search = Some(process_search_for_init(search, r#type));
    }

    // 20. If init["hash"] exists then set result["hash"] to the result of process hash for init given init["hash"] and type.
    if let Some(hash) = &init.hash {
        result.hash = Some(process_hash_for_init(hash, r#type));
    }

    // 21. Return result.
    Ok(result)
}
