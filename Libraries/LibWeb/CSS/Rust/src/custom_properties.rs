/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rust-owned custom-property data with the same structurally shared parent shape as the C++
//! `CustomPropertyData` shell.

use std::collections::HashMap;
use std::collections::HashSet;
use std::ffi::c_void;
use std::rc::Rc;

use crate::abort_on_panic;
use crate::css_tokenizer::OwnedToken;
use crate::css_tokenizer::OwnedTokenKind;
use crate::css_tokenizer::tokenize_owned;
use crate::style_value::RetainedStyleValue;
use crate::style_value::RetainedUtf16FlyString;
use crate::style_value::StyleValueData;

#[repr(C)]
pub struct FfiCustomPropertyStoreEntry {
    pub name_raw: usize,
    pub name_utf8: *const u8,
    pub name_utf8_length: usize,
    pub important: bool,
    pub shell: *const c_void,
    pub data: *const c_void,
}

struct CustomPropertyEntry {
    _name: RetainedUtf16FlyString,
    _value: RetainedStyleValue,
    important: bool,
    data: *const c_void,
}

pub struct CustomPropertyStore {
    own_values: HashMap<usize, CustomPropertyEntry>,
    own_names: HashMap<String, usize>,
    parent: Option<Rc<CustomPropertyStore>>,
}

pub struct CustomPropertyRegistry {
    registrations: HashMap<String, RegisteredCustomProperty>,
}

struct RegisteredCustomProperty {
    syntax_is_universal: bool,
    _inherits: bool,
    initial_source: Option<Vec<u8>>,
}

#[repr(C)]
pub struct FfiCustomPropertyRegistration {
    pub name: *const u8,
    pub name_length: usize,
    pub syntax_is_universal: bool,
    pub inherits: bool,
    pub has_initial_value: bool,
    pub initial_value: *const u8,
    pub initial_value_length: usize,
}

impl CustomPropertyStore {
    fn get(&self, name_raw: usize) -> Option<&CustomPropertyEntry> {
        self.own_values
            .get(&name_raw)
            .or_else(|| self.parent.as_ref()?.get(name_raw))
    }

    fn get_by_name(&self, name: &str) -> Option<&CustomPropertyEntry> {
        self.own_names
            .get(name)
            .and_then(|name_raw| self.own_values.get(name_raw))
            .or_else(|| self.parent.as_ref()?.get_by_name(name))
    }
}

const MAX_SUBSTITUTED_TOKEN_COUNT: usize = 16384;

pub(crate) enum NativeVarResolution {
    Resolved(Vec<u8>),
    Invalid,
    NotHandled,
}

enum TokenResolution {
    Resolved(Vec<OwnedToken>),
    Invalid,
    NotHandled,
}

fn matching_close(kind: &OwnedTokenKind) -> Option<OwnedTokenKind> {
    match kind {
        OwnedTokenKind::Function(_) | OwnedTokenKind::OpenParen => Some(OwnedTokenKind::CloseParen),
        OwnedTokenKind::OpenSquare => Some(OwnedTokenKind::CloseSquare),
        OwnedTokenKind::OpenCurly => Some(OwnedTokenKind::CloseCurly),
        _ => None,
    }
}

fn find_matching_close(tokens: &[OwnedToken], open_index: usize) -> Option<usize> {
    let mut expected_closes = vec![matching_close(&tokens[open_index].kind)?];
    for (index, token) in tokens.iter().enumerate().skip(open_index + 1) {
        if let Some(close) = matching_close(&token.kind) {
            expected_closes.push(close);
            continue;
        }
        if expected_closes.last() == Some(&token.kind) {
            expected_closes.pop();
            if expected_closes.is_empty() {
                return Some(index);
            }
        }
    }
    None
}

fn find_top_level_comma(tokens: &[OwnedToken]) -> Option<usize> {
    let mut index = 0;
    while index < tokens.len() {
        if matches!(tokens[index].kind, OwnedTokenKind::Comma) {
            return Some(index);
        }
        if matching_close(&tokens[index].kind).is_some() {
            index = find_matching_close(tokens, index)? + 1;
        } else {
            index += 1;
        }
    }
    None
}

