/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::panic::{AssertUnwindSafe, catch_unwind};

#[repr(C)]
pub struct ContentBlockerString {
    data: *mut u8,
    length: usize,
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

unsafe fn engine_from_raw<'a>(engine: *const c_void) -> Option<&'a adblock::engine::Engine> {
    if engine.is_null() {
        return None;
    }
    Some(unsafe { &*engine.cast::<adblock::engine::Engine>() })
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

fn cosmetic_css_for_url(
    engine: &adblock::engine::Engine,
    url: &str,
    classes: impl IntoIterator<Item = impl AsRef<str>>,
    ids: impl IntoIterator<Item = impl AsRef<str>>,
) -> String {
    let resources = engine.url_cosmetic_resources(url);
    let mut selectors = resources.hide_selectors;

    if !resources.generichide {
        selectors.extend(engine.hidden_class_id_selectors(classes, ids, &resources.exceptions));
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
        Box::into_raw(Box::new(engine)).cast()
    })
}

/// # Safety
/// - `engine` must be null or a valid pointer returned by `rust_content_blocker_create`
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_content_blocker_free(engine: *mut c_void) {
    abort_on_panic(|| {
        if !engine.is_null() {
            drop(unsafe { Box::from_raw(engine.cast::<adblock::engine::Engine>()) });
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

        engine.check_network_request(&request).matched
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

        string_to_ffi(cosmetic_css_for_url(engine, url, classes.lines(), ids.lines()))
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
