/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[cfg(feature = "allocator")]
#[path = "../../../../RustAllocator.rs"]
mod rust_allocator;

use std::collections::HashMap;
use std::collections::HashSet;
use std::ffi::c_void;
use std::panic::AssertUnwindSafe;
use std::panic::catch_unwind;

#[repr(C)]
pub struct ContentBlockerString {
    data: *mut u8,
    length: usize,
}

struct ContentBlockerEngine {
    engine: adblock::engine::Engine,
    generic_selector_list_rules: GenericSelectorListRules,
}

#[derive(Default)]
struct GenericSelectorListRules {
    always_needed: HashSet<String>,
    by_class: HashMap<String, Vec<String>>,
    by_id: HashMap<String, Vec<String>>,
}

enum SelectorKey {
    Class(String),
    Id(String),
}

fn abort_on_panic<F: FnOnce() -> R, R>(f: F) -> R {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(result) => result,
        Err(payload) => {
            let message = if let Some(message) = payload.downcast_ref::<&str>() {
                (*message).to_string()
            } else if let Some(message) = payload.downcast_ref::<String>() {
                message.clone()
            } else {
                "unknown panic".to_string()
            };
            eprintln!("Rust panic at FFI boundary: {message}");
            std::process::abort();
        }
    }
}

unsafe fn bytes_from_raw<'a>(bytes: *const u8, len: usize) -> Option<&'a [u8]> {
    if len == 0 {
        return Some(&[]);
    }
    if bytes.is_null() {
        return None;
    }
    Some(unsafe { std::slice::from_raw_parts(bytes, len) })
}

unsafe fn string_from_raw<'a>(bytes: *const u8, len: usize) -> Option<&'a str> {
    let bytes = unsafe { bytes_from_raw(bytes, len)? };
    std::str::from_utf8(bytes).ok()
}

unsafe fn engine_from_raw<'a>(engine: *const c_void) -> Option<&'a ContentBlockerEngine> {
    if engine.is_null() {
        return None;
    }
    Some(unsafe { &*engine.cast::<ContentBlockerEngine>() })
}

fn string_to_ffi(string: String) -> ContentBlockerString {
    if string.is_empty() {
        return ContentBlockerString {
            data: std::ptr::null_mut(),
            length: 0,
        };
    }

    let bytes = Box::leak(string.into_bytes().into_boxed_slice());
    let data = bytes.as_mut_ptr();
    let length = bytes.len();

    ContentBlockerString { data, length }
}

fn for_each_selector_list_item(selector: &str, mut callback: impl FnMut(&str)) {
    let mut start = 0;
    let mut parenthesis_depth: u32 = 0;
    let mut bracket_depth: u32 = 0;
    let mut quote = None;
    let mut escaped = false;

    for (index, character) in selector.char_indices() {
        if escaped {
            escaped = false;
            continue;
        }

        if character == '\\' {
            escaped = true;
            continue;
        }

        if let Some(quote_character) = quote {
            if character == quote_character {
                quote = None;
            }
            continue;
        }

        match character {
            '"' | '\'' => quote = Some(character),
            '(' => parenthesis_depth += 1,
            ')' => parenthesis_depth = parenthesis_depth.saturating_sub(1),
            '[' => bracket_depth += 1,
            ']' => bracket_depth = bracket_depth.saturating_sub(1),
            ',' if parenthesis_depth == 0 && bracket_depth == 0 => {
                callback(&selector[start..index]);
                start = index + character.len_utf8();
            }
            _ => {}
        }
    }

    callback(&selector[start..]);
}

fn selector_key_from_start(selector: &str) -> Option<SelectorKey> {
    let selector = selector.trim_start();
    let marker = selector.chars().next()?;
    if marker != '.' && marker != '#' {
        return None;
    }

    let mut end = marker.len_utf8();
    for (offset, character) in selector[end..].char_indices() {
        if character == '\\' {
            return None;
        }
        if !character.is_alphanumeric() && character != '_' && character != '-' {
            break;
        }
        end = marker.len_utf8() + offset + character.len_utf8();
    }

    if end == marker.len_utf8() {
        return None;
    }

    let key = selector[marker.len_utf8()..end].to_string();
    match marker {
        '.' => Some(SelectorKey::Class(key)),
        '#' => Some(SelectorKey::Id(key)),
        _ => None,
    }
}

impl GenericSelectorListRules {
    fn from_rules(rules: &str) -> Self {
        let mut selector_list_rules = Self::default();

        for line in rules.lines() {
            let trimmed_line = line.trim();
            if trimmed_line.is_empty() || trimmed_line.starts_with('!') || trimmed_line.starts_with('[') {
                continue;
            }

            let Ok(filter) = adblock::filters::cosmetic::CosmeticFilter::parse(
                trimmed_line,
                false,
                adblock::resources::PermissionMask::default(),
            ) else {
                continue;
            };

            let generic_filter = if filter.has_hostname_constraint() {
                filter.hidden_generic_rule()
            } else {
                Some(filter)
            };

            let Some(generic_filter) = generic_filter else {
                continue;
            };
            let Some(selector) = generic_filter.plain_css_selector() else {
                continue;
            };
            selector_list_rules.add_selector(selector);
        }

        selector_list_rules
    }