fn trim_whitespace(mut tokens: &[OwnedToken]) -> &[OwnedToken] {
    while matches!(
        tokens.first().map(|token| &token.kind),
        Some(OwnedTokenKind::Whitespace)
    ) {
        tokens = &tokens[1..];
    }
    while matches!(tokens.last().map(|token| &token.kind), Some(OwnedTokenKind::Whitespace)) {
        tokens = &tokens[..tokens.len() - 1];
    }
    tokens
}

fn is_single_css_wide_keyword(tokens: &[OwnedToken]) -> bool {
    let [
        OwnedToken {
            kind: OwnedTokenKind::Ident(keyword),
            ..
        },
    ] = trim_whitespace(tokens)
    else {
        return false;
    };
    keyword.eq_ignore_ascii_case("inherit")
        || keyword.eq_ignore_ascii_case("initial")
        || keyword.eq_ignore_ascii_case("unset")
        || keyword.eq_ignore_ascii_case("revert")
        || keyword.eq_ignore_ascii_case("revert-layer")
}

fn resolve_custom_property(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    name: &str,
    guarded_names: &mut HashSet<String>,
) -> TokenResolution {
    let registration = registry.and_then(|registry| registry.registrations.get(name));
    if registration.is_some_and(|registration| !registration.syntax_is_universal) {
        return TokenResolution::NotHandled;
    }
    let Some(entry) = store.and_then(|store| store.get_by_name(name)) else {
        return registration
            .and_then(|registration| registration.initial_source.as_ref())
            .map_or(TokenResolution::Invalid, |source| {
                TokenResolution::Resolved(tokenize_owned(source))
            });
    };
    let data = unsafe { &*entry.data.cast::<StyleValueData>() };
    if matches!(data, StyleValueData::GuaranteedInvalid) {
        return TokenResolution::Invalid;
    }
    let Some((source, includes_var)) = data.unresolved_var_source() else {
        return TokenResolution::NotHandled;
    };
    if registration.is_some() && includes_var {
        // NB: Cycles involving registered properties affect the registered property's computed
        // value and fallback behavior. Keep those on the registered-property computation path.
        return TokenResolution::NotHandled;
    }
    if !guarded_names.insert(name.to_owned()) {
        return TokenResolution::Invalid;
    }
    let result = substitute_tokens(store, registry, &tokenize_owned(source), guarded_names);
    guarded_names.remove(name);
    if let TokenResolution::Resolved(tokens) = &result
        && is_single_css_wide_keyword(tokens)
    {
        // NB: A CSS-wide keyword produced by substitution must first take on its custom-property
        // meaning. Keep this case on the C++ path until custom-property keyword resolution moves.
        return TokenResolution::NotHandled;
    }
    result
}

fn replace_var_function(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    arguments: &[OwnedToken],
    guarded_names: &mut HashSet<String>,
) -> TokenResolution {
    // https://drafts.csswg.org/css-variables-1/#replace-a-var-function
    // 1. Let el be the element that the style containing the var() function is being applied to.
    //    Let first arg be the first <declaration-value> in arguments.
    //    Let second arg be the <declaration-value>? passed after the comma, or null if there was no comma.
    let comma = find_top_level_comma(arguments);
    let first_argument = trim_whitespace(&arguments[..comma.unwrap_or(arguments.len())]);
    let [
        OwnedToken {
            kind: OwnedTokenKind::Ident(name),
            ..
        },
    ] = first_argument
    else {
        return TokenResolution::NotHandled;
    };
    if !name.starts_with("--") {
        return TokenResolution::Invalid;
    }

    // 2. Substitute arbitrary substitution functions in first arg, then parse it as a <custom-property-name>.
    //    If parsing returned a <custom-property-name>, let result be the computed value of the corresponding custom
    //    property on el. Otherwise, let result be the guaranteed-invalid value.
    let result = resolve_custom_property(store, registry, name, guarded_names);
    if !matches!(result, TokenResolution::Invalid) {
        return result;
    }
    let Some(comma) = comma else {
        return TokenResolution::Invalid;
    };
    // 4. If result contains the guaranteed-invalid value, and second arg was provided, set result to the result of
    //    substitute arbitrary substitution functions on second arg.
    substitute_tokens(store, registry, &arguments[comma + 1..], guarded_names)
}

