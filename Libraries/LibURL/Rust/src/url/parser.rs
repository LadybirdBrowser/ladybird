/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::borrow::Cow;

use super::BasicParseOptions;
use super::Host;
use super::State;
use super::Url;
use super::default_port_for_scheme;
use super::host::parse_host;
use super::is_special_scheme;
use super::percent_encoding::PercentEncodeSet;
use super::percent_encoding::append_percent_encoded_if_necessary;
use super::percent_encoding::percent_encode;
use super::percent_encoding::percent_encode_after_encoding;
use crate::textcodec::get_output_encoding;

pub(super) fn starts_with_two_ascii_hex_digits(remaining: &str) -> bool {
    let bytes = remaining.as_bytes();
    bytes.len() >= 3 && bytes[1].is_ascii_hexdigit() && bytes[2].is_ascii_hexdigit()
}

fn contains_ascii_tab_or_newline(input: &str) -> bool {
    input.chars().any(|code_point| matches!(code_point, '\t' | '\n' | '\r'))
}

fn strip_ascii_tab_or_newline(input: &str) -> Cow<'_, str> {
    if !contains_ascii_tab_or_newline(input) {
        return Cow::Borrowed(input);
    }

    let mut output = String::with_capacity(input.len());
    for code_point in input.chars() {
        if !matches!(code_point, '\t' | '\n' | '\r') {
            output.push(code_point);
        }
    }
    Cow::Owned(output)
}

pub(super) fn preprocess_input(input: &str, url_is_given: bool) -> Cow<'_, str> {
    let trimmed_input = if url_is_given {
        input
    } else {
        input.trim_matches(is_ascii_c0_control_or_space)
    };

    strip_ascii_tab_or_newline(trimmed_input)
}

// https://url.spec.whatwg.org/#validation-error
pub(super) fn report_validation_error(state: State, pointer: usize, code_point: Option<char>, message: &str) {
    #[cfg(not(feature = "debug-validation-errors"))]
    let _ = (state, pointer, code_point, message);

    #[cfg(feature = "debug-validation-errors")]
    {
        let rendered_code_point = code_point
            .map(|code_point| format!("{code_point:?}"))
            .unwrap_or_else(|| "EOF".to_string());
        eprintln!(
            "URL validation error: state={state:?} pointer={pointer} code_point={rendered_code_point} message={message}"
        );
    }
}

// https://infra.spec.whatwg.org/#c0-control-or-space
pub(super) fn is_ascii_c0_control_or_space(code_point: char) -> bool {
    code_point.is_ascii_control() || code_point == ' '
}

// https://url.spec.whatwg.org/#url-code-points
pub(super) fn is_url_code_point(code_point: char) -> bool {
    // The URL code points are ASCII alphanumeric, U+0021 (!), U+0024 ($), U+0026 (&),
    // U+0027 ('), U+0028 LEFT PARENTHESIS, U+0029 RIGHT PARENTHESIS, U+002A (*),
    // U+002B (+), U+002C (,), U+002D (-), U+002E (.), U+002F (/), U+003A (:),
    // U+003B (;), U+003D (=), U+003F (?), U+0040 (@), U+005F (_), U+007E (~), and code
    // points in the range U+00A0 to U+10FFFD, inclusive, excluding surrogates and
    // noncharacters.
    code_point.is_ascii() && is_ascii_url_code_point_byte(code_point as u8)
}

#[inline]
fn is_ascii_url_code_point_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric()
        || matches!(
            byte,
            b'!' | b'$'
                | b'&'
                | b'\''
                | b'('
                | b')'
                | b'*'
                | b'+'
                | b','
                | b'-'
                | b'.'
                | b'/'
                | b':'
                | b';'
                | b'='
                | b'?'
                | b'@'
                | b'_'
                | b'~'
        )
}

#[inline]
fn is_ascii_path_state_copyable_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric()
        || matches!(
            byte,
            b'!' | b'$'
                | b'&'
                | b'\''
                | b'('
                | b')'
                | b'*'
                | b'+'
                | b','
                | b'-'
                | b'.'
                | b':'
                | b';'
                | b'='
                | b'@'
                | b'_'
                | b'~'
        )
}

#[inline]
fn is_ascii_query_state_copyable_byte(byte: u8) -> bool {
    is_ascii_url_code_point_byte(byte)
}

#[inline]
fn ascii_copyable_state_prefix_length(input: &str, is_copyable_byte: impl Fn(u8) -> bool) -> usize {
    input
        .as_bytes()
        .iter()
        .position(|&byte| !is_copyable_byte(byte))
        .unwrap_or(input.len())
}

// https://url.spec.whatwg.org/#single-dot-path-segment
pub(super) fn is_single_dot_path_segment(input: &[u8]) -> bool {
    // A single-dot URL path segment is a URL path segment that is "." or an ASCII case-insensitive match for "%2e".
    input == b"." || input.eq_ignore_ascii_case(b"%2e")
}

// https://url.spec.whatwg.org/#double-dot-path-segment
pub(super) fn is_double_dot_path_segment(input: &[u8]) -> bool {
    // A double-dot URL path segment is a URL path segment that is ".." or an ASCII case-insensitive match for
    // ".%2e", "%2e.", or "%2e%2e".
    input == b".."
        || input.eq_ignore_ascii_case(b".%2e")
        || input.eq_ignore_ascii_case(b"%2e.")
        || input.eq_ignore_ascii_case(b"%2e%2e")
}

// https://url.spec.whatwg.org/#start-with-a-windows-drive-letter
pub(super) fn starts_with_windows_drive_letter(input: &str) -> bool {
    // A string starts with a Windows drive letter if all of the following are true:

    // * its length is greater than or equal to 2
    let mut code_points = input.chars();
    let Some(first) = code_points.next() else {
        return false;
    };
    let Some(second) = code_points.next() else {
        return false;
    };

    // * its first two code points are a Windows drive letter
    if !first.is_ascii_alphabetic() || !matches!(second, ':' | '|') {
        return false;
    }

    // * its length is 2 or its third code point is U+002F (/), U+005C (\), U+003F (?), or U+0023 (#).
    match code_points.next() {
        None => true,
        Some(third) => matches!(third, '/' | '\\' | '?' | '#'),
    }
}

