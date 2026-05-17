/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::decode_utf8_to_u32;
use crate::token::{Attribute, Token, TokenPayload, TokenType};
use crate::tokenizer::HtmlTokenizer;
use std::ffi::c_void;

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiPreloadScannerAction {
    Base = 0,
    Fetch = 1,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiPreloadScannerDestination {
    None = 0,
    Font = 1,
    Image = 2,
    Script = 3,
    Style = 4,
    Track = 5,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiPreloadScannerCorsSetting {
    NoCors = 0,
    Anonymous = 1,
    UseCredentials = 2,
}

#[repr(C)]
pub struct RustFfiPreloadScannerEntry {
    pub action: RustFfiPreloadScannerAction,
    pub url_ptr: *const u8,
    pub url_len: usize,
    pub destination: RustFfiPreloadScannerDestination,
    pub cors_setting: RustFfiPreloadScannerCorsSetting,
}

/// Scan pending parser input for resources the speculative HTML parser can fetch.
///
/// # Safety
/// `input` must point to `input_len` valid UTF-8 bytes. `callback` must not retain pointers
/// from the provided entry beyond the callback invocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_preload_scanner_scan(
    input: *const u8,
    input_len: usize,
    ctx: *mut c_void,
    callback: unsafe extern "C" fn(ctx: *mut c_void, entry: *const RustFfiPreloadScannerEntry) -> bool,
) {
    let input = if input.is_null() || input_len == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(input, input_len) }
    };

    scan(input, |entry| unsafe { callback(ctx, &raw const *entry) });
}

pub(crate) fn scan(input: &[u8], mut callback: impl FnMut(&RustFfiPreloadScannerEntry) -> bool) {
    let code_points = decode_utf8_to_u32(input);
    let mut tokenizer = HtmlTokenizer::new(code_points);
    let mut template_depth: u64 = 0;
    let mut foreign_depth: u64 = 0;

    while let Some(token) = tokenizer.next_token(false, false) {
        let should_continue = match token.token_type {
            TokenType::StartTag => process_start_tag(&token, &mut template_depth, &mut foreign_depth, &mut callback),
            TokenType::EndTag => {
                process_end_tag(&token, &mut template_depth, &mut foreign_depth);
                true
            }
            TokenType::EndOfFile => false,
            _ => true,
        };
        if !should_continue {
            break;
        }
    }
}

fn process_start_tag(
    token: &Token,
    template_depth: &mut u64,
    foreign_depth: &mut u64,
    callback: &mut impl FnMut(&RustFfiPreloadScannerEntry) -> bool,
) -> bool {
    let tag_name = token.tag_name().as_bytes();

    if tag_name == b"template" {
        *template_depth = template_depth.saturating_add(1);
        return true;
    }

    if tag_name == b"svg" || tag_name == b"math" {
        *foreign_depth = foreign_depth.saturating_add(1);
        return true;
    }

    if *template_depth > 0 || *foreign_depth > 0 {
        return true;
    }

    let TokenPayload::Tag { attributes, .. } = &token.payload else {
        return true;
    };

    match tag_name {
        b"base" => process_base(attributes, callback),
        b"script" => process_script(attributes, callback),
        b"link" => process_link(attributes, callback),
        b"img" => process_img(attributes, callback),
        _ => true,
    }
}

fn process_end_tag(token: &Token, template_depth: &mut u64, foreign_depth: &mut u64) {
    let tag_name = token.tag_name().as_bytes();
    if tag_name == b"template" && *template_depth > 0 {
        *template_depth -= 1;
    } else if (tag_name == b"svg" || tag_name == b"math") && *foreign_depth > 0 {
        *foreign_depth -= 1;
    }
}

fn process_base(attributes: &[Attribute], callback: &mut impl FnMut(&RustFfiPreloadScannerEntry) -> bool) -> bool {
    let Some(href) = attribute_value(attributes, b"href") else {
        return true;
    };
    if href.is_empty() {
        return true;
    }

    emit_entry(
        callback,
        RustFfiPreloadScannerAction::Base,
        href,
        RustFfiPreloadScannerDestination::None,
        RustFfiPreloadScannerCorsSetting::NoCors,
    )
}