fn substitute_tokens(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    tokens: &[OwnedToken],
    guarded_names: &mut HashSet<String>,
) -> TokenResolution {
    let mut output = Vec::new();
    let mut index = 0;
    while index < tokens.len() {
        let Some(close_index) = matching_close(&tokens[index].kind).and_then(|_| find_matching_close(tokens, index))
        else {
            output.push(tokens[index].clone());
            index += 1;
            continue;
        };

        let contents = &tokens[index + 1..close_index];
        let resolved = match &tokens[index].kind {
            OwnedTokenKind::Function(name) if name.eq_ignore_ascii_case("var") => {
                replace_var_function(store, registry, contents, guarded_names)
            }
            _ => substitute_tokens(store, registry, contents, guarded_names),
        };
        let resolved = match resolved {
            TokenResolution::Resolved(resolved) => resolved,
            TokenResolution::Invalid => return TokenResolution::Invalid,
            TokenResolution::NotHandled => return TokenResolution::NotHandled,
        };

        if !matches!(tokens[index].kind, OwnedTokenKind::Function(ref name) if name.eq_ignore_ascii_case("var")) {
            output.push(tokens[index].clone());
        }
        output.extend(resolved);
        if !matches!(tokens[index].kind, OwnedTokenKind::Function(ref name) if name.eq_ignore_ascii_case("var")) {
            output.push(tokens[close_index].clone());
        }
        let remaining_token_count = tokens.len() - close_index - 1;
        if output.len() + remaining_token_count > MAX_SUBSTITUTED_TOKEN_COUNT {
            return TokenResolution::Invalid;
        }
        index = close_index + 1;
    }
    TokenResolution::Resolved(output)
}

// https://drafts.csswg.org/css-syntax/#serialization
fn needs_comment_between(first: &OwnedTokenKind, second: &OwnedTokenKind) -> bool {
    let second_is_common = matches!(
        second,
        OwnedTokenKind::Url
            | OwnedTokenKind::BadUrl
            | OwnedTokenKind::Number
            | OwnedTokenKind::Percentage
            | OwnedTokenKind::Dimension
            | OwnedTokenKind::Cdc
    );
    let second_is_ident = matches!(second, OwnedTokenKind::Ident(_));
    let second_is_function = matches!(second, OwnedTokenKind::Function(_));
    let common = second_is_common || second_is_ident;

    match first {
        OwnedTokenKind::Ident(_) => {
            second_is_function || matches!(second, OwnedTokenKind::OpenParen | OwnedTokenKind::Delim(45)) || common
        }
        OwnedTokenKind::AtKeyword
        | OwnedTokenKind::Hash
        | OwnedTokenKind::Dimension
        | OwnedTokenKind::Delim(35 | 45) => second_is_function || matches!(second, OwnedTokenKind::Delim(45)) || common,
        OwnedTokenKind::Number => second_is_function || matches!(second, OwnedTokenKind::Delim(37)) || common,
        OwnedTokenKind::Delim(64) => {
            second_is_function
                || matches!(second, OwnedTokenKind::Delim(45))
                || second_is_ident
                || matches!(
                    second,
                    OwnedTokenKind::Url | OwnedTokenKind::BadUrl | OwnedTokenKind::Cdc
                )
        }
        OwnedTokenKind::Delim(46 | 43) => matches!(
            second,
            OwnedTokenKind::Number | OwnedTokenKind::Percentage | OwnedTokenKind::Dimension
        ),
        OwnedTokenKind::Delim(47) => matches!(second, OwnedTokenKind::Delim(42)),
        _ => false,
    }
}

fn serialize_tokens(tokens: &[OwnedToken]) -> Vec<u8> {
    let mut output = Vec::new();
    for (index, token) in tokens.iter().enumerate() {
        output.extend_from_slice(&token.source);
        if let Some(next) = tokens.get(index + 1)
            && needs_comment_between(&token.kind, &next.kind)
        {
            output.extend_from_slice(b"/**/");
        }
    }
    output
}