    fn add_selector(&mut self, selector: &str) {
        let mut selector_list_item_count = 0;
        let mut has_unkeyable_item = false;
        let mut class_keys = HashSet::new();
        let mut id_keys = HashSet::new();
        for_each_selector_list_item(selector, |selector_list_item| {
            selector_list_item_count += 1;

            match selector_key_from_start(selector_list_item) {
                Some(SelectorKey::Class(class)) => {
                    class_keys.insert(class);
                }
                Some(SelectorKey::Id(id)) => {
                    id_keys.insert(id);
                }
                None => has_unkeyable_item = true,
            }
        });

        if selector_list_item_count < 2 {
            return;
        }

        let selector = selector.to_string();
        if has_unkeyable_item {
            self.always_needed.insert(selector);
            return;
        }

        for class in class_keys {
            self.by_class.entry(class).or_default().push(selector.clone());
        }
        for id in id_keys {
            self.by_id.entry(id).or_default().push(selector.clone());
        }
    }

    fn hidden_selectors(
        &self,
        classes: impl IntoIterator<Item = impl AsRef<str>>,
        ids: impl IntoIterator<Item = impl AsRef<str>>,
        exceptions: &HashSet<String>,
    ) -> Vec<String> {
        let mut selectors: Vec<_> = self
            .always_needed
            .iter()
            .filter(|selector| !exceptions.contains(*selector))
            .cloned()
            .collect();

        for class in classes {
            if let Some(class_selectors) = self.by_class.get(class.as_ref()) {
                selectors.extend(
                    class_selectors
                        .iter()
                        .filter(|selector| !exceptions.contains(*selector))
                        .cloned(),
                );
            }
        }

        for id in ids {
            if let Some(id_selectors) = self.by_id.get(id.as_ref()) {
                selectors.extend(
                    id_selectors
                        .iter()
                        .filter(|selector| !exceptions.contains(*selector))
                        .cloned(),
                );
            }
        }

        selectors
    }

    fn has_hidden_selectors_for_class_or_id(
        &self,
        classes: impl IntoIterator<Item = impl AsRef<str>>,
        ids: impl IntoIterator<Item = impl AsRef<str>>,
        exceptions: &HashSet<String>,
    ) -> bool {
        for class in classes {
            if let Some(class_selectors) = self.by_class.get(class.as_ref())
                && class_selectors.iter().any(|selector| !exceptions.contains(selector))
            {
                return true;
            }
        }

        for id in ids {
            if let Some(id_selectors) = self.by_id.get(id.as_ref())
                && id_selectors.iter().any(|selector| !exceptions.contains(selector))
            {
                return true;
            }
        }

        false
    }
}

fn cosmetic_css_for_url(engine: &ContentBlockerEngine, url: &str, classes: &[&str], ids: &[&str]) -> String {
    let resources = engine.engine.url_cosmetic_resources(url);
    let mut selectors = resources.hide_selectors;

    if !resources.generichide {
        selectors.extend(engine.engine.hidden_class_id_selectors(
            classes.iter().copied(),
            ids.iter().copied(),
            &resources.exceptions,
        ));
        selectors.extend(engine.generic_selector_list_rules.hidden_selectors(
            classes.iter().copied(),
            ids.iter().copied(),
            &resources.exceptions,
        ));
    }

    let mut selector_styles: Vec<_> = selectors
        .into_iter()
        .map(|selector| (selector, "display: none !important".to_string()))
        .collect();

    for action in resources.procedural_actions {
        let Ok(filter) = serde_json::from_str::<adblock::cosmetic_filter_cache::ProceduralOrActionFilter>(&action)
        else {
            continue;
        };
        if let Some((selector, style)) = filter.as_css() {
            selector_styles.push((selector, style));
        }
    }

    selector_styles.sort_unstable();
    selector_styles.dedup();

    let mut css = String::new();
    for (selector, style) in selector_styles {
        css.push_str(&selector);
        css.push_str(" { ");
        css.push_str(&style);
        if !style.trim_end().ends_with(';') {
            css.push(';');
        }
        css.push_str(" }\n");
    }
    css
}

fn has_generic_cosmetic_selectors_for_url(
    engine: &ContentBlockerEngine,
    url: &str,
    classes: &[&str],
    ids: &[&str],
) -> bool {
    let resources = engine.engine.url_cosmetic_resources(url);
    if resources.generichide {
        return false;
    }

    !engine
        .engine
        .hidden_class_id_selectors(classes.iter().copied(), ids.iter().copied(), &resources.exceptions)
        .is_empty()
        || engine.generic_selector_list_rules.has_hidden_selectors_for_class_or_id(
            classes.iter().copied(),
            ids.iter().copied(),
            &resources.exceptions,
        )
}

