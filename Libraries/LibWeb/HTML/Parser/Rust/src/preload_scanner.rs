/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::known_names::attribute_name;
use crate::known_names::tag_name;
use crate::token::Attribute;
use crate::token::KnownName;
use crate::token::Token;
use crate::token::TokenPayload;
use crate::token::TokenType;
use crate::tokenizer::HtmlTokenizer;
use std::borrow::Cow;
use std::ffi::c_void;

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiPreloadScannerAction {
    Base = 0,
    Fetch = 1,
    ModulePreload = 2,
    ModuleScript = 3,
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
    AudioWorklet = 6,
    JSON = 7,
    PaintWorklet = 8,
    ServiceWorker = 9,
    SharedWorker = 10,
    Worker = 11,
    Text = 12,
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
    pub nonce_ptr: *const u8,
    pub nonce_len: usize,
    pub integrity_ptr: *const u8,
    pub integrity_len: usize,
    pub integrity_present: bool,
    pub referrer_policy_ptr: *const u8,
    pub referrer_policy_len: usize,
    pub fetch_priority_ptr: *const u8,
    pub fetch_priority_len: usize,
    pub media_ptr: *const u8,
    pub media_len: usize,
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

/// Scan pending UTF-16 parser input for resources the speculative HTML parser can fetch.
///
/// # Safety
/// `input` must point to `input_len` UTF-16 code units. `callback` must not retain
/// pointers from the provided entry beyond the callback invocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_preload_scanner_scan_utf16(
    input: *const u16,
    input_len: usize,
    ctx: *mut c_void,
    callback: unsafe extern "C" fn(ctx: *mut c_void, entry: *const RustFfiPreloadScannerEntry) -> bool,
) {
    let input = if input.is_null() || input_len == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(input, input_len) }
    };

    scan_utf16(input, |entry| unsafe { callback(ctx, &raw const *entry) });
}

pub(crate) fn scan(input: &[u8], mut callback: impl FnMut(&RustFfiPreloadScannerEntry) -> bool) {
    // SAFETY: The FFI entry point guarantees valid UTF-8.
    let code_units = unsafe { std::str::from_utf8_unchecked(input) }.encode_utf16().collect();
    scan_code_units(code_units, &mut callback);
}

pub(crate) fn scan_utf16(input: &[u16], mut callback: impl FnMut(&RustFfiPreloadScannerEntry) -> bool) {
    scan_code_units(input.to_vec(), &mut callback);
}