fn process_script(attributes: &[Attribute], callback: &mut impl FnMut(&RustFfiPreloadScannerEntry) -> bool) -> bool {
    let Some(src) = attribute_value(attributes, b"src") else {
        return true;
    };
    if src.is_empty() {
        return true;
    }

    emit_entry(
        callback,
        RustFfiPreloadScannerAction::Fetch,
        src,
        RustFfiPreloadScannerDestination::Script,
        cors_setting_from_attribute(attributes),
    )
}

fn process_link(attributes: &[Attribute], callback: &mut impl FnMut(&RustFfiPreloadScannerEntry) -> bool) -> bool {
    let Some(href) = attribute_value(attributes, b"href") else {
        return true;
    };
    if href.is_empty() {
        return true;
    }

    let Some(rel) = attribute_value(attributes, b"rel") else {
        return true;
    };

    let destination = if rel_contains_keyword(rel.as_bytes(), b"stylesheet") {
        RustFfiPreloadScannerDestination::Style
    } else if rel_contains_keyword(rel.as_bytes(), b"preload") {
        let Some(destination) = translate_preload_destination(attribute_value(attributes, b"as")) else {
            return true;
        };
        destination
    } else {
        return true;
    };

    emit_entry(
        callback,
        RustFfiPreloadScannerAction::Fetch,
        href,
        destination,
        cors_setting_from_attribute(attributes),
    )
}

fn process_img(attributes: &[Attribute], callback: &mut impl FnMut(&RustFfiPreloadScannerEntry) -> bool) -> bool {
    let Some(src) = attribute_value(attributes, b"src") else {
        return true;
    };
    if src.is_empty() {
        return true;
    }

    emit_entry(
        callback,
        RustFfiPreloadScannerAction::Fetch,
        src,
        RustFfiPreloadScannerDestination::Image,
        cors_setting_from_attribute(attributes),
    )
}

fn emit_entry(
    callback: &mut impl FnMut(&RustFfiPreloadScannerEntry) -> bool,
    action: RustFfiPreloadScannerAction,
    url: &str,
    destination: RustFfiPreloadScannerDestination,
    cors_setting: RustFfiPreloadScannerCorsSetting,
) -> bool {
    let entry = RustFfiPreloadScannerEntry {
        action,
        url_ptr: url.as_ptr(),
        url_len: url.len(),
        destination,
        cors_setting,
    };
    callback(&entry)
}

fn attribute_value<'a>(attributes: &'a [Attribute], name: &[u8]) -> Option<&'a str> {
    attributes
        .iter()
        .find(|attribute| attribute.local_name_bytes() == name)
        .map(|attribute| attribute.value.as_str())
}

fn rel_contains_keyword(rel: &[u8], keyword: &[u8]) -> bool {
    rel.split(|byte| byte.is_ascii_whitespace())
        .any(|token| token.eq_ignore_ascii_case(keyword))
}

fn translate_preload_destination(destination: Option<&str>) -> Option<RustFfiPreloadScannerDestination> {
    Some(match destination?.as_bytes() {
        b"fetch" => RustFfiPreloadScannerDestination::None,
        b"font" => RustFfiPreloadScannerDestination::Font,
        b"image" => RustFfiPreloadScannerDestination::Image,
        b"script" => RustFfiPreloadScannerDestination::Script,
        b"style" => RustFfiPreloadScannerDestination::Style,
        b"track" => RustFfiPreloadScannerDestination::Track,
        _ => return None,
    })
}

