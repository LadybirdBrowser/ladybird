/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::RustFfiTokenizerHandle;
use crate::code_units::RustFfiHtmlUtf16View;
use crate::known_names::attribute_name;
use crate::known_names::tag_name;
use crate::token::Attribute;
use crate::token::HtmlName;
use crate::token::KnownName;
use crate::token::Token;
use crate::token::TokenPayload;
use crate::token::TokenType;
use crate::tokenizer::HtmlTokenizer;
use crate::tokenizer::State;
use ak::Utf16View;
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
    pub url: RustFfiHtmlUtf16View,
    pub destination: RustFfiPreloadScannerDestination,
    pub cors_setting: RustFfiPreloadScannerCorsSetting,
    pub nonce: RustFfiHtmlUtf16View,
    pub integrity: RustFfiHtmlUtf16View,
    pub integrity_present: bool,
    pub referrer_policy: RustFfiHtmlUtf16View,
    pub fetch_priority: RustFfiHtmlUtf16View,
    pub media: RustFfiHtmlUtf16View,
}

/// A preload scanner that keeps its place in the parser's input from one run to the next, so that a speculative HTML
/// parser started while another script blocks the parser goes on from where the last one stopped, rather than scanning
/// the same input again.
pub struct RustFfiPreloadScanner {
    tokenizer: HtmlTokenizer,
    template_depth: u64,
    foreign_depth: u64,
    scripting_enabled: bool,
}

impl RustFfiPreloadScanner {
    fn new(scripting_enabled: bool) -> Self {
        let mut tokenizer = HtmlTokenizer::new(Vec::<u8>::new());
        // More input can arrive, so the scanner waits at the end of what it has for the rest of a token.
        tokenizer.set_input_stream_closed(false);
        RustFfiPreloadScanner {
            tokenizer,
            template_depth: 0,
            foreign_depth: 0,
            scripting_enabled,
        }
    }

    fn reset(&mut self) {
        *self = Self::new(self.scripting_enabled);
    }

    fn append_input(&mut self, code_units: Utf16View<'_>) {
        self.tokenizer.append_input(code_units);
    }

    fn scan(&mut self, callback: &mut impl FnMut(&RustFfiPreloadScannerEntry) -> bool) {
        while let Some(token) = self.tokenizer.next_token(false, false) {
            let should_continue = match token.token_type {
                TokenType::StartTag => {
                    let should_continue =
                        process_start_tag(&token, &mut self.template_depth, &mut self.foreign_depth, callback);

                    // The tree builder switches the tokenizer out of the data state for these elements, so their
                    // contents are text rather than markup. It never does so for foreign content.
                    if self.foreign_depth == 0
                        && let Some(state) = tokenizer_state_after_start_tag(token.tag_name(), self.scripting_enabled)
                    {
                        self.tokenizer.switch_to(state);
                    }
                    should_continue
                }
                TokenType::EndTag => {
                    process_end_tag(&token, &mut self.template_depth, &mut self.foreign_depth);
                    true
                }
                TokenType::EndOfFile => false,
                _ => true,
            };
            if !should_continue {
                break;
            }
        }

        // Drop the input scanned so far, unless a token that isn't finished yet still needs it.
        self.tokenizer.parser_did_run();
    }
}

/// Create a preload scanner for a speculative HTML parser.
#[unsafe(no_mangle)]
pub extern "C" fn rust_html_preload_scanner_create(scripting_enabled: bool) -> *mut RustFfiPreloadScanner {
    Box::into_raw(Box::new(RustFfiPreloadScanner::new(scripting_enabled)))
}

/// Destroy a preload scanner.
///
/// # Safety
/// `scanner` must come from `rust_html_preload_scanner_create`, and must not be used afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_preload_scanner_destroy(scanner: *mut RustFfiPreloadScanner) {
    if !scanner.is_null() {
        // SAFETY: The caller passes a scanner from rust_html_preload_scanner_create.
        drop(unsafe { Box::from_raw(scanner) });
    }
}