fn scan_code_units(code_units: Vec<u16>, callback: &mut impl FnMut(&RustFfiPreloadScannerEntry) -> bool) {
    let mut tokenizer = HtmlTokenizer::new(code_units);
    let mut template_depth: u64 = 0;
    let mut foreign_depth: u64 = 0;

    while let Some(token) = tokenizer.next_token(false, false) {
        let should_continue = match token.token_type {
            TokenType::StartTag => process_start_tag(&token, &mut template_depth, &mut foreign_depth, callback),
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
    let tag_name = token.tag_name();

    if *tag_name == tag_name!("template") {
        *template_depth = template_depth.saturating_add(1);
        return true;
    }

    if *tag_name == tag_name!("svg") || *tag_name == tag_name!("math") {
        *foreign_depth = foreign_depth.saturating_add(1);
        return true;
    }

    if *template_depth > 0 || *foreign_depth > 0 {
        return true;
    }

    let TokenPayload::Tag { attributes, .. } = &token.payload else {
        return true;
    };

    if *tag_name == tag_name!("base") {
        process_base(attributes, callback)
    } else if *tag_name == tag_name!("script") {
        process_script(attributes, callback)
    } else if *tag_name == tag_name!("link") {
        process_link(attributes, callback)
    } else if *tag_name == tag_name!("img") {
        process_img(attributes, callback)
    } else {
        true
    }
}

fn process_end_tag(token: &Token, template_depth: &mut u64, foreign_depth: &mut u64) {
    let tag_name = token.tag_name();
    if *tag_name == tag_name!("template") && *template_depth > 0 {
        *template_depth -= 1;
    } else if (*tag_name == tag_name!("svg") || *tag_name == tag_name!("math")) && *foreign_depth > 0 {
        *foreign_depth -= 1;
    }
}

fn process_base(attributes: &[Attribute], callback: &mut impl FnMut(&RustFfiPreloadScannerEntry) -> bool) -> bool {
    let Some(href) = attribute_value(attributes, attribute_name!("href")) else {
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
        None,
    )
}

fn process_script(attributes: &[Attribute], callback: &mut impl FnMut(&RustFfiPreloadScannerEntry) -> bool) -> bool {
    let Some(src) = attribute_value(attributes, attribute_name!("src")) else {
        return true;
    };
    if src.is_empty() {
        return true;
    }
    let action = match script_type_from_attributes(attributes) {
        // 'If el has a nomodule content attribute and its type is "classic", then return.' — so the script never runs,
        // and preparing it doesn't fetch src.
        Some(ScriptType::Classic) if attribute_value(attributes, attribute_name!("nomodule")).is_some() => return true,
        Some(ScriptType::Classic) => RustFfiPreloadScannerAction::Fetch,
        Some(ScriptType::Module) => RustFfiPreloadScannerAction::ModuleScript,
        // An import map or speculation-rules script with a src attribute only fires an error event, and a script whose
        // type is null never runs — none of them fetch src.
        Some(ScriptType::ImportMap) | Some(ScriptType::SpeculationRules) | None => return true,
    };
    emit_entry(
        callback,
        action,
        src,
        RustFfiPreloadScannerDestination::Script,
        cors_setting_from_attribute(attributes),
        Some(attributes),
    )
}

/// The type "prepare the script element" gives a script element from its type and language attributes.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ScriptType {
    Classic,
    Module,
    ImportMap,
    SpeculationRules,
}

// https://mimesniff.spec.whatwg.org/#javascript-mime-type
// A JavaScript MIME type is any MIME type whose essence is one of the following:
const JAVASCRIPT_MIME_TYPE_ESSENCES: [&[u8]; 16] = [
    b"application/ecmascript",
    b"application/javascript",
    b"application/x-ecmascript",
    b"application/x-javascript",
    b"text/ecmascript",
    b"text/javascript",
    b"text/javascript1.0",
    b"text/javascript1.1",
    b"text/javascript1.2",
    b"text/javascript1.3",
    b"text/javascript1.4",
    b"text/javascript1.5",
    b"text/jscript",
    b"text/livescript",
    b"text/x-ecmascript",
    b"text/x-javascript",
];

// https://html.spec.whatwg.org/multipage/scripting.html#prepare-the-script-element
// Steps 8 to 13 of "prepare the script element": the type the element gets from its type and language attributes, or
// None when "no script is executed, and el's type is left as null".
fn script_type_from_attributes(attributes: &[Attribute]) -> Option<ScriptType> {
    let type_attribute = attribute_value(attributes, attribute_name!("type"));
    let language_attribute = attribute_value(attributes, attribute_name!("language"));

    // 8. If any of the following are true:
    //    - el has a type attribute whose value is the empty string;
    //    - el has no type attribute but it has a language attribute and that attribute's value is the empty string; or
    //    - el has neither a type attribute nor a language attribute,
    //    then let the script block's type string for this script element be "text/javascript".
    //    Otherwise, if el has a type attribute, then let the script block's type string be the value of that attribute
    //    with leading and trailing ASCII whitespace stripped.
    //    Otherwise, el has a non-empty language attribute; let the script block's type string be the concatenation of
    //    "text/" and the value of el's language attribute.
    let type_string: Cow<'_, [u8]> = match (type_attribute, language_attribute) {
        (Some(""), _) | (None, Some("")) | (None, None) => Cow::Borrowed(b"text/javascript"),
        (Some(type_attribute), _) => Cow::Borrowed(type_attribute.as_bytes().trim_ascii()),
        (None, Some(language)) => Cow::Owned([b"text/".as_slice(), language.as_bytes()].concat()),
    };

    // 9. If the script block's type string is a JavaScript MIME type essence match, then set el's type to "classic".
    if JAVASCRIPT_MIME_TYPE_ESSENCES
        .iter()
        .any(|essence| type_string.eq_ignore_ascii_case(essence))
    {
        return Some(ScriptType::Classic);
    }

    // 10. Otherwise, if the script block's type string is an ASCII case-insensitive match for the string "module", then
    //     set el's type to "module".
    if type_string.eq_ignore_ascii_case(b"module") {
        return Some(ScriptType::Module);
    }

    // 11. Otherwise, if the script block's type string is an ASCII case-insensitive match for the string "importmap",
    //     then set el's type to "importmap".
    if type_string.eq_ignore_ascii_case(b"importmap") {
        return Some(ScriptType::ImportMap);
    }

    // 12. Otherwise, if the script block's type string is an ASCII case-insensitive match for the string
    //     "speculationrules", then set el's type to "speculationrules".
    if type_string.eq_ignore_ascii_case(b"speculationrules") {
        return Some(ScriptType::SpeculationRules);
    }

    // 13. Otherwise, return. (No script is executed, and el's type is left as null.)
    None
}