// https://url.spec.whatwg.org/#include-credentials
pub(super) fn url_includes_credentials(url: &Url) -> bool {
    // A URL includes credentials if its username or password is not the empty string.
    !url.username.is_empty() || !url.password.is_empty()
}

// https://url.spec.whatwg.org/#windows-drive-letter
pub(super) fn is_windows_drive_letter(input: &str) -> bool {
    // A Windows drive letter is two code points, of which the first is an ASCII alpha and the second is either U+003A (:) or U+007C (|).
    let mut code_points = input.chars();
    matches!(
        (code_points.next(), code_points.next(), code_points.next()),
        (Some(first), Some(':' | '|'), None) if first.is_ascii_alphabetic()
    )
}

// https://url.spec.whatwg.org/#normalized-windows-drive-letter
pub(super) fn is_normalized_windows_drive_letter(input: &str) -> bool {
    // A normalized Windows drive letter is a Windows drive letter of which the second code point is U+003A (:).
    let bytes = input.as_bytes();
    bytes.len() == 2 && bytes[0].is_ascii_alphabetic() && bytes[1] == b':'
}

// https://url.spec.whatwg.org/#shorten-a-urls-path
pub(super) fn shorten_urls_path(url: &mut Url) {
    // 1. Assert: url does not have an opaque path.
    assert!(!url.has_opaque_path);

    // 2. Let path be url’s path.
    let path = &mut url.path;

    // 3. If url’s scheme is "file", path’s size is 1, and path[0] is a normalized Windows drive letter, then return.
    if url.scheme == "file" && path.len() == 1 && is_normalized_windows_drive_letter(&path[0]) {
        return;
    }

    // 4. Remove path’s last item, if any.
    if !path.is_empty() {
        path.pop();
    }
}

pub(super) fn cloned_shortened_path(base_url: &Url) -> Vec<String> {
    if base_url.scheme == "file" && base_url.path.len() == 1 && is_normalized_windows_drive_letter(&base_url.path[0]) {
        return base_url.path.clone();
    }

    if base_url.path.is_empty() {
        return Vec::new();
    }

    base_url.path[..base_url.path.len() - 1].to_vec()
}