/// Hand the scanner the input of the parser that it hasn't seen yet. Returns false if the scanner had to start over at
/// the parser's current position, because the parser got past where it stopped, input was inserted before there, or
/// the caller asked for it with `start_over`.
#[unsafe(no_mangle)]
pub extern "C" fn rust_html_preload_scanner_append_parser_input(
    scanner: Option<&mut RustFfiPreloadScanner>,
    tokenizer: Option<&mut RustFfiTokenizerHandle>,
    start_over: bool,
) -> bool {
    let (Some(scanner), Some(tokenizer)) = (scanner, tokenizer) else {
        return false;
    };
    if start_over {
        tokenizer.tokenizer.forget_speculative_scan_end();
    }
    let (input, continues) = tokenizer.tokenizer.input_for_speculative_scan();
    if !continues {
        scanner.reset();
    }
    scanner.append_input(input);
    continues
}

/// Scan the input the scanner has for resources the speculative HTML parser can fetch, stopping at the end of it or
/// once `callback` returns false.
///
/// # Safety
/// `callback` must not retain pointers from the provided entry beyond the callback invocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_preload_scanner_scan(
    scanner: Option<&mut RustFfiPreloadScanner>,
    ctx: *mut c_void,
    callback: unsafe extern "C" fn(ctx: *mut c_void, entry: *const RustFfiPreloadScannerEntry) -> bool,
) {
    let Some(scanner) = scanner else {
        return;
    };
    scanner.scan(&mut |entry| unsafe { callback(ctx, &raw const *entry) });
}

#[cfg(test)]
pub(crate) fn scan(input: &[u8], scripting_enabled: bool, callback: impl FnMut(&RustFfiPreloadScannerEntry) -> bool) {
    // SAFETY: The tests pass valid UTF-8.
    let code_units: Vec<u16> = unsafe { std::str::from_utf8_unchecked(input) }.encode_utf16().collect();
    scan_utf16(&code_units, scripting_enabled, callback);
}