fn process_link(attributes: &[Attribute], callback: &mut impl FnMut(&RustFfiPreloadScannerEntry) -> bool) -> bool {
    let Some(href) = attribute_value(attributes, attribute_name!("href")) else {
        return true;
    };
    if href.is_empty() {
        return true;
    }

    let Some(rel) = attribute_value(attributes, attribute_name!("rel")) else {
        return true;
    };

    let (action, destination) = if rel_contains_keyword(rel.as_bytes(), b"stylesheet") {
        (
            RustFfiPreloadScannerAction::Fetch,
            RustFfiPreloadScannerDestination::Style,
        )
    } else if rel_contains_keyword(rel.as_bytes(), b"modulepreload") {
        let Some(destination) = translate_modulepreload_destination(attribute_value(attributes, attribute_name!("as")))
        else {
            return true;
        };
        (RustFfiPreloadScannerAction::ModulePreload, destination)
    } else if rel_contains_keyword(rel.as_bytes(), b"preload") {
        let Some(destination) = translate_preload_destination(attribute_value(attributes, attribute_name!("as")))
        else {
            return true;
        };
        (RustFfiPreloadScannerAction::Fetch, destination)
    } else {
        return true;
    };

    emit_entry(
        callback,
        action,
        href,
        destination,
        cors_setting_from_attribute(attributes),
        Some(attributes),
    )
}

fn process_img(attributes: &[Attribute], callback: &mut impl FnMut(&RustFfiPreloadScannerEntry) -> bool) -> bool {
    let Some(src) = attribute_value(attributes, attribute_name!("src")) else {
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
        Some(attributes),
    )
}

fn emit_entry(
    callback: &mut impl FnMut(&RustFfiPreloadScannerEntry) -> bool,
    action: RustFfiPreloadScannerAction,
    url: &str,
    destination: RustFfiPreloadScannerDestination,
    cors_setting: RustFfiPreloadScannerCorsSetting,
    attributes: Option<&[Attribute]>,
) -> bool {
    let attribute_pointer_and_length = |name| {
        attribute_value(attributes.unwrap_or_default(), name)
            .map(|value| (value.as_ptr(), value.len()))
            .unwrap_or((std::ptr::null(), 0))
    };
    let (nonce_ptr, nonce_len) = attribute_pointer_and_length(attribute_name!("nonce"));
    let (integrity_ptr, integrity_len) = attribute_pointer_and_length(attribute_name!("integrity"));
    let (referrer_policy_ptr, referrer_policy_len) = attribute_pointer_and_length(attribute_name!("referrerpolicy"));
    let (fetch_priority_ptr, fetch_priority_len) = attribute_pointer_and_length(attribute_name!("fetchpriority"));
    let (media_ptr, media_len) = attribute_pointer_and_length(attribute_name!("media"));
    let entry = RustFfiPreloadScannerEntry {
        action,
        url_ptr: url.as_ptr(),
        url_len: url.len(),
        destination,
        cors_setting,
        nonce_ptr,
        nonce_len,
        integrity_ptr,
        integrity_len,
        integrity_present: !integrity_ptr.is_null(),
        referrer_policy_ptr,
        referrer_policy_len,
        fetch_priority_ptr,
        fetch_priority_len,
        media_ptr,
        media_len,
    };
    callback(&entry)
}