pub(crate) unsafe fn resolve_vars(
    store: *const c_void,
    registry: *const c_void,
    value_data: *const c_void,
) -> NativeVarResolution {
    let store = if store.is_null() {
        None
    } else {
        Some(unsafe { &*store.cast::<CustomPropertyStore>() })
    };
    let registry = if registry.is_null() {
        None
    } else {
        Some(unsafe { &*registry.cast::<CustomPropertyRegistry>() })
    };
    let value_data = unsafe { &*value_data.cast::<StyleValueData>() };
    let Some((source, includes_var)) = value_data.unresolved_var_source() else {
        return NativeVarResolution::NotHandled;
    };
    if !includes_var {
        return NativeVarResolution::NotHandled;
    }
    match substitute_tokens(store, registry, &tokenize_owned(source), &mut HashSet::new()) {
        TokenResolution::Resolved(tokens) => NativeVarResolution::Resolved(serialize_tokens(&tokens)),
        TokenResolution::Invalid => NativeVarResolution::Invalid,
        TokenResolution::NotHandled => NativeVarResolution::NotHandled,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn substitute_without_custom_properties(source: &str) -> TokenResolution {
        substitute_tokens(None, None, &tokenize_owned(source.as_bytes()), &mut HashSet::new())
    }

    #[test]
    fn missing_variable_uses_fallback() {
        let TokenResolution::Resolved(tokens) = substitute_without_custom_properties("calc(var(--missing, 1px) + 2px)")
        else {
            panic!("expected fallback substitution");
        };
        assert_eq!(serialize_tokens(&tokens), b"calc( 1px + 2px)");
    }

    #[test]
    fn empty_fallback_is_valid() {
        let TokenResolution::Resolved(tokens) = substitute_without_custom_properties("var(--missing,)") else {
            panic!("expected empty fallback substitution");
        };
        assert!(tokens.is_empty());
    }

    #[test]
    fn missing_variable_without_fallback_is_invalid() {
        assert!(matches!(
            substitute_without_custom_properties("var(--missing)"),
            TokenResolution::Invalid
        ));
    }

    #[test]
    fn serialization_preserves_token_boundaries() {
        let TokenResolution::Resolved(tokens) = substitute_without_custom_properties("var(--missing, 1)px") else {
            panic!("expected fallback substitution");
        };
        assert_eq!(serialize_tokens(&tokens), b" 1/**/px");
    }

    #[test]
    fn recognizes_substituted_css_wide_keywords() {
        assert!(is_single_css_wide_keyword(&tokenize_owned(b"  inherit ")));
        assert!(!is_single_css_wide_keyword(&tokenize_owned(b"inherit green")));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_custom_property_registry_create() -> *mut c_void {
    abort_on_panic(|| {
        Box::into_raw(Box::new(CustomPropertyRegistry {
            registrations: HashMap::new(),
        }))
        .cast()
    })
}

/// Replaces the effective registered custom-property names for one document.
///
/// # Safety
/// `registry` must be a live pointer returned by `rust_custom_property_registry_create`, and
/// `registrations` must point at `registration_count` valid entries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_registry_update(
    registry: *mut c_void,
    registrations: *const FfiCustomPropertyRegistration,
    registration_count: usize,
) {
    abort_on_panic(|| {
        let registrations = if registration_count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(registrations, registration_count) }
        };
        let registry = unsafe { &mut *registry.cast::<CustomPropertyRegistry>() };
        registry.registrations.clear();
        registry.registrations.reserve(registrations.len());
        for registration in registrations {
            let name = unsafe { crate::bytes_from_raw(registration.name, registration.name_length) }
                .and_then(|name| std::str::from_utf8(name).ok())
                .expect("invalid registered custom property name");
            let initial_source = if registration.has_initial_value {
                Some(
                    unsafe { crate::bytes_from_raw(registration.initial_value, registration.initial_value_length) }
                        .expect("invalid registered custom property initial value")
                        .to_vec(),
                )
            } else {
                None
            };
            registry.registrations.insert(
                name.to_owned(),
                RegisteredCustomProperty {
                    syntax_is_universal: registration.syntax_is_universal,
                    _inherits: registration.inherits,
                    initial_source,
                },
            );
        }
    });
}