#[cfg(test)]
pub(crate) fn scan_utf16(
    input: &[u16],
    scripting_enabled: bool,
    mut callback: impl FnMut(&RustFfiPreloadScannerEntry) -> bool,
) {
    let mut scanner = RustFfiPreloadScanner::new(scripting_enabled);
    scanner.append_input(Utf16View::Utf16(input));
    scanner.tokenizer.set_input_stream_closed(true);
    scanner.scan(&mut callback);
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
        // A self-closing svg or math element leaves no element open.
        if !token.is_self_closing() {
            *foreign_depth = foreign_depth.saturating_add(1);
        }
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

pub(crate) fn tokenizer_state_after_start_tag(tag_name: &HtmlName, scripting_enabled: bool) -> Option<State> {
    if *tag_name == tag_name!("script") {
        Some(State::ScriptData)
    } else if *tag_name == tag_name!("title") || *tag_name == tag_name!("textarea") {
        Some(State::RCDATA)
    } else if *tag_name == tag_name!("style")
        || *tag_name == tag_name!("xmp")
        || *tag_name == tag_name!("iframe")
        || *tag_name == tag_name!("noembed")
        || *tag_name == tag_name!("noframes")
        || (*tag_name == tag_name!("noscript") && scripting_enabled)
    {
        Some(State::RAWTEXT)
    } else if *tag_name == tag_name!("plaintext") {
        Some(State::PLAINTEXT)
    } else {
        None
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
    //    Otherwise, if el has a type attribute, then let the script block's type string be the value of that attribute.
    //    Otherwise, el has a non-empty language attribute; let the script block's type string be the concatenation of
    //    "text/" and the value of el's language attribute.
    // FIXME: Preserve whitespace in the type string; only the classic JavaScript MIME comparison strips it.
    let type_attribute = type_attribute.map(keyword_bytes);
    let language_attribute = language_attribute.map(keyword_bytes);
    let type_string: Cow<'_, [u8]> = match (&type_attribute, &language_attribute) {
        (Some(type_attribute), _) if type_attribute.is_empty() => Cow::Borrowed(b"text/javascript"),
        (None, Some(language)) if language.is_empty() => Cow::Borrowed(b"text/javascript"),
        (None, None) => Cow::Borrowed(b"text/javascript"),
        (Some(type_attribute), _) => Cow::Borrowed(type_attribute.trim_ascii()),
        (None, Some(language)) => Cow::Owned([b"text/".as_slice(), language].concat()),
    };

    // 9. If the script block's type string is a JavaScript MIME type essence match, or el has a type attribute whose
    //    value with leading and trailing ASCII whitespace stripped is a JavaScript MIME type essence match, then set
    //    el's type to "classic".
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
    let rel = keyword_bytes(rel);

    let (action, destination) = if rel_contains_keyword(&rel, b"stylesheet") {
        (
            RustFfiPreloadScannerAction::Fetch,
            RustFfiPreloadScannerDestination::Style,
        )
    } else if rel_contains_keyword(&rel, b"modulepreload") {
        let Some(destination) = translate_modulepreload_destination(attribute_value(attributes, attribute_name!("as")))
        else {
            return true;
        };
        (RustFfiPreloadScannerAction::ModulePreload, destination)
    } else if rel_contains_keyword(&rel, b"preload") {
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
    url: Utf16View<'_>,
    destination: RustFfiPreloadScannerDestination,
    cors_setting: RustFfiPreloadScannerCorsSetting,
    attributes: Option<&[Attribute]>,
) -> bool {
    let attribute = |name| attribute_value(attributes.unwrap_or_default(), name);
    let attribute_or_empty = |name| attribute(name).unwrap_or(Utf16View::Latin1(&[])).into();
    let entry = RustFfiPreloadScannerEntry {
        action,
        url: url.into(),
        destination,
        cors_setting,
        nonce: attribute_or_empty(attribute_name!("nonce")),
        integrity: attribute_or_empty(attribute_name!("integrity")),
        integrity_present: attribute(attribute_name!("integrity")).is_some(),
        referrer_policy: attribute_or_empty(attribute_name!("referrerpolicy")),
        fetch_priority: attribute_or_empty(attribute_name!("fetchpriority")),
        media: attribute_or_empty(attribute_name!("media")),
    };
    callback(&entry)
}

fn attribute_value(attributes: &[Attribute], name: KnownName) -> Option<Utf16View<'_>> {
    attributes
        .iter()
        .find(|attribute| attribute.local_name == name)
        .map(|attribute| attribute.value.as_slice())
}

/// The code units of an attribute value as bytes, to look for ASCII keywords in. A code unit that doesn't fit in a
/// byte becomes one that is in no keyword and isn't whitespace.
fn keyword_bytes(value: Utf16View<'_>) -> Cow<'_, [u8]> {
    match value {
        Utf16View::Latin1(code_units) => Cow::Borrowed(code_units),
        Utf16View::Utf16(code_units) => Cow::Owned(
            code_units
                .iter()
                .map(|&code_unit| u8::try_from(code_unit).unwrap_or(0xFF))
                .collect(),
        ),
    }
}

fn rel_contains_keyword(rel: &[u8], keyword: &[u8]) -> bool {
    rel.split(|byte| byte.is_ascii_whitespace())
        .any(|token| token.eq_ignore_ascii_case(keyword))
}

fn translate_preload_destination(destination: Option<Utf16View<'_>>) -> Option<RustFfiPreloadScannerDestination> {
    Some(match &*keyword_bytes(destination?) {
        b"fetch" => RustFfiPreloadScannerDestination::None,
        b"font" => RustFfiPreloadScannerDestination::Font,
        b"image" => RustFfiPreloadScannerDestination::Image,
        b"script" => RustFfiPreloadScannerDestination::Script,
        b"style" => RustFfiPreloadScannerDestination::Style,
        b"track" => RustFfiPreloadScannerDestination::Track,
        _ => return None,
    })
}

fn translate_modulepreload_destination(destination: Option<Utf16View<'_>>) -> Option<RustFfiPreloadScannerDestination> {
    let Some(destination) = destination else {
        return Some(RustFfiPreloadScannerDestination::Script);
    };

    let destination = &*keyword_bytes(destination);

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

    if crossorigin.eq_ignore_ascii_case("use-credentials") {
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
        collect_with_scripting(input, true)
    }

    fn collect_with_scripting(input: &str, scripting_enabled: bool) -> Vec<ScannedEntry> {
        let mut entries = Vec::new();
        scan(input.as_bytes(), scripting_enabled, |entry| {
            entries.push(ScannedEntry {
                action: entry.action,
                url: entry.url.to_test_string(),
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
        scan_utf16(&input, true, |entry| {
            entries.push(ScannedEntry {
                action: entry.action,
                url: entry.url.to_test_string(),
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

    fn collect_urls(input: &str, scripting_enabled: bool) -> Vec<String> {
        collect_with_scripting(input, scripting_enabled)
            .into_iter()
            .map(|entry| entry.url)
            .collect()
    }

    #[test]
    fn does_not_scan_markup_inside_script_text() {
        let urls = collect_urls(
            r#"
                <script>
                    var g = ["", "app.js"];
                    document.write('<script src="https://example.com/' +
                        g[1] +
                        '"><\/script>');
                </script>
                <img src="./after.png">
            "#,
            true,
        );

        assert_eq!(urls, vec!["./after.png".to_string()]);
    }

    #[test]
    fn does_not_scan_markup_inside_raw_text_and_rcdata_elements() {
        let urls = collect_urls(
            r#"
                <style>/* <img src="./style.png"> */</style>
                <title><img src="./title.png"></title>
                <textarea><img src="./textarea.png"></textarea>
                <xmp><script src="./xmp.js"></script></xmp>
                <iframe><img src="./iframe.png"></iframe>
                <noembed><img src="./noembed.png"></noembed>
                <noframes><img src="./noframes.png"></noframes>
                <img src="./after.png">
            "#,
            true,
        );

        assert_eq!(urls, vec!["./after.png".to_string()]);
    }

    #[test]
    fn noscript_content_is_raw_text_only_when_scripting_is_enabled() {
        let input = r#"<noscript><img src="./fallback.png"></noscript>"#;

        assert_eq!(collect_urls(input, true), Vec::<String>::new());
        assert_eq!(collect_urls(input, false), vec!["./fallback.png".to_string()]);
    }

    #[test]
    fn script_text_cannot_close_a_template() {
        let urls = collect_urls(
            r#"<template><script>"</template><img src="./trap.png">"</script></template><img src="./after.png">"#,
            true,
        );

        assert_eq!(urls, vec!["./after.png".to_string()]);
    }

    #[test]
    fn foreign_content_elements_do_not_switch_tokenizer_state() {
        let urls = collect_urls(r#"<svg><title>SVG</svg><img src="./after.png">"#, true);

        assert_eq!(urls, vec!["./after.png".to_string()]);
    }

    #[test]
    fn plaintext_ends_markup() {
        let urls = collect_urls(r#"<plaintext><img src="./trap.png">"#, true);

        assert_eq!(urls, Vec::<String>::new());
    }

    #[test]
    fn self_closing_foreign_root_leaves_nothing_open() {
        let urls = collect_urls(r#"<svg/><math/><img src="./after.png">"#, true);

        assert_eq!(urls, vec!["./after.png".to_string()]);
    }

    #[test]
    fn preserves_modulepreload_fetch_options() {
        let mut options = None;
        scan(
            br#"<link rel="modulepreload" href="./module" nonce="abc" integrity="sha256-xyz" referrerpolicy="origin" fetchpriority="high" media="screen">"#,
            true,
            |entry| {
                options = Some((
                    entry.nonce.to_test_string(),
                    entry.integrity.to_test_string(),
                    entry.integrity_present,
                    entry.referrer_policy.to_test_string(),
                    entry.fetch_priority.to_test_string(),
                    entry.media.to_test_string(),
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

    fn code_units(input: &str) -> Vec<u16> {
        input.encode_utf16().collect()
    }

    fn scan_urls(scanner: &mut RustFfiPreloadScanner) -> Vec<String> {
        let mut urls = Vec::new();
        scanner.scan(&mut |entry| {
            urls.push(entry.url.to_test_string());
            true
        });
        urls
    }

    #[test]
    fn goes_on_from_where_it_stopped() {
        let mut scanner = RustFfiPreloadScanner::new(true);

        // The input runs out inside a tag, and then inside the text of a script.
        scanner.append_input(Utf16View::Utf16(&code_units(r#"<img src="./a.png"><img sr"#)));
        assert_eq!(scan_urls(&mut scanner), vec!["./a.png"]);

        scanner.append_input(Utf16View::Utf16(&code_units(
            r#"c="./b.png"><script>var trap = '<img src="./tr"#,
        )));
        assert_eq!(scan_urls(&mut scanner), vec!["./b.png"]);

        scanner.append_input(Utf16View::Utf16(&code_units(
            r#"ap.png">';</script><img src="./c.png">"#,
        )));
        assert_eq!(scan_urls(&mut scanner), vec!["./c.png"]);
    }

    #[test]
    fn hands_on_only_input_not_scanned_yet() {
        let mut parser_tokenizer = HtmlTokenizer::new(code_units("<img src=a>"));
        parser_tokenizer.set_input_stream_closed(false);

        let (input, continues) = parser_tokenizer.input_for_speculative_scan();
        assert_eq!(input.to_utf16(), code_units("<img src=a>"));
        assert!(!continues);

        parser_tokenizer.append_input(Utf16View::Utf16(&code_units("<img src=b>")));
        let (input, continues) = parser_tokenizer.input_for_speculative_scan();
        assert_eq!(input.to_utf16(), code_units("<img src=b>"));
        assert!(continues);

        // Once the parser gets past the scanned input, the scan starts over where the parser is.
        while parser_tokenizer.next_token(false, false).is_some() {}
        parser_tokenizer.append_input(Utf16View::Utf16(&code_units("<img src=c>")));
        let (input, continues) = parser_tokenizer.input_for_speculative_scan();
        assert_eq!(input.to_utf16(), code_units("<img src=c>"));
        assert!(continues);
        parser_tokenizer.append_input(Utf16View::Utf16(&code_units("<img src=d><img src=e>")));
        while parser_tokenizer.next_token(false, false).is_some() {}
        parser_tokenizer.parser_did_run();
        parser_tokenizer.append_input(Utf16View::Utf16(&code_units("<img src=f>")));
        let (input, continues) = parser_tokenizer.input_for_speculative_scan();
        assert_eq!(input.to_utf16(), code_units("<img src=f>"));
        assert!(!continues);
    }

    #[test]
    fn starts_over_after_input_is_inserted_before_the_scanned_input() {
        let mut parser_tokenizer = HtmlTokenizer::new(code_units("<script></script><img src=a>"));
        parser_tokenizer.set_input_stream_closed(false);
        parser_tokenizer.next_token(false, false);
        parser_tokenizer.next_token(false, false);
        parser_tokenizer.update_insertion_point();

        let (input, _) = parser_tokenizer.input_for_speculative_scan();
        assert_eq!(input.to_utf16(), code_units("<img src=a>"));

        // Input that document.write() inserts at the insertion point comes before the scanned input, so the scan has
        // to take it in too.
        parser_tokenizer.insert_input_at_insertion_point(Utf16View::Utf16(&code_units("<img src=w>")));
        let (input, continues) = parser_tokenizer.input_for_speculative_scan();
        assert_eq!(input.to_utf16(), code_units("<img src=w><img src=a>"));
        assert!(!continues);
    }
}
