/*
 * Copyright (c) 2025-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::pattern::Component;
use crate::pattern::ConstructorStringParser;
use crate::pattern::Init;
use crate::pattern::PatternErrorOr;
use crate::pattern::PatternProcessType;
use crate::pattern::canonicalize_a_hash;
use crate::pattern::canonicalize_a_hostname;
use crate::pattern::canonicalize_a_password;
use crate::pattern::canonicalize_a_pathname;
use crate::pattern::canonicalize_a_port;
use crate::pattern::canonicalize_a_protocol;
use crate::pattern::canonicalize_a_search;
use crate::pattern::canonicalize_a_username;
use crate::pattern::canonicalize_an_ipv6_hostname;
use crate::pattern::canonicalize_an_opaque_pathname;
use crate::pattern::process_a_url_pattern_init;
use crate::pattern::protocol_component_matches_a_special_scheme;
use crate::url::BasicParseOptions;
use crate::url::ExcludeFragment;
use crate::url::Url;
use crate::url::basic_parse;
use crate::url::default_port_for_scheme;
use crate::url::is_special_scheme;

// https://urlpattern.spec.whatwg.org/#typedefdef-urlpatterninput
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Input {
    String(String),
    Init(Init),
}

#[derive(Clone, Debug)]
pub enum MatchInput {
    String(String),
    Init(Init),
    Url(Url),
}

// https://urlpattern.spec.whatwg.org/#dictdef-urlpatternresult
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct Result {
    pub inputs: Vec<Input>,

    pub protocol: crate::pattern::component::Result,
    pub username: crate::pattern::component::Result,
    pub password: crate::pattern::component::Result,
    pub hostname: crate::pattern::component::Result,
    pub port: crate::pattern::component::Result,
    pub pathname: crate::pattern::component::Result,
    pub search: crate::pattern::component::Result,
    pub hash: crate::pattern::component::Result,
}

// https://urlpattern.spec.whatwg.org/#dictdef-urlpatternoptions
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum IgnoreCase {
    Yes,
    No,
}

// https://urlpattern.spec.whatwg.org/#url-pattern
#[derive(Clone, Debug, Default)]
pub struct Pattern {
    // https://urlpattern.spec.whatwg.org/#url-pattern-protocol-component
    // protocol component, a component
    protocol_component: Component,

    // https://urlpattern.spec.whatwg.org/#url-pattern-username-component
    // username component, a component
    username_component: Component,

    // https://urlpattern.spec.whatwg.org/#url-pattern-password-component
    // password component, a component
    password_component: Component,

    // https://urlpattern.spec.whatwg.org/#url-pattern-hostname-component
    // hostname component, a component
    hostname_component: Component,

    // https://urlpattern.spec.whatwg.org/#url-pattern-port-component
    // port component, a component
    port_component: Component,

    // https://urlpattern.spec.whatwg.org/#url-pattern-pathname-component
    // pathname component, a component
    pathname_component: Component,

    // https://urlpattern.spec.whatwg.org/#url-pattern-search-component
    // search component, a component
    search_component: Component,

    // https://urlpattern.spec.whatwg.org/#url-pattern-hash-component
    // hash component, a component
    hash_component: Component,
}

// https://urlpattern.spec.whatwg.org/#hostname-pattern-is-an-ipv6-address
fn hostname_pattern_is_an_ipv6_address(input: &str) -> bool {
    // 1. If input’s code point length is less than 2, then return false.
    if input.len() < 2 {
        return false;
    }

    // 2. Let input code points be input interpreted as a list of code points.
    let input_code_points = input.as_bytes();

    // 3. If input code points[0] is U+005B ([), then return true.
    if input_code_points[0] == b'[' {
        return true;
    }

    // 4. If input code points[0] is U+007B ({) and input code points[1] is U+005B ([), then return true.
    if input_code_points[0] == b'{' && input_code_points[1] == b'[' {
        return true;
    }

    // 5. If input code points[0] is U+005C (\) and input code points[1] is U+005B ([), then return true.
    if input_code_points[0] == b'\\' && input_code_points[1] == b'[' {
        return true;
    }

    // 6. Return false.
    false
}

impl Pattern {
    // https://urlpattern.spec.whatwg.org/#url-pattern-create
    #[allow(clippy::field_reassign_with_default)]
    pub fn create(input: &Input, base_url: &Option<String>, ignore_case: IgnoreCase) -> PatternErrorOr<Self> {
        // 1. Let init be null.
        let mut init;

        // 2. If input is a scalar value string then:
        if let Input::String(input_string) = input {
            // 1. Set init to the result of running parse a constructor string given input.
            init = ConstructorStringParser::parse(input_string)?;

            // 2. If baseURL is null and init["protocol"] does not exist, then throw a TypeError.
            if base_url.is_none() && init.protocol.is_none() {
                return Err(crate::pattern::ErrorInfo::new(
                    "Relative URLPattern constructor must provide one of baseURL or protocol",
                ));
            }

            // 3. If baseURL is not null, set init["baseURL"] to baseURL.
            if let Some(base_url) = base_url {
                init.base_url = Some(base_url.clone());
            }
        }
        // 3. Otherwise:
        else {
            // 1. Assert: input is a URLPatternInit.
            let Input::Init(input_init) = input else { unreachable!() };

            // 2. If baseURL is not null, then throw a TypeError.
            if base_url.is_some() {
                return Err(crate::pattern::ErrorInfo::new(
                    "Constructor with URLPatternInit should provide no baseURL",
                ));
            }

            // 3. Set init to input.
            init = input_init.clone();
        }

        // 4. Let processedInit be the result of process a URLPatternInit given init, "pattern", null, null, null, null, null, null, null, and null.
        let none = None;
        let mut processed_init = process_a_url_pattern_init(
            &init,
            PatternProcessType::Pattern,
            &none,
            &none,
            &none,
            &none,
            &none,
            &none,
            &none,
            &none,
        )?;

        // 5. For each componentName of « "protocol", "username", "password", "hostname", "port", "pathname", "search", "hash" »:
        //     1. If processedInit[componentName] does not exist, then set processedInit[componentName] to "*".
        if processed_init.protocol.is_none() {
            processed_init.protocol = Some("*".to_string());
        }
        if processed_init.username.is_none() {
            processed_init.username = Some("*".to_string());
        }
        if processed_init.password.is_none() {
            processed_init.password = Some("*".to_string());
        }
        if processed_init.hostname.is_none() {
            processed_init.hostname = Some("*".to_string());
        }
        if processed_init.port.is_none() {
            processed_init.port = Some("*".to_string());
        }
        if processed_init.pathname.is_none() {
            processed_init.pathname = Some("*".to_string());
        }
        if processed_init.search.is_none() {
            processed_init.search = Some("*".to_string());
        }
        if processed_init.hash.is_none() {
            processed_init.hash = Some("*".to_string());
        }

        // 6. If processedInit["protocol"] is a special scheme and processedInit["port"] is a string which represents its
        //    corresponding default port in radix-10 using ASCII digits then set processedInit["port"] to the empty string.
        if is_special_scheme(processed_init.protocol.as_ref().unwrap().as_bytes())
            && let Ok(maybe_port) = processed_init.port.as_ref().unwrap().parse::<u16>()
            && Some(maybe_port) == default_port_for_scheme(processed_init.protocol.as_ref().unwrap())
        {
            processed_init.port = Some(String::new());
        }

        // 7. Let urlPattern be a new URL pattern.
        let mut url_pattern = Self::default();

        // 8. Set urlPattern’s protocol component to the result of compiling a component given processedInit["protocol"],
        //    canonicalize a protocol, and default options.
        url_pattern.protocol_component = Component::compile(
            processed_init.protocol.as_deref().unwrap(),
            Box::new(canonicalize_a_protocol),
            &crate::pattern::Options::default_(),
        )?;

        // 9. Set urlPattern’s username component to the result of compiling a component given processedInit["username"],
        //    canonicalize a username, and default options.
        url_pattern.username_component = Component::compile(
            processed_init.username.as_deref().unwrap(),
            Box::new(|value| Ok(canonicalize_a_username(value))),
            &crate::pattern::Options::default_(),
        )?;

        // 10. Set urlPattern’s password component to the result of compiling a component given processedInit["password"],
        //     canonicalize a password, and default options.
        url_pattern.password_component = Component::compile(
            processed_init.password.as_deref().unwrap(),
            Box::new(|value| Ok(canonicalize_a_password(value))),
            &crate::pattern::Options::default_(),
        )?;

        // 11. If the result running hostname pattern is an IPv6 address given processedInit["hostname"] is true, then set
        //     urlPattern’s hostname component to the result of compiling a component given processedInit["hostname"],
        //     canonicalize an IPv6 hostname, and hostname options.
        if hostname_pattern_is_an_ipv6_address(processed_init.hostname.as_deref().unwrap()) {
            url_pattern.hostname_component = Component::compile(
                processed_init.hostname.as_deref().unwrap(),
                Box::new(canonicalize_an_ipv6_hostname),
                &crate::pattern::Options::hostname(),
            )?;
        }
        // 12. Otherwise, set urlPattern’s hostname component to the result of compiling a component given
        //     processedInit["hostname"], canonicalize a hostname, and hostname options.
        else {
            url_pattern.hostname_component = Component::compile(
                processed_init.hostname.as_deref().unwrap(),
                Box::new(canonicalize_a_hostname),
                &crate::pattern::Options::hostname(),
            )?;
        }

        // 13. Set urlPattern’s port component to the result of compiling a component given processedInit["port"],
        //     canonicalize a port, and default options.
        url_pattern.port_component = Component::compile(
            processed_init.port.as_deref().unwrap(),
            Box::new(|value| canonicalize_a_port(value, &None)),
            &crate::pattern::Options::default_(),
        )?;

        // 14. Let compileOptions be a copy of the default options with the ignore case property set to options["ignoreCase"].
        let mut compile_options = crate::pattern::Options::default_();
        compile_options.ignore_case = ignore_case == IgnoreCase::Yes;

        // 15. If the result of running protocol component matches a special scheme given urlPattern’s protocol component is true, then:
        if protocol_component_matches_a_special_scheme(&url_pattern.protocol_component) {
            // 1. Let pathCompileOptions be copy of the pathname options with the ignore case property set to options["ignoreCase"].
            let mut path_compile_options = crate::pattern::Options::pathname();
            path_compile_options.ignore_case = ignore_case == IgnoreCase::Yes;

            // 2. Set urlPattern’s pathname component to the result of compiling a component given processedInit["pathname"],
            //    canonicalize a pathname, and pathCompileOptions.
            url_pattern.pathname_component = Component::compile(
                processed_init.pathname.as_deref().unwrap(),
                Box::new(|value| Ok(canonicalize_a_pathname(value))),
                &path_compile_options,
            )?;
        }
        // 16. Otherwise set urlPattern’s pathname component to the result of compiling a component given
        //     processedInit["pathname"], canonicalize an opaque pathname, and compileOptions.
        else {
            url_pattern.pathname_component = Component::compile(
                processed_init.pathname.as_deref().unwrap(),
                Box::new(canonicalize_an_opaque_pathname),
                &compile_options,
            )?;
        }

        // 17. Set urlPattern’s search component to the result of compiling a component given processedInit["search"],
        //     canonicalize a search, and compileOptions.
        url_pattern.search_component = Component::compile(
            processed_init.search.as_deref().unwrap(),
            Box::new(|value| Ok(canonicalize_a_search(value))),
            &compile_options,
        )?;

        // 18. Set urlPattern’s hash component to the result of compiling a component given processedInit["hash"],
        //     canonicalize a hash, and compileOptions.
        url_pattern.hash_component = Component::compile(
            processed_init.hash.as_deref().unwrap(),
            Box::new(|value| Ok(canonicalize_a_hash(value))),
            &compile_options,
        )?;

        // 19. Return urlPattern.
        Ok(url_pattern)
    }

    // https://urlpattern.spec.whatwg.org/#url-pattern-match
    #[allow(clippy::field_reassign_with_default)]
    pub fn r#match(&self, input: &MatchInput, base_url_string: &Option<String>) -> PatternErrorOr<Option<Result>> {
        // 1. Let protocol be the empty string.
        let mut protocol = String::new();

        // 2. Let username be the empty string.
        let mut username = String::new();

        // 3. Let password be the empty string.
        let mut password = String::new();

        // 4. Let hostname be the empty string.
        let mut hostname = String::new();

        // 5. Let port be the empty string.
        let mut port = String::new();

        // 6. Let pathname be the empty string.
        let mut pathname = String::new();

        // 7. Let search be the empty string.
        let mut search = String::new();

        // 8. Let hash be the empty string.
        let mut hash = String::new();

        // 9. Let inputs be an empty list.
        let mut inputs = Vec::new();

        // 10. If input is a URL, then append the serialization of input to inputs.
        if let MatchInput::Url(input_url) = input {
            inputs.push(Input::String(input_url.serialize(ExcludeFragment::No)));
        }
        // 11. Otherwise, append input to inputs.
        else {
            match input {
                MatchInput::String(input_string) => {
                    inputs.push(Input::String(input_string.clone()));
                }
                MatchInput::Init(input_init) => inputs.push(Input::Init(input_init.clone())),
                MatchInput::Url(_) => unreachable!(),
            }
        }

        // 12. If input is a URLPatternInit then:
        if let MatchInput::Init(input_init) = input {
            // 1. If baseURLString was given, throw a TypeError.
            if base_url_string.is_some() {
                return Err(crate::pattern::ErrorInfo::new(
                    "Base URL cannot be provided when URLPatternInput is provided",
                ));
            }

            // 2. Let applyResult be the result of process a URLPatternInit given input, "url", protocol, username, password,
            //    hostname, port, pathname, search, and hash. If this throws an exception, catch it, and return null.
            let protocol_option = Some(protocol.clone());
            let username_option = Some(username.clone());
            let password_option = Some(password.clone());
            let hostname_option = Some(hostname.clone());
            let port_option = Some(port.clone());
            let pathname_option = Some(pathname.clone());
            let search_option = Some(search.clone());
            let hash_option = Some(hash.clone());
            let apply_result = process_a_url_pattern_init(
                input_init,
                PatternProcessType::Url,
                &protocol_option,
                &username_option,
                &password_option,
                &hostname_option,
                &port_option,
                &pathname_option,
                &search_option,
                &hash_option,
            );
            let Ok(apply_result) = apply_result else {
                return Ok(None);
            };

            // 3. Set protocol to applyResult["protocol"].
            protocol = apply_result.protocol.unwrap();

            // 4. Set username to applyResult["username"].
            username = apply_result.username.unwrap();

            // 5. Set password to applyResult["password"].
            password = apply_result.password.unwrap();

            // 6. Set hostname to applyResult["hostname"].
            hostname = apply_result.hostname.unwrap();

            // 7. Set port to applyResult["port"].
            port = apply_result.port.unwrap();

            // 8. Set pathname to applyResult["pathname"].
            pathname = apply_result.pathname.unwrap();

            // 9. Set search to applyResult["search"].
            search = apply_result.search.unwrap();

            // 10. Set hash to applyResult["hash"].
            hash = apply_result.hash.unwrap();
        }
        // 13. Otherwise:
        else {
            // 1. Let url be input.
            let url;

            // 2. If input is a USVString:
            if let MatchInput::String(input_string) = input {
                // 1. Let baseURL be null.
                let mut base_url = None;

                // 2. If baseURLString was given, then:
                if let Some(base_url_string) = base_url_string {
                    // 1. Set baseURL to the result of running the basic URL parser on baseURLString.
                    base_url = basic_parse(base_url_string, BasicParseOptions::new());

                    // 2. If baseURL is failure, return null.
                    if base_url.is_none() {
                        return Ok(None);
                    }

                    // 3. Append baseURLString to inputs.
                    inputs.push(Input::String(base_url_string.clone()));
                }

                // 3. Set url to the result of running the basic URL parser on input with baseURL.
                // 4. If url is failure, return null.
                let maybe_url = if let Some(base_url) = base_url.as_ref() {
                    basic_parse(input_string, BasicParseOptions::new().base_url(base_url))
                } else {
                    basic_parse(input_string, BasicParseOptions::new())
                };
                let Some(parsed_url) = maybe_url else {
                    return Ok(None);
                };
                url = parsed_url;
            } else {
                // 3. Assert: url is a URL.
                let MatchInput::Url(input_url) = input else {
                    unreachable!()
                };
                url = input_url.clone();
            }

            // 4. Set protocol to url’s scheme.
            protocol = url.scheme.clone();

            // 5. Set username to url’s username.
            username = url.username.clone();

            // 6. Set password to url’s password.
            password = url.password.clone();

            // 7. Set hostname to url’s host, serialized, or the empty string if the value is null.
            if let Some(host) = &url.host {
                hostname = host.serialize();
            } else {
                hostname = String::new();
            }

            // 8. Set port to url’s port, serialized, or the empty string if the value is null.
            if let Some(url_port) = url.port {
                port = url_port.to_string();
            } else {
                port = String::new();
            }

            // 9. Set pathname to the result of URL path serializing url.
            pathname = url.serialize_path();

            // 10. Set search to url’s query or the empty string if the value is null.
            search = url.query.unwrap_or_default();

            // 11. Set hash to url’s fragment or the empty string if the value is null.
            hash = url.fragment.unwrap_or_default();
        }

        // 14. Let protocolExecResult be RegExpBuiltinExec(urlPattern’s protocol component's regular expression, protocol).
        let protocol_exec_result = self.protocol_component.execute(&protocol);
        if !protocol_exec_result.success {
            return Ok(None);
        }

        // 15. Let usernameExecResult be RegExpBuiltinExec(urlPattern’s username component's regular expression, username).
        let username_exec_result = self.username_component.execute(&username);
        if !username_exec_result.success {
            return Ok(None);
        }

        // 16. Let passwordExecResult be RegExpBuiltinExec(urlPattern’s password component's regular expression, password).
        let password_exec_result = self.password_component.execute(&password);
        if !password_exec_result.success {
            return Ok(None);
        }

        // 17. Let hostnameExecResult be RegExpBuiltinExec(urlPattern’s hostname component's regular expression, hostname).
        let hostname_exec_result = self.hostname_component.execute(&hostname);
        if !hostname_exec_result.success {
            return Ok(None);
        }

        // 18. Let portExecResult be RegExpBuiltinExec(urlPattern’s port component's regular expression, port).
        let port_exec_result = self.port_component.execute(&port);
        if !port_exec_result.success {
            return Ok(None);
        }

        // 19. Let pathnameExecResult be RegExpBuiltinExec(urlPattern’s pathname component's regular expression, pathname).
        let pathname_exec_result = self.pathname_component.execute(&pathname);
        if !pathname_exec_result.success {
            return Ok(None);
        }

        // 20. Let searchExecResult be RegExpBuiltinExec(urlPattern’s search component's regular expression, search).
        let search_exec_result = self.search_component.execute(&search);
        if !search_exec_result.success {
            return Ok(None);
        }

        // 21. Let hashExecResult be RegExpBuiltinExec(urlPattern’s hash component's regular expression, hash).
        let hash_exec_result = self.hash_component.execute(&hash);
        if !hash_exec_result.success {
            return Ok(None);
        }

        // 22. If protocolExecResult, usernameExecResult, passwordExecResult, hostnameExecResult, portExecResult,
        //     pathnameExecResult, searchExecResult, or hashExecResult are null then return null.
        // NOTE: Done in steps above at point of exec.

        // 23. Let result be a new URLPatternResult.
        let mut result = Result::default();

        // 24. Set result["inputs"] to inputs.
        result.inputs = inputs;

        // 25. Set result["protocol"] to the result of creating a component match result given urlPattern’s protocol
        //     component, protocol, and protocolExecResult.
        result.protocol = self
            .protocol_component
            .create_match_result(&protocol, &protocol_exec_result);

        // 26. Set result["username"] to the result of creating a component match result given urlPattern’s username
        //     component, username, and usernameExecResult.
        result.username = self
            .username_component
            .create_match_result(&username, &username_exec_result);

        // 27. Set result["password"] to the result of creating a component match result given urlPattern’s password
        //     component, password, and passwordExecResult.
        result.password = self
            .password_component
            .create_match_result(&password, &password_exec_result);

        // 28. Set result["hostname"] to the result of creating a component match result given urlPattern’s hostname
        //     component, hostname, and hostnameExecResult.
        result.hostname = self
            .hostname_component
            .create_match_result(&hostname, &hostname_exec_result);

        // 29. Set result["port"] to the result of creating a component match result given urlPattern’s port component,
        //     port, and portExecResult.
        result.port = self.port_component.create_match_result(&port, &port_exec_result);

        // 30. Set result["pathname"] to the result of creating a component match result given urlPattern’s pathname
        //     component, pathname, and pathnameExecResult.
        result.pathname = self
            .pathname_component
            .create_match_result(&pathname, &pathname_exec_result);

        // 31. Set result["search"] to the result of creating a component match result given urlPattern’s search component,
        //     search, and searchExecResult.
        result.search = self.search_component.create_match_result(&search, &search_exec_result);

        // 32. Set result["hash"] to the result of creating a component match result given urlPattern’s hash component,
        //     hash, and hashExecResult.
        result.hash = self.hash_component.create_match_result(&hash, &hash_exec_result);

        // 33. Return result.
        Ok(Some(result))
    }

    pub fn has_regexp_groups(&self) -> bool {
        // 1. If urlPattern’s protocol component has regexp groups is true, then return true.
        if self.protocol_component.has_regexp_groups {
            return true;
        }

        // 2. If urlPattern’s username component has regexp groups is true, then return true.
        if self.username_component.has_regexp_groups {
            return true;
        }

        // 3. If urlPattern’s password component has regexp groups is true, then return true.
        if self.password_component.has_regexp_groups {
            return true;
        }

        // 4. If urlPattern’s hostname component has regexp groups is true, then return true.
        if self.hostname_component.has_regexp_groups {
            return true;
        }

        // 5. If urlPattern’s port component has regexp groups is true, then return true.
        if self.port_component.has_regexp_groups {
            return true;
        }

        // 6. If urlPattern’s pathname component has regexp groups is true, then return true.
        if self.pathname_component.has_regexp_groups {
            return true;
        }

        // 7. If urlPattern’s search component has regexp groups is true, then return true.
        if self.search_component.has_regexp_groups {
            return true;
        }

        // 8. If urlPattern’s hash component has regexp groups is true, then return true.
        if self.hash_component.has_regexp_groups {
            return true;
        }

        // 9. Return false.
        false
    }

    pub fn protocol_component(&self) -> &Component {
        &self.protocol_component
    }

    pub fn username_component(&self) -> &Component {
        &self.username_component
    }

    pub fn password_component(&self) -> &Component {
        &self.password_component
    }

    pub fn hostname_component(&self) -> &Component {
        &self.hostname_component
    }

    pub fn port_component(&self) -> &Component {
        &self.port_component
    }

    pub fn pathname_component(&self) -> &Component {
        &self.pathname_component
    }

    pub fn search_component(&self) -> &Component {
        &self.search_component
    }

    pub fn hash_component(&self) -> &Component {
        &self.hash_component
    }
}