/// # Safety
/// `registry` must be a pointer returned by `rust_custom_property_registry_create` that has not
/// already been destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_registry_destroy(registry: *mut c_void) {
    abort_on_panic(|| drop(unsafe { Box::from_raw(registry.cast::<CustomPropertyRegistry>()) }));
}

/// Creates one Rust store node. Each entry name transfers one leaked fly-string reference;
/// value shells are borrowed and retained by Rust. The parent is another Rc raw pointer.
///
/// # Safety
/// `entries` must point at `entry_count` valid entries and `parent` must be null or a pointer
/// returned by this function that remains live for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_create(
    entries: *const FfiCustomPropertyStoreEntry,
    entry_count: usize,
    parent: *const c_void,
) -> *const c_void {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CustomPropertyStoreLifecycleEntry);
    abort_on_panic(|| {
        let entries = if entry_count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(entries, entry_count) }
        };
        let parent = if parent.is_null() {
            None
        } else {
            let parent = parent.cast::<CustomPropertyStore>();
            unsafe { Rc::increment_strong_count(parent) };
            Some(unsafe { Rc::from_raw(parent) })
        };
        let mut own_names = HashMap::with_capacity(entries.len());
        let own_values = entries
            .iter()
            .map(|entry| {
                let name = unsafe { crate::bytes_from_raw(entry.name_utf8, entry.name_utf8_length) }
                    .and_then(|name| std::str::from_utf8(name).ok())
                    .expect("invalid custom property name");
                own_names.insert(name.to_owned(), entry.name_raw);
                (
                    entry.name_raw,
                    CustomPropertyEntry {
                        _name: unsafe { RetainedUtf16FlyString::from_leaked_raw(entry.name_raw) },
                        _value: unsafe { RetainedStyleValue::from_borrowed_shell_pointer(entry.shell) },
                        important: entry.important,
                        data: entry.data,
                    },
                )
            })
            .collect();
        Rc::into_raw(Rc::new(CustomPropertyStore {
            own_values,
            own_names,
            parent,
        }))
        .cast()
    })
}

/// Releases one store reference returned by `rust_custom_property_store_create`.
///
/// # Safety
/// `store` must be a non-null pointer returned by `rust_custom_property_store_create` that has
/// not already been released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_destroy(store: *const c_void) {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CustomPropertyStoreLifecycleEntry);
    abort_on_panic(|| drop(unsafe { Rc::from_raw(store.cast::<CustomPropertyStore>()) }));
}

#[repr(C)]
pub struct FfiCustomPropertyStoreValue {
    pub found: bool,
    pub important: bool,
    pub shell: *const c_void,
    pub data: *const c_void,
    pub token_source: *const u8,
    pub token_source_length: usize,
}

/// Looks up a custom property through the structurally shared parent chain.
///
/// # Safety
/// `store` must be a live pointer returned by `rust_custom_property_store_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_get(
    store: *const c_void,
    name_raw: usize,
) -> FfiCustomPropertyStoreValue {
    crate::ffi_stats::bump(crate::ffi_stats::FfiOp::CustomPropertyStoreQueryEntry);
    abort_on_panic(|| {
        let store = unsafe { &*store.cast::<CustomPropertyStore>() };
        let Some(entry) = store.get(name_raw) else {
            return FfiCustomPropertyStoreValue {
                found: false,
                important: false,
                shell: std::ptr::null(),
                data: std::ptr::null(),
                token_source: std::ptr::null(),
                token_source_length: 0,
            };
        };
        let token_source = unsafe { &*entry.data.cast::<StyleValueData>() }
            .unresolved_token_source()
            .unwrap_or_default();
        FfiCustomPropertyStoreValue {
            found: true,
            important: entry.important,
            shell: entry._value.shell_pointer(),
            data: entry.data,
            token_source: token_source.as_ptr(),
            token_source_length: token_source.len(),
        }
    })
}