/// # Safety
/// - `rules` and `rules_len` must point to a valid UTF-8 string
/// - The returned pointer must be freed with `rust_content_blocker_free`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_content_blocker_create(rules: *const u8, rules_len: usize) -> *mut c_void {
    abort_on_panic(|| {
        let Some(rules) = (unsafe { string_from_raw(rules, rules_len) }) else {
            return std::ptr::null_mut();
        };

        let engine = adblock::engine::Engine::from_rules(rules.lines(), adblock::lists::ParseOptions::default());
        let generic_selector_list_rules = GenericSelectorListRules::from_rules(rules);
        let engine = ContentBlockerEngine {
            engine,
            generic_selector_list_rules,
        };
        Box::into_raw(Box::new(engine)).cast()
    })
}

/// # Safety
/// - `engine` must be null or a valid pointer returned by `rust_content_blocker_create`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_content_blocker_free(engine: *mut c_void) {
    abort_on_panic(|| {
        if !engine.is_null() {
            drop(unsafe { Box::from_raw(engine.cast::<ContentBlockerEngine>()) });
        }
    });
}

/// # Safety
/// - `engine` must be null or a valid pointer returned by `rust_content_blocker_create`
/// - String pointers and lengths must point to valid UTF-8 strings
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_content_blocker_matches(
    engine: *const c_void,
    url: *const u8,
    url_len: usize,
    source_url: *const u8,
    source_url_len: usize,
    request_type: *const u8,
    request_type_len: usize,
) -> bool {
    abort_on_panic(|| {
        let Some(engine) = (unsafe { engine_from_raw(engine) }) else {
            return false;
        };
        let Some(url) = (unsafe { string_from_raw(url, url_len) }) else {
            return false;
        };
        let Some(source_url) = (unsafe { string_from_raw(source_url, source_url_len) }) else {
            return false;
        };
        let Some(request_type) = (unsafe { string_from_raw(request_type, request_type_len) }) else {
            return false;
        };

        let Ok(request) = adblock::request::Request::new(url, source_url, request_type) else {
            return false;
        };

        engine.engine.check_network_request(&request).matched
    })
}

/// # Safety
/// - `engine` must be null or a valid pointer returned by `rust_content_blocker_create`
/// - String pointers and lengths must point to valid UTF-8 strings
/// - The returned string must be freed with `rust_content_blocker_free_string`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_content_blocker_cosmetic_css(
    engine: *const c_void,
    url: *const u8,
    url_len: usize,
    classes: *const u8,
    classes_len: usize,
    ids: *const u8,
    ids_len: usize,
) -> ContentBlockerString {
    abort_on_panic(|| {
        let Some(engine) = (unsafe { engine_from_raw(engine) }) else {
            return string_to_ffi(String::new());
        };
        let Some(url) = (unsafe { string_from_raw(url, url_len) }) else {
            return string_to_ffi(String::new());
        };
        let Some(classes) = (unsafe { string_from_raw(classes, classes_len) }) else {
            return string_to_ffi(String::new());
        };
        let Some(ids) = (unsafe { string_from_raw(ids, ids_len) }) else {
            return string_to_ffi(String::new());
        };

        let classes: Vec<_> = classes.lines().collect();
        let ids: Vec<_> = ids.lines().collect();
        string_to_ffi(cosmetic_css_for_url(engine, url, &classes, &ids))
    })
}

/// # Safety
/// - `engine` must be null or a valid pointer returned by `rust_content_blocker_create`
/// - String pointers and lengths must point to valid UTF-8 strings
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_content_blocker_has_generic_cosmetic_selectors(
    engine: *const c_void,
    url: *const u8,
    url_len: usize,
    classes: *const u8,
    classes_len: usize,
    ids: *const u8,
    ids_len: usize,
) -> bool {
    abort_on_panic(|| {
        let Some(engine) = (unsafe { engine_from_raw(engine) }) else {
            return false;
        };
        let Some(url) = (unsafe { string_from_raw(url, url_len) }) else {
            return false;
        };
        let Some(classes) = (unsafe { string_from_raw(classes, classes_len) }) else {
            return false;
        };
        let Some(ids) = (unsafe { string_from_raw(ids, ids_len) }) else {
            return false;
        };

        let classes: Vec<_> = classes.lines().collect();
        let ids: Vec<_> = ids.lines().collect();
        has_generic_cosmetic_selectors_for_url(engine, url, &classes, &ids)
    })
}

/// # Safety
/// - `data` and `length` must match a string returned by `rust_content_blocker_cosmetic_css`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_content_blocker_free_string(data: *mut u8, length: usize) {
    abort_on_panic(|| {
        if !data.is_null() {
            drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(data, length)) });
        }
    });
}