fn attribute_value(attributes: &[Attribute], name: KnownName) -> Option<&str> {
    attributes
        .iter()
        .find(|attribute| attribute.local_name == name)
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

fn translate_modulepreload_destination(destination: Option<&str>) -> Option<RustFfiPreloadScannerDestination> {
    let Some(destination) = destination else {
        return Some(RustFfiPreloadScannerDestination::Script);
    };

    let destination = destination.as_bytes();

    // A module preload destination is "json", "style", "text", or a script-like destination.
    if destination.eq_ignore_ascii_case(b"audioworklet") {
        return Some(RustFfiPreloadScannerDestination::AudioWorklet);
    }
    if destination.eq_ignore_ascii_case(b"json") {
        return Some(RustFfiPreloadScannerDestination::JSON);
    }
    if destination.eq_ignore_ascii_case(b"paintworklet") {
        return Some(RustFfiPreloadScannerDestination::PaintWorklet);
    }
    if destination.eq_ignore_ascii_case(b"script") {
        return Some(RustFfiPreloadScannerDestination::Script);
    }
    if destination.eq_ignore_ascii_case(b"serviceworker") {
        return Some(RustFfiPreloadScannerDestination::ServiceWorker);
    }
    if destination.eq_ignore_ascii_case(b"sharedworker") {
        return Some(RustFfiPreloadScannerDestination::SharedWorker);
    }
    if destination.eq_ignore_ascii_case(b"style") {
        return Some(RustFfiPreloadScannerDestination::Style);
    }
    if destination.eq_ignore_ascii_case(b"text") {
        return Some(RustFfiPreloadScannerDestination::Text);
    }
    if destination.eq_ignore_ascii_case(b"worker") {
        return Some(RustFfiPreloadScannerDestination::Worker);
    }

    const NON_MODULE_DESTINATIONS: [&[u8]; 15] = [
        b"audio",
        b"document",
        b"embed",
        b"fetch",
        b"font",
        b"frame",
        b"iframe",
        b"image",
        b"manifest",
        b"object",
        b"report",
        b"track",
        b"video",
        b"webidentity",
        b"xslt",
    ];
    if NON_MODULE_DESTINATIONS
        .iter()
        .any(|candidate| destination.eq_ignore_ascii_case(candidate))
    {
        return None;
    }

    // NB: Invalid and empty values put the enumerated as attribute in no state, so step 2 uses "script".
    Some(RustFfiPreloadScannerDestination::Script)
}

fn cors_setting_from_attribute(attributes: &[Attribute]) -> RustFfiPreloadScannerCorsSetting {
    let Some(crossorigin) = attribute_value(attributes, attribute_name!("crossorigin")) else {
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

    fn collect_utf16(input: &str) -> Vec<ScannedEntry> {
        let input = input.encode_utf16().collect::<Vec<_>>();
        let mut entries = Vec::new();
        scan_utf16(&input, |entry| {
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
    fn scans_utf16_input() {
        let entries = collect_utf16(r#"<base href="/base/😀"><img src="./photo.png">"#);

        assert_eq!(
            entries,
            vec![
                ScannedEntry {
                    action: RustFfiPreloadScannerAction::Base,
                    url: "/base/😀".to_string(),
                    destination: RustFfiPreloadScannerDestination::None,
                    cors_setting: RustFfiPreloadScannerCorsSetting::NoCors,
                },
                ScannedEntry {
                    action: RustFfiPreloadScannerAction::Fetch,
                    url: "./photo.png".to_string(),
                    destination: RustFfiPreloadScannerDestination::Image,
                    cors_setting: RustFfiPreloadScannerCorsSetting::NoCors,
                },
            ]
        );
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
                <link rel="modulepreload" href="./module">
                <link rel="modulepreload" as="JSON" crossorigin="use-credentials" href="./data">
                <link rel="modulepreload" as="invalid" href="./invalid-default">
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
                    action: RustFfiPreloadScannerAction::ModulePreload,
                    url: "./module".to_string(),
                    destination: RustFfiPreloadScannerDestination::Script,
                    cors_setting: RustFfiPreloadScannerCorsSetting::NoCors,
                },
                ScannedEntry {
                    action: RustFfiPreloadScannerAction::ModulePreload,
                    url: "./data".to_string(),
                    destination: RustFfiPreloadScannerDestination::JSON,
                    cors_setting: RustFfiPreloadScannerCorsSetting::UseCredentials,
                },
                ScannedEntry {
                    action: RustFfiPreloadScannerAction::ModulePreload,
                    url: "./invalid-default".to_string(),
                    destination: RustFfiPreloadScannerDestination::Script,
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

    #[test]
    fn preserves_modulepreload_fetch_options() {
        let mut options = None;
        scan(
            br#"<link rel="modulepreload" href="./module" nonce="abc" integrity="sha256-xyz" referrerpolicy="origin" fetchpriority="high" media="screen">"#,
            |entry| {
                let string = |pointer, length| {
                    let bytes = unsafe { std::slice::from_raw_parts(pointer, length) };
                    std::str::from_utf8(bytes).unwrap().to_string()
                };
                options = Some((
                    string(entry.nonce_ptr, entry.nonce_len),
                    string(entry.integrity_ptr, entry.integrity_len),
                    entry.integrity_present,
                    string(entry.referrer_policy_ptr, entry.referrer_policy_len),
                    string(entry.fetch_priority_ptr, entry.fetch_priority_len),
                    string(entry.media_ptr, entry.media_len),
                ));
                true
            },
        );

        assert_eq!(
            options,
            Some((
                "abc".to_string(),
                "sha256-xyz".to_string(),
                true,
                "origin".to_string(),
                "high".to_string(),
                "screen".to_string(),
            ))
        );
    }

    #[test]
    fn classifies_script_elements_by_type() {
        let entries = collect(
            r#"
                <script src="./classic.js"></script>
                <script type="" src="./empty-type.js"></script>
                <script type=" Text/JavaScript " src="./javascript-mime-type.js"></script>
                <script language="javascript1.5" src="./language.js"></script>
                <script type="module" src="./module.js"></script>
                <script type=" MODULE " src="./module-uppercase.js"></script>
                <script type="module" crossorigin src="./module-anonymous.js"></script>
                <script type="module" crossorigin="use-credentials" src="./module-credentialed.js"></script>
                <script type="module" nomodule src="./module-nomodule.js"></script>
                <script nomodule src="./classic-nomodule.js"></script>
                <script type="importmap" src="./import-map.json"></script>
                <script type="speculationrules" src="./speculation-rules.json"></script>
                <script type="text/template" src="./template.html"></script>
                <script language="" type="text/template" src="./template-language.html"></script>
            "#,
        );
        let script = |action, url: &str, cors_setting| ScannedEntry {
            action,
            url: url.to_string(),
            destination: RustFfiPreloadScannerDestination::Script,
            cors_setting,
        };
        assert_eq!(
            entries,
            vec![
                script(
                    RustFfiPreloadScannerAction::Fetch,
                    "./classic.js",
                    RustFfiPreloadScannerCorsSetting::NoCors
                ),
                script(
                    RustFfiPreloadScannerAction::Fetch,
                    "./empty-type.js",
                    RustFfiPreloadScannerCorsSetting::NoCors
                ),
                script(
                    RustFfiPreloadScannerAction::Fetch,
                    "./javascript-mime-type.js",
                    RustFfiPreloadScannerCorsSetting::NoCors
                ),
                script(
                    RustFfiPreloadScannerAction::Fetch,
                    "./language.js",
                    RustFfiPreloadScannerCorsSetting::NoCors
                ),
                script(
                    RustFfiPreloadScannerAction::ModuleScript,
                    "./module.js",
                    RustFfiPreloadScannerCorsSetting::NoCors
                ),
                script(
                    RustFfiPreloadScannerAction::ModuleScript,
                    "./module-uppercase.js",
                    RustFfiPreloadScannerCorsSetting::NoCors
                ),
                script(
                    RustFfiPreloadScannerAction::ModuleScript,
                    "./module-anonymous.js",
                    RustFfiPreloadScannerCorsSetting::Anonymous
                ),
                script(
                    RustFfiPreloadScannerAction::ModuleScript,
                    "./module-credentialed.js",
                    RustFfiPreloadScannerCorsSetting::UseCredentials
                ),
                script(
                    RustFfiPreloadScannerAction::ModuleScript,
                    "./module-nomodule.js",
                    RustFfiPreloadScannerCorsSetting::NoCors
                ),
            ]
        );
    }
}