fn cors_setting_from_attribute(attributes: &[Attribute]) -> RustFfiPreloadScannerCorsSetting {
    let Some(crossorigin) = attribute_value(attributes, b"crossorigin") else {
        return RustFfiPreloadScannerCorsSetting::NoCors;
    };

    if crossorigin.as_bytes().eq_ignore_ascii_case(b"use-credentials") {
        RustFfiPreloadScannerCorsSetting::UseCredentials
    } else {
        RustFfiPreloadScannerCorsSetting::Anonymous
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Debug, Eq, PartialEq)]
    struct ScannedEntry {
        action: RustFfiPreloadScannerAction,
        url: String,
        destination: RustFfiPreloadScannerDestination,
        cors_setting: RustFfiPreloadScannerCorsSetting,
    }

    fn collect(input: &str) -> Vec<ScannedEntry> {
        let mut entries = Vec::new();
        scan(input.as_bytes(), |entry| {
            let url = unsafe { std::slice::from_raw_parts(entry.url_ptr, entry.url_len) };
            entries.push(ScannedEntry {
                action: entry.action,
                url: std::str::from_utf8(url).unwrap().to_string(),
                destination: entry.destination,
                cors_setting: entry.cors_setting,
            });
            true
        });
        entries
    }

    #[test]
    fn finds_resources_and_skips_template_and_foreign_content() {
        let entries = collect(
            r#"
                <base href="./base/">
                <link rel="stylesheet" href="./style.css">
                <link rel="preload" as="image" href="./image.png">
                <img src="./photo.png">
                <script src="./script.js"></script>
                <template><script src="./template.js"></script></template>
                <svg><script src="./svg.js"></script></svg>
            "#,
        );

        assert_eq!(
            entries,
            vec![
                ScannedEntry {
                    action: RustFfiPreloadScannerAction::Base,
                    url: "./base/".to_string(),
                    destination: RustFfiPreloadScannerDestination::None,
                    cors_setting: RustFfiPreloadScannerCorsSetting::NoCors,
                },
                ScannedEntry {
                    action: RustFfiPreloadScannerAction::Fetch,
                    url: "./style.css".to_string(),
                    destination: RustFfiPreloadScannerDestination::Style,
                    cors_setting: RustFfiPreloadScannerCorsSetting::NoCors,
                },
                ScannedEntry {
                    action: RustFfiPreloadScannerAction::Fetch,
                    url: "./image.png".to_string(),
                    destination: RustFfiPreloadScannerDestination::Image,
                    cors_setting: RustFfiPreloadScannerCorsSetting::NoCors,
                },
                ScannedEntry {
                    action: RustFfiPreloadScannerAction::Fetch,
                    url: "./photo.png".to_string(),
                    destination: RustFfiPreloadScannerDestination::Image,
                    cors_setting: RustFfiPreloadScannerCorsSetting::NoCors,
                },
                ScannedEntry {
                    action: RustFfiPreloadScannerAction::Fetch,
                    url: "./script.js".to_string(),
                    destination: RustFfiPreloadScannerDestination::Script,
                    cors_setting: RustFfiPreloadScannerCorsSetting::NoCors,
                },
            ]
        );
    }

    #[test]
    fn handles_link_rel_and_preload_destination_rules() {
        let entries = collect(
            r#"
                <link rel="modulepreload PRELOAD" as="fetch" href="./fetch">
                <link rel="preload stylesheet" as="image" href="./style">
                <link rel="preload" as="font" href="./font">
                <link rel="preload" as="IMAGE" href="./invalid-case">
                <link rel="preload" href="./missing-as">
            "#,
        );

        assert_eq!(
            entries,
            vec![
                ScannedEntry {
                    action: RustFfiPreloadScannerAction::Fetch,
                    url: "./fetch".to_string(),
                    destination: RustFfiPreloadScannerDestination::None,
                    cors_setting: RustFfiPreloadScannerCorsSetting::NoCors,
                },
                ScannedEntry {
                    action: RustFfiPreloadScannerAction::Fetch,
                    url: "./style".to_string(),
                    destination: RustFfiPreloadScannerDestination::Style,
                    cors_setting: RustFfiPreloadScannerCorsSetting::NoCors,
                },
                ScannedEntry {
                    action: RustFfiPreloadScannerAction::Fetch,
                    url: "./font".to_string(),
                    destination: RustFfiPreloadScannerDestination::Font,
                    cors_setting: RustFfiPreloadScannerCorsSetting::NoCors,
                },
            ]
        );
    }

    #[test]
    fn maps_crossorigin_attribute_values() {
        let entries = collect(
            r#"
                <script src="./missing.js"></script>
                <script crossorigin src="./empty.js"></script>
                <script crossorigin="anonymous" src="./anonymous.js"></script>
                <script crossorigin="use-credentials" src="./credentials.js"></script>
                <script crossorigin="USE-CREDENTIALS" src="./credentials-case.js"></script>
            "#,
        );

        assert_eq!(
            entries.iter().map(|entry| entry.cors_setting).collect::<Vec<_>>(),
            vec![
                RustFfiPreloadScannerCorsSetting::NoCors,
                RustFfiPreloadScannerCorsSetting::Anonymous,
                RustFfiPreloadScannerCorsSetting::Anonymous,
                RustFfiPreloadScannerCorsSetting::UseCredentials,
                RustFfiPreloadScannerCorsSetting::UseCredentials,
            ]
        );
    }
}