// https://url.spec.whatwg.org/#concept-basic-url-parser
pub(crate) fn basic_parse_into(
    input: &str,
    url: &mut Url,
    options: &BasicParseOptions<'_>,
    url_is_given: bool,
) -> bool {
    // 1. If url is not given:
    if !url_is_given {
        // 1. Set url to a new URL.
        // NB: The Rust entry points choose whether a fresh Url is allocated or an existing Url is passed in. By the
        //     time we reach this function, `url` is always available as the output record.

        // 2. If input contains any leading or trailing C0 control or space, invalid-URL-unit validation error.
        // 3. Remove any leading and trailing C0 control or space from input.
        let trimmed_input = input.trim_matches(is_ascii_c0_control_or_space);
        if trimmed_input.len() != input.len() {
            report_validation_error(State::SchemeStart, 0, input.chars().next(), "invalid-URL-unit");
        }
    }

    // 2. If input contains any ASCII tab or newline, invalid-URL-unit validation error.
    // 3. Remove all ASCII tab or newline from input.
    let processed_input = preprocess_input(input, url_is_given);
    let processed_input = processed_input.as_ref();

    // 4. Let state be state override if given, or scheme start state otherwise.
    let mut state = options.state_override.unwrap_or(State::SchemeStart);

    // 5. Set encoding to the result of getting an output encoding from encoding.
    let mut encoding = options.encoding.map(get_output_encoding).unwrap_or("utf-8");

    // 6. Let buffer be the empty string.
    let mut buffer = String::new();

    // 7. Let atSignSeen, insideBrackets, and passwordTokenSeen be false.
    let mut inside_brackets = false;
    let mut at_sign_seen = false;
    let mut password_token_seen = false;
    let mut username_builder = String::new();
    let mut password_builder = String::new();

    // 8. Let pointer be a pointer for input.
    let mut pointer = 0usize;

    // 9. Keep running the following state machine by switching on state.
    //    If after a run pointer points to the EOF code point, go to the next step.
    //    Otherwise, increase pointer by 1 and continue with the state machine.
    loop {
        let remaining = if pointer < processed_input.len() {
            &processed_input[pointer..]
        } else {
            ""
        };
        let code_point = remaining.chars().next();
        let remaining_after_code_point = code_point
            .map(|code_point| &remaining[code_point.len_utf8()..])
            .unwrap_or("");

        match state {
            // -> scheme start state, https://url.spec.whatwg.org/#scheme-start-state
            State::SchemeStart => {
                // 1. If c is an ASCII alpha, append c, lowercased, to buffer, and set state to scheme state.
                if let Some(code_point) = code_point.filter(|code_point| code_point.is_ascii_alphabetic()) {
                    buffer.push(code_point.to_ascii_lowercase());
                    state = State::Scheme;
                }
                // 2. Otherwise, if state override is not given, set state to no scheme state and decrease pointer by 1.
                else if options.state_override.is_none() {
                    state = State::NoScheme;
                    continue;
                }
                // 3. Otherwise, return failure.
                else {
                    return false;
                }
            }
            // -> scheme state, https://url.spec.whatwg.org/#scheme-state
            State::Scheme => {
                // 1. If c is an ASCII alphanumeric, U+002B (+), U+002D (-), or U+002E (.), append c, lowercased, to buffer.
                if let Some(code_point) = code_point
                    .filter(|code_point| code_point.is_ascii_alphanumeric() || matches!(code_point, '+' | '-' | '.'))
                {
                    buffer.push(code_point.to_ascii_lowercase());
                }
                // 2. Otherwise, if c is U+003A (:), then:
                else if code_point == Some(':') {
                    // 1. If state override is given, then:
                    if options.state_override.is_some() {
                        // 1. If url’s scheme is a special scheme and buffer is not a special scheme, then return.
                        if url.is_special() && !is_special_scheme(buffer.as_bytes()) {
                            return true;
                        }

                        // 2. If url’s scheme is not a special scheme and buffer is a special scheme, then return.
                        if !url.is_special() && is_special_scheme(buffer.as_bytes()) {
                            return true;
                        }

                        // 3. If url includes credentials or has a non-null port, and buffer is "file", then return.
                        if (url_includes_credentials(url) || url.port.is_some()) && buffer == "file" {
                            return true;
                        }

                        // 4. If url’s scheme is "file" and its host is an empty host, then return.
                        if url.scheme == "file" && url.host.as_ref().is_some_and(Host::is_empty_host) {
                            return true;
                        }
                    }

                    // 2. Set url’s scheme to buffer.
                    url.scheme = std::mem::take(&mut buffer);

                    // 3. If state override is given, then:
                    if options.state_override.is_some() {
                        // 1. If url’s port is url’s scheme’s default port, then set url’s port to null.
                        if url.port == default_port_for_scheme(&url.scheme) {
                            url.port = None;
                        }

                        // 2. Return.
                        return true;
                    }

                    // 4. Set buffer to the empty string.
                    buffer.clear();

                    // 5. If url’s scheme is "file", then:
                    if url.scheme == "file" {
                        // 1. If remaining does not start with "//", special-scheme-missing-following-solidus validation error.
                        if !remaining_after_code_point.starts_with("//") {
                            report_validation_error(
                                State::Scheme,
                                pointer,
                                code_point,
                                "special-scheme-missing-following-solidus",
                            );
                        }

                        // 2. Set state to file state.
                        state = State::File;
                    }
                    // 6. Otherwise, if url is special, base is non-null, and base’s scheme is url’s scheme:
                    else if url.is_special() && options.base_url.is_some_and(|base_url| base_url.scheme == url.scheme)
                    {
                        // 1. Assert: base is special (and therefore does not have an opaque path).
                        assert!(options.base_url.as_ref().unwrap().is_special());

                        // 2. Set state to special relative or authority state.
                        state = State::SpecialRelativeOrAuthority;
                    }
                    // 7. Otherwise, if url is special, set state to special authority slashes state.
                    else if url.is_special() {
                        state = State::SpecialAuthoritySlashes;
                    }
                    // 8. Otherwise, if remaining starts with an U+002F (/), set state to path or authority state and
                    //    increase pointer by 1.
                    else if remaining_after_code_point.starts_with('/') {
                        state = State::PathOrAuthority;
                        pointer += 1;
                    }
                    // 9. Otherwise, set url’s path to the empty string and set state to opaque path state.
                    else {
                        url.path.clear();
                        url.has_opaque_path = true;
                        state = State::OpaquePath;
                    }
                }
                // 3. Otherwise, if state override is not given, set buffer to the empty string, state to no scheme state,
                //    and start over (from the first code point in input).
                else if options.state_override.is_none() {
                    buffer.clear();
                    state = State::NoScheme;
                    pointer = 0;
                    continue;
                }
                // 4. Otherwise, return failure.
                else {
                    return false;
                }
            }
            // -> no scheme state, https://url.spec.whatwg.org/#no-scheme-state
            State::NoScheme => {
                // 1. If base is null...
                let Some(base_url) = options.base_url else {
                    return false;
                };

                // ... or base has an opaque path and c is not U+0023 (#), missing-scheme-non-relative-URL
                //     validation error, return failure.
                if base_url.has_opaque_path && code_point != Some('#') {
                    return false;
                }
                // 2. Otherwise, if base has an opaque path and c is U+0023 (#), set url’s scheme to base’s scheme, url’s
                //    path to base’s path, url’s query to base’s query, url’s fragment to the empty string, and set state
                //    to fragment state.
                else if base_url.has_opaque_path && code_point == Some('#') {
                    url.scheme = base_url.scheme.clone();
                    url.path = base_url.path.clone();
                    url.query = base_url.query.clone();
                    url.fragment = Some(String::new());
                    url.has_opaque_path = true;
                    state = State::Fragment;
                }
                // 3. Otherwise, if base’s scheme is not "file", set state to relative state and decrease pointer by 1.
                else if base_url.scheme != "file" {
                    state = State::Relative;
                    continue;
                }
                // 4. Otherwise, set state to file state and decrease pointer by 1.
                else {
                    state = State::File;
                    continue;
                }
            }
            // -> special relative or authority state, https://url.spec.whatwg.org/#special-relative-or-authority-state
            State::SpecialRelativeOrAuthority => {
                // 1. If c is U+002F (/) and remaining starts with U+002F (/), then set state to special authority ignore
                //    slashes state and increase pointer by 1.
                if code_point == Some('/') && remaining.chars().nth(1) == Some('/') {
                    state = State::SpecialAuthorityIgnoreSlashes;
                    pointer += 1;
                }
                // 2. Otherwise, special-scheme-missing-following-solidus validation error, set state to relative state
                //    and decrease pointer by 1.
                else {
                    report_validation_error(
                        State::SpecialRelativeOrAuthority,
                        pointer,
                        code_point,
                        "special-scheme-missing-following-solidus",
                    );
                    state = State::Relative;
                    continue;
                }
            }
            // -> path or authority state, https://url.spec.whatwg.org/#path-or-authority-state
            State::PathOrAuthority => {
                // 1. If c is U+002F (/), then set state to authority state.
                if code_point == Some('/') {
                    state = State::Authority;
                }
                // 2. Otherwise, set state to path state, and decrease pointer by 1.
                else {
                    state = State::Path;
                    continue;
                }
            }
            // -> relative state, https://url.spec.whatwg.org/#relative-state
            State::Relative => {
                // 1. Assert: base’s scheme is not "file".
                let base_url = options.base_url.expect("relative state requires a base URL");
                assert!(base_url.scheme != "file");

                // 2. Set url’s scheme to base’s scheme.
                url.scheme = base_url.scheme.clone();

                // 3. If c is U+002F (/), then set state to relative slash state.
                if code_point == Some('/') {
                    state = State::RelativeSlash;
                }
                // 4. Otherwise, if url is special and c is U+005C (\), invalid-reverse-solidus validation error, set
                //    state to relative slash state.
                else if url.is_special() && code_point == Some('\\') {
                    report_validation_error(State::Relative, pointer, code_point, "invalid-reverse-solidus");
                    state = State::RelativeSlash;
                }
                // 5. Otherwise:
                else {
                    // 1. Set url’s username to base’s username, url’s password to base’s password, url’s host to base’s
                    //    host, url’s port to base’s port, url’s path to a clone of base’s path, and url’s query to base’s
                    //    query.
                    url.username = base_url.username.clone();
                    url.password = base_url.password.clone();
                    url.host = base_url.host.clone();
                    url.port = base_url.port;
                    url.has_opaque_path = base_url.has_opaque_path;
                    url.query = base_url.query.clone();

                    // 2. If c is U+003F (?), then set url’s query to the empty string, and state to query state.
                    if code_point == Some('?') {
                        url.path = base_url.path.clone();
                        url.query = Some(String::new());
                        state = State::Query;
                    }
                    // 3. Otherwise, if c is U+0023 (#), set url’s fragment to the empty string and state to fragment state.
                    else if code_point == Some('#') {
                        url.path = base_url.path.clone();
                        url.fragment = Some(String::new());
                        state = State::Fragment;
                    }
                    // 4. Otherwise, if c is not the EOF code point:
                    else if code_point.is_some() {
                        // 1. Set url’s query to null.
                        url.query = None;

                        // 2. Shorten url’s path.
                        url.path = cloned_shortened_path(base_url);

                        // 3. Set state to path state and decrease pointer by 1.
                        state = State::Path;
                        continue;
                    } else {
                        url.path = base_url.path.clone();
                    }
                }
            }
            // -> relative slash state, https://url.spec.whatwg.org/#relative-slash-state
            State::RelativeSlash => {
                // 1. If url is special and c is U+002F (/) or U+005C (\), then:
                let base_url = options.base_url.expect("relative slash state requires a base URL");
                if url.is_special() && matches!(code_point, Some('/' | '\\')) {
                    // 1. If c is U+005C (\), invalid-reverse-solidus validation error.
                    if code_point == Some('\\') {
                        report_validation_error(State::RelativeSlash, pointer, code_point, "invalid-reverse-solidus");
                    }

                    // 2. Set state to special authority ignore slashes state.
                    state = State::SpecialAuthorityIgnoreSlashes;
                }
                // 2. Otherwise, if c is U+002F (/), then set state to authority state.
                else if code_point == Some('/') {
                    state = State::Authority;
                }
                // 3. Otherwise, set url’s username to base’s username, url’s password to base’s password, url’s host to
                //    base’s host, url’s port to base’s port, state to path state, and then, decrease pointer by 1.
                else {
                    url.username = base_url.username.clone();
                    url.password = base_url.password.clone();
                    url.host = base_url.host.clone();
                    url.port = base_url.port;
                    state = State::Path;
                    continue;
                }
            }
            // -> special authority slashes state, https://url.spec.whatwg.org/#special-authority-slashes-state
            State::SpecialAuthoritySlashes => {
                // 1. If c is U+002F (/) and remaining starts with U+002F (/), then
                // set state to special authority ignore slashes state and increase pointer by 1.
                if code_point == Some('/') && remaining.chars().nth(1) == Some('/') {
                    state = State::SpecialAuthorityIgnoreSlashes;
                    pointer += 1;
                }
                // 2. Otherwise, special-scheme-missing-following-solidus validation error,
                // set state to special authority ignore slashes state and decrease pointer by 1.
                else {
                    report_validation_error(
                        State::SpecialAuthoritySlashes,
                        pointer,
                        code_point,
                        "special-scheme-missing-following-solidus",
                    );
                    state = State::SpecialAuthorityIgnoreSlashes;
                    continue;
                }
            }
            // -> special authority ignore slashes state, https://url.spec.whatwg.org/#special-authority-ignore-slashes-state
            State::SpecialAuthorityIgnoreSlashes => {
                // 1. If c is neither U+002F (/) nor U+005C (\), then set state to authority state and decrease pointer by 1.
                if code_point != Some('/') && code_point != Some('\\') {
                    state = State::Authority;
                    continue;
                }
                // 2. Otherwise, special-scheme-missing-following-solidus validation error.
                else {
                    report_validation_error(
                        State::SpecialAuthorityIgnoreSlashes,
                        pointer,
                        code_point,
                        "special-scheme-missing-following-solidus",
                    );
                }
            }
            // -> authority state, https://url.spec.whatwg.org/#authority-state
            State::Authority => {
                // Authority is delimited by '@/?#' and additionally '\' for special URLs
                let authority_end = remaining.char_indices().find_map(|(index, byte)| {
                    (matches!(byte, '@' | '/' | '?' | '#') || (url.is_special() && byte == '\\')).then_some(index)
                });
                let authority_length = authority_end.unwrap_or(remaining.len());
                let authority = &remaining[..authority_length];
                let delimiter_code_point = remaining[authority_length..].chars().next();

                // 1. If c is U+0040 (@), then:
                if delimiter_code_point == Some('@') {
                    // 1. Invalid-credentials validation error.
                    report_validation_error(
                        State::Authority,
                        pointer + authority_length,
                        delimiter_code_point,
                        "invalid-credentials",
                    );

                    // 2. If atSignSeen is true, then prepend "%40" to buffer.
                    if at_sign_seen {
                        if password_token_seen {
                            password_builder.push_str("%40");
                        } else {
                            username_builder.push_str("%40");
                        }
                    }

                    // 3. Set atSignSeen to true.
                    at_sign_seen = true;

                    // 4. For each codePoint in buffer:
                    //     1. If codePoint is U+003A (:) and passwordTokenSeen is false, then set passwordTokenSeen to true and continue.
                    //     2. Let encodedCodePoints be the result of running UTF-8 percent-encode codePoint using the userinfo percent-encode set.
                    //     3. If passwordTokenSeen is true, then append encodedCodePoints to url’s password.
                    //     4. Otherwise, append encodedCodePoints to url’s username.
                    if password_token_seen {
                        let encoded_authority = percent_encode(authority, PercentEncodeSet::Userinfo, false);
                        password_builder.push_str(&encoded_authority);
                    } else if let Some(password_end) = authority.find(':') {
                        password_token_seen = true;

                        let encoded_username =
                            percent_encode(&authority[..password_end], PercentEncodeSet::Userinfo, false);
                        let encoded_password =
                            percent_encode(&authority[password_end + 1..], PercentEncodeSet::Userinfo, false);

                        username_builder.push_str(&encoded_username);
                        password_builder.push_str(&encoded_password);
                    } else {
                        let encoded_authority = percent_encode(authority, PercentEncodeSet::Userinfo, false);
                        username_builder.push_str(&encoded_authority);
                    }

                    // NB: Since we have batch processed the username/password, we need to move the pointer past those code points.
                    pointer += authority_length;
                }
                // 2. Otherwise, if one of the following is true:
                //    * c is the EOF code point, U+002F (/), U+003F (?), or U+0023 (#)
                //    * url is special and c is U+005C (\)
                // NB: This is always the case per our search over authority above.
                else {
                    // then:

                    // 1. If atSignSeen is true and buffer is the empty string, host-missing validation error, return failure.
                    if at_sign_seen && authority.is_empty() {
                        return false;
                    }

                    // 2. Decrease pointer by buffer’s code point length + 1, set buffer to the empty string, and set state to host state.
                    buffer.clear();
                    state = State::Host;
                    url.password = std::mem::take(&mut password_builder);
                    url.username = std::mem::take(&mut username_builder);
                    continue;
                }
            }
            // -> host state, https://url.spec.whatwg.org/#host-state
            // -> hostname state, https://url.spec.whatwg.org/#hostname-state
            State::Host | State::Hostname => {
                // 1. If state override is given and url’s scheme is "file", then decrease pointer by 1 and set state to file host state.
                if options.state_override.is_some() && url.scheme == "file" {
                    state = State::FileHost;
                    continue;
                }
                // 2. Otherwise, if c is U+003A (:) and insideBrackets is false:
                else if code_point == Some(':') && !inside_brackets {
                    // 1. If buffer is the empty string, host-missing validation error, return failure.
                    if buffer.is_empty() {
                        return false;
                    }

                    // 2. If state override is given and state override is hostname state, then return failure.
                    if options.state_override == Some(State::Hostname) {
                        return false;
                    }

                    // 3. Let host be the result of host parsing buffer with url is not special.
                    // 4. If host is failure, then return failure.
                    let Some(host) = parse_host(&buffer, !url.is_special()) else {
                        return false;
                    };

                    // 5. Set url’s host to host, buffer to the empty string, and state to port state.
                    url.host = Some(host);
                    buffer.clear();
                    state = State::Port;
                }
                // 3. Otherwise, if one of the following is true:
                //     * c is the EOF code point, U+002F (/), U+003F (?), or U+0023 (#)
                //     * url is special and c is U+005C (\)
                else if matches!(code_point, None | Some('/' | '?' | '#'))
                    || (url.is_special() && code_point == Some('\\'))
                {
                    // then decrease pointer by 1, and:

                    // 1. If url is special and buffer is the empty string, host-missing validation error, return failure.
                    if url.is_special() && buffer.is_empty() {
                        return false;
                    }

                    // 2. Otherwise, if state override is given, buffer is the empty string, and either url
                    //    includes credentials or url’s port is non-null, then return failure.
                    if options.state_override.is_some()
                        && buffer.is_empty()
                        && (url_includes_credentials(url) || url.port.is_some())
                    {
                        return false;
                    }

                    // 3. Let host be the result of host parsing buffer with url is not special.
                    let Some(host) = parse_host(&buffer, !url.is_special()) else {
                        return false;
                    };

                    // 4. If host is failure, then return failure.
                    // NB: handled by the `let Some(host)` above.

                    // 5. Set url’s host to host, buffer to the empty string, and state to path start state.
                    url.host = Some(host);
                    buffer.clear();
                    state = State::PathStart;

                    // 6. If state override is given, then return.
                    if options.state_override.is_some() {
                        return true;
                    }

                    continue;
                }
                // 4. Otherwise:
                else if let Some(byte) = code_point {
                    // 1. If c is U+005B ([), then set insideBrackets to true.
                    if byte == '[' {
                        inside_brackets = true;
                    }
                    // 2. If c is U+005D (]), then set insideBrackets to false.
                    else if byte == ']' {
                        inside_brackets = false;
                    }
                    // 3. Append c to buffer.
                    buffer.push(byte);
                }
            }
            // -> port state, https://url.spec.whatwg.org/#port-state
            State::Port => {
                // 1. If c is an ASCII digit, append c to buffer.
                if let Some(byte) = code_point.filter(|byte| byte.is_ascii_digit()) {
                    buffer.push(byte);
                }
                // 2. Otherwise, if one of the following is true:
                //     * c is the EOF code point, U+002F (/), U+003F (?), or U+0023 (#);
                //     * url is special and c is U+005C (\); or
                //     * state override is given,
                else if matches!(code_point, None | Some('/' | '?' | '#'))
                    || (url.is_special() && code_point == Some('\\'))
                    || options.state_override.is_some()
                {
                    // 1. If buffer is not the empty string:
                    if !buffer.is_empty() {
                        // 1. Let port be the mathematical integer value that is represented by buffer in radix-10 using
                        //    ASCII digits for digits with values 0 through 9.
                        // 2. If port is not a 16-bit unsigned integer, port-out-of-range validation error, return failure.
                        let Ok(port) = buffer.parse::<u16>() else {
                            return false;
                        };

                        // 3. Set url’s port to null, if port is url’s scheme’s default port; otherwise to port.
                        if Some(port) == default_port_for_scheme(&url.scheme) {
                            url.port = None;
                        } else {
                            url.port = Some(port);
                        }

                        // 4. Set buffer to the empty string.
                        buffer.clear();

                        // 5. If state override is given, then return.
                        if options.state_override.is_some() {
                            return true;
                        }
                    }

                    // 2. If state override is given, then return failure.
                    if options.state_override.is_some() {
                        return false;
                    }

                    // 3. Set state to path start state and decrease pointer by 1.
                    state = State::PathStart;
                    continue;
                }
                // 3. Otherwise, port-invalid validation error, return failure.
                else {
                    return false;
                }
            }
            // -> file state, https://url.spec.whatwg.org/#file-state
            State::File => {
                // 1. Set url’s scheme to "file".
                url.scheme = "file".to_string();

                // 2. Set url’s host to the empty string.
                url.host = Some(Host::Domain(String::new()));

                // 3. If c is U+002F (/) or U+005C (\), then:
                if matches!(code_point, Some('/' | '\\')) {
                    // 1. If c is U+005C (\), invalid-reverse-solidus validation error.
                    if code_point == Some('\\') {
                        report_validation_error(State::File, pointer, code_point, "invalid-reverse-solidus");
                    }

                    // 2. Set state to file slash state.
                    state = State::FileSlash;
                }
                // 4. Otherwise, if base is non-null and base’s scheme is "file":
                else if options.base_url.is_some_and(|base_url| base_url.scheme == "file") {
                    let base_url = options.base_url.unwrap();

                    // 1. Set url’s host to base’s host, url’s path to a clone of base’s path, and url’s query to base’s query.
                    url.host = base_url.host.clone();
                    url.query = base_url.query.clone();

                    // 2. If c is U+003F (?), then set url’s query to the empty string and state to query state.
                    if code_point == Some('?') {
                        url.path = base_url.path.clone();
                        url.query = Some(String::new());
                        state = State::Query;
                    }
                    // 3. Otherwise, if c is U+0023 (#), set url’s fragment to the empty string and state to fragment state.
                    else if code_point == Some('#') {
                        url.path = base_url.path.clone();
                        url.fragment = Some(String::new());
                        state = State::Fragment;
                    }
                    // 4. Otherwise, if c is not the EOF code point:
                    else if code_point.is_some() {
                        // 1. Set url’s query to null.
                        url.query = None;

                        // 2. If the code point substring from pointer to the end of input does not start with a Windows
                        //    drive letter, then shorten url’s path.
                        if !starts_with_windows_drive_letter(remaining) {
                            url.path = cloned_shortened_path(base_url);
                        }
                        // 3. Otherwise:
                        else {
                            // 1. File-invalid-Windows-drive-letter validation error.
                            report_validation_error(
                                State::File,
                                pointer,
                                code_point,
                                "file-invalid-Windows-drive-letter",
                            );

                            // 2. Set url’s path to « ».
                            // NOTE: This is a (platform-independent) Windows drive letter quirk.
                            url.path.clear();
                        }

                        // 4. Set state to path state and decrease pointer by 1.
                        state = State::Path;
                        continue;
                    } else {
                        url.path = base_url.path.clone();
                    }
                }
                // 5. Otherwise, set state to path state, and decrease pointer by 1.
                else {
                    state = State::Path;
                    continue;
                }
            }
            // -> file slash state, https://url.spec.whatwg.org/#file-slash-state
            State::FileSlash => {
                // 1. If c is U+002F (/) or U+005C (\), then:
                if matches!(code_point, Some('/' | '\\')) {
                    // 1. If c is U+005C (\), invalid-reverse-solidus validation error.
                    if code_point == Some('\\') {
                        report_validation_error(State::FileSlash, pointer, code_point, "invalid-reverse-solidus");
                    }

                    // 2. Set state to file host state.
                    state = State::FileHost;
                }
                // 2. Otherwise:
                else {
                    // 1. If base is non-null and base’s scheme is "file", then:
                    if options.base_url.is_some_and(|base_url| base_url.scheme == "file") {
                        let base_url = options.base_url.unwrap();

                        // 1. Set url’s host to base’s host.
                        url.host = base_url.host.clone();

                        // 2. If the code point substring from pointer to the end of input does not start with a Windows
                        //    drive letter and base’s path[0] is a normalized Windows drive letter, then append base’s
                        //    path[0] to url’s path.
                        // NOTE: This is a (platform-independent) Windows drive letter quirk.
                        if !starts_with_windows_drive_letter(remaining)
                            && base_url
                                .path
                                .first()
                                .is_some_and(|segment| is_normalized_windows_drive_letter(segment))
                        {
                            url.path.push(base_url.path[0].clone());
                        }
                    }

                    // 2. Set state to path state, and decrease pointer by 1.
                    state = State::Path;
                    continue;
                }
            }
            // -> file host state, https://url.spec.whatwg.org/#file-host-state
            State::FileHost => {
                // 1. If c is the EOF code point, U+002F (/), U+005C (\), U+003F (?), or U+0023 (#), then decrease pointer by 1 and then:
                if matches!(code_point, None | Some('/' | '\\' | '?' | '#')) {
                    // 1. If state override is not given and buffer is a Windows drive letter,
                    //    file-invalid-Windows-drive-letter-host validation error, set state to path state.
                    //
                    // NOTE: This is a (platform-independent) Windows drive letter quirk. buffer is not reset here and
                    //       instead used in the path state.
                    if options.state_override.is_none() && is_windows_drive_letter(&buffer) {
                        report_validation_error(
                            State::FileHost,
                            pointer,
                            code_point,
                            "file-invalid-Windows-drive-letter-host",
                        );
                        state = State::Path;
                    }
                    // 2. Otherwise, if buffer is the empty string, then:
                    else if buffer.is_empty() {
                        // 1. Set url’s host to the empty string.
                        url.host = Some(Host::Domain(String::new()));

                        // 2. If state override is given, then return.
                        if options.state_override.is_some() {
                            return true;
                        }

                        // 3. Set state to path start state.
                        state = State::PathStart;
                    }
                    // 3. Otherwise, run these steps:
                    else {
                        // 1. Let host be the result of host parsing buffer with url is not special.
                        // 2. If host is failure, then return failure.
                        let Some(mut host) = parse_host(&buffer, !url.is_special()) else {
                            return false;
                        };

                        // 3. If host is "localhost", then set host to the empty string.
                        if host == Host::Domain("localhost".to_string()) {
                            host = Host::Domain(String::new());
                        }

                        // 4. Set url’s host to host.
                        url.host = Some(host);

                        // 5. If state override is given, then return.
                        if options.state_override.is_some() {
                            return true;
                        }

                        // 6. Set buffer to the empty string and state to path start state.
                        buffer.clear();
                        state = State::PathStart;
                    }

                    continue;
                }
                // 2. Otherwise, append c to buffer.
                else if let Some(byte) = code_point {
                    buffer.push(byte);
                }
            }
            // -> path start state, https://url.spec.whatwg.org/#path-start-state
            State::PathStart => {
                // 1. If url is special, then:
                if url.is_special() {
                    // 1. If c is U+005C (\), invalid-reverse-solidus validation error.
                    if code_point == Some('\\') {
                        report_validation_error(State::PathStart, pointer, code_point, "invalid-reverse-solidus");
                    }

                    // 2. Set state to path state.
                    state = State::Path;

                    // 3. If c is neither U+002F (/) nor U+005C (\), then decrease pointer by 1.
                    if code_point != Some('/') && code_point != Some('\\') {
                        continue;
                    }
                }
                // 2. Otherwise, if state override is not given and c is U+003F (?), set url’s query to the empty string
                //    and state to query state.
                else if options.state_override.is_none() && code_point == Some('?') {
                    url.query = Some(String::new());
                    state = State::Query;
                }
                // 3. Otherwise, if state override is not given and c is U+0023 (#), set url’s fragment to the empty
                //    string and state to fragment state.
                else if options.state_override.is_none() && code_point == Some('#') {
                    url.fragment = Some(String::new());
                    state = State::Fragment;
                }
                // 4. Otherwise, if c is not the EOF code point:
                else if code_point.is_some() {
                    // 1. Set state to path state.
                    state = State::Path;

                    // 2. If c is not U+002F (/), then decrease pointer by 1.
                    if code_point != Some('/') {
                        continue;
                    }
                }
                // 5. Otherwise, if state override is given and url’s host is null, append the empty string to url’s path.
                else if options.state_override.is_some() && url.host.is_none() {
                    url.path.push(String::new());
                }
            }
            // -> path state, https://url.spec.whatwg.org/#path-state
            State::Path => {
                // 1. If one of the following is true:
                //     * c is the EOF code point or U+002F (/)
                //     * url is special and c is U+005C (\)
                //     * state override is not given and c is U+003F (?) or U+0023 (#)
                // then:
                if matches!(code_point, None | Some('/'))
                    || (url.is_special() && code_point == Some('\\'))
                    || (options.state_override.is_none() && matches!(code_point, Some('?' | '#')))
                {
                    // 1. If url is special and c is U+005C (\), invalid-reverse-solidus validation error.
                    if url.is_special() && code_point == Some('\\') {
                        report_validation_error(State::Path, pointer, code_point, "invalid-reverse-solidus");
                    }

                    // 2. If buffer is a double-dot URL path segment, then:
                    if is_double_dot_path_segment(buffer.as_bytes()) {
                        // 1. Shorten url’s path.
                        shorten_urls_path(url);

                        // 2. If neither c is U+002F (/), nor url is special and c is U+005C (\), append the empty string to url’s path.
                        // NOTE: This means that for input /usr/.. the result is / and not a lack of a path.
                        if code_point != Some('/') && !(url.is_special() && code_point == Some('\\')) {
                            url.path.push(String::new());
                        }
                    }
                    // 3. Otherwise, if buffer is a single-dot URL path segment and if neither c is U+002F (/), nor url
                    //    is special and c is U+005C (\), append the empty string to url’s path.
                    else if is_single_dot_path_segment(buffer.as_bytes())
                        && code_point != Some('/')
                        && !(url.is_special() && code_point == Some('\\'))
                    {
                        url.path.push(String::new());
                    }
                    // 4. Otherwise, if buffer is not a single-dot URL path segment, then:
                    else if !is_single_dot_path_segment(buffer.as_bytes()) {
                        // 1. If url’s scheme is "file", url’s path is empty, and buffer is a Windows drive letter, then
                        //    replace the second code point in buffer with U+003A (:).
                        // NOTE: This is a (platform-independent) Windows drive letter quirk.
                        if url.scheme == "file" && url.path.is_empty() && is_windows_drive_letter(&buffer) {
                            let drive_letter = buffer.as_bytes()[0] as char;
                            buffer.clear();
                            buffer.push(drive_letter);
                            buffer.push(':');
                        }

                        // 2. Append buffer to url’s path.
                        url.path.push(std::mem::take(&mut buffer));
                    }

                    // 5. Set buffer to the empty string.
                    buffer.clear();

                    // 6. If c is U+003F (?), then set url’s query to the empty string and state to query state.
                    if code_point == Some('?') {
                        url.query = Some(String::new());
                        state = State::Query;
                    }
                    // 7. If c is U+0023 (#), then set url’s fragment to the empty string and state to fragment state.
                    else if code_point == Some('#') {
                        url.fragment = Some(String::new());
                        state = State::Fragment;
                    }
                }
                // 2. Otherwise, run these steps:
                else {
                    // OPTIMIZATION: The spec processes one code point at a time here. Copy the prefix of ASCII
                    // bytes that can be appended without changing parser state or requiring percent-encoding.
                    let prefix_length =
                        ascii_copyable_state_prefix_length(remaining, is_ascii_path_state_copyable_byte);
                    if prefix_length > 0 {
                        buffer.push_str(&remaining[..prefix_length]);
                        pointer += prefix_length;
                        continue;
                    }

                    // 1. If c is not a URL code point and not U+0025 (%), invalid-URL-unit validation error.
                    if let Some(byte) = code_point {
                        if !is_url_code_point(byte) && byte != '%' {
                            report_validation_error(State::Path, pointer, code_point, "invalid-URL-unit");
                        }

                        // 2. If c is U+0025 (%) and remaining does not start with two ASCII hex digits, invalid-URL-unit validation error.
                        if byte == '%' && !starts_with_two_ascii_hex_digits(remaining) {
                            report_validation_error(State::Path, pointer, code_point, "invalid-URL-unit");
                        }

                        append_percent_encoded_if_necessary(&mut buffer, byte, PercentEncodeSet::Path);
                    }
                }
            }
            // -> opaque path state, https://url.spec.whatwg.org/#cannot-be-a-base-url-path-state
            State::OpaquePath => {
                // NOTE: This does not follow the spec exactly but rather uses the buffer and only sets the path on EOF.
                assert!(url.has_opaque_path);
                if url.path.is_empty() {
                    url.path.push(String::new());
                }
                assert!(url.path.len() == 1 && url.path[0].is_empty());

                // 1. If c is U+003F (?), then set url’s query to the empty string and state to query state.
                if code_point == Some('?') {
                    url.path[0] = std::mem::take(&mut buffer);
                    url.query = Some(String::new());
                    state = State::Query;
                }
                // 2. Otherwise, if c is U+0023 (#), then set url’s fragment to the empty string and state to fragment state.
                else if code_point == Some('#') {
                    url.path[0] = std::mem::take(&mut buffer);
                    url.fragment = Some(String::new());
                    state = State::Fragment;
                }
                // 3. Otherwise, if c is U+0020 SPACE:
                else if code_point == Some(' ') {
                    // 1. If remaining starts with U+003F (?) or U+003F (#), then append "%20" to url’s path.
                    if remaining_after_code_point
                        .chars()
                        .next()
                        .is_some_and(|byte| matches!(byte, '?' | '#'))
                    {
                        buffer.push_str("%20");
                    }
                    // 2. Otherwise, append U+0020 SPACE to url’s path.
                    else {
                        buffer.push(' ');
                    }
                }
                // 4. Otherwise, if c is not the EOF code point:
                else if let Some(byte) = code_point {
                    // 1. If c is not a URL code point and not U+0025 (%), invalid-URL-unit validation error.
                    if !is_url_code_point(byte) && byte != '%' {
                        report_validation_error(State::OpaquePath, pointer, code_point, "invalid-URL-unit");
                    }
                    // 2. If c is U+0025 (%) and remaining does not start with two ASCII hex digits, invalid-URL-unit validation error.
                    if byte == '%' && !starts_with_two_ascii_hex_digits(remaining) {
                        report_validation_error(State::OpaquePath, pointer, code_point, "invalid-URL-unit");
                    }
                    // 3. UTF-8 percent-encode c using the C0 control percent-encode set and append the result to url’s path.
                    append_percent_encoded_if_necessary(&mut buffer, byte, PercentEncodeSet::C0Control);
                } else {
                    url.path[0] = std::mem::take(&mut buffer);
                }
            }
            // -> query state, https://url.spec.whatwg.org/#query-state
            State::Query => {
                // 1. If encoding is not UTF-8 and one of the following is true:
                //     * url is not special
                //     * url’s scheme is "ws" or "wss"
                if encoding != "utf-8" && (!url.is_special() || url.scheme == "ws" || url.scheme == "wss") {
                    // then set encoding to UTF-8.
                    encoding = "utf-8";
                }

                // 2. If one of the following is true:
                //     * state override is not given and c is U+0023 (#)
                //     * c is the EOF code point
                // then:
                if (options.state_override.is_none() && code_point == Some('#')) || code_point.is_none() {
                    // 1. Let queryPercentEncodeSet be the special-query percent-encode set if url is special; otherwise
                    //    the query percent-encode set.
                    let query_percent_encode_set = if url.is_special() {
                        PercentEncodeSet::SpecialQuery
                    } else {
                        PercentEncodeSet::Query
                    };

                    // 2. Percent-encode after encoding, with encoding, buffer, and queryPercentEncodeSet, and append the result to url’s query.
                    // NOTE: This operation cannot be invoked code-point-for-code-point due to the stateful ISO-2022-JP encoder.
                    let query = percent_encode_after_encoding(encoding, &buffer, query_percent_encode_set, false);
                    url.query = Some(query);

                    // 3. Set buffer to the empty string.
                    buffer.clear();

                    // 4. If c is U+0023 (#), then set url’s fragment to the empty string and state to fragment state.
                    if code_point == Some('#') {
                        url.fragment = Some(String::new());
                        state = State::Fragment;
                    }
                }
                // 3. Otherwise, if c is not the EOF code point:
                else if let Some(byte) = code_point {
                    // OPTIMIZATION: The spec appends query code points one at a time before the final encoding pass.
                    // Copy the matching ASCII URL-code-point prefix directly and resume normal handling at the first special byte.
                    let prefix_length =
                        ascii_copyable_state_prefix_length(remaining, is_ascii_query_state_copyable_byte);
                    if prefix_length > 0 {
                        buffer.push_str(&remaining[..prefix_length]);
                        pointer += prefix_length;
                        continue;
                    }

                    // 1. If c is not a URL code point and not U+0025 (%), invalid-URL-unit validation error.
                    if !is_url_code_point(byte) && byte != '%' {
                        report_validation_error(State::Query, pointer, code_point, "invalid-URL-unit");
                    }
                    // 2. If c is U+0025 (%) and remaining does not start with two ASCII hex digits, invalid-URL-unit validation error.
                    if byte == '%' && !starts_with_two_ascii_hex_digits(remaining) {
                        report_validation_error(State::Query, pointer, code_point, "invalid-URL-unit");
                    }
                    // 3. Append c to buffer.
                    buffer.push(byte);
                }
            }
            // -> fragment state, https://url.spec.whatwg.org/#fragment-state
            State::Fragment => {
                // 1. If c is not the EOF code point, then:
                if let Some(byte) = code_point {
                    // 1. If c is not a URL code point and not U+0025 (%), invalid-URL-unit validation error.
                    if !is_url_code_point(byte) && byte != '%' {
                        report_validation_error(State::Fragment, pointer, code_point, "invalid-URL-unit");
                    }
                    // 2. If c is U+0025 (%) and remaining does not start with two ASCII hex digits, invalid-URL-unit validation error.
                    if byte == '%' && !starts_with_two_ascii_hex_digits(remaining) {
                        report_validation_error(State::Fragment, pointer, code_point, "invalid-URL-unit");
                    }
                    // 3. UTF-8 percent-encode c using the fragment percent-encode set and append the result to url’s fragment.
                    // NOTE: The percent-encode is done on EOF on the entire buffer.
                    buffer.push(byte);
                } else {
                    let fragment_input = std::mem::take(&mut buffer);
                    let fragment =
                        percent_encode_after_encoding("utf-8", &fragment_input, PercentEncodeSet::Fragment, false);
                    url.fragment = Some(fragment);
                }
            }
        }

        if code_point.is_none() {
            break;
        }
        pointer += code_point.unwrap().len_utf8();
    }

    true
}
