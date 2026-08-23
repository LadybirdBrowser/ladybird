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
use crate::css::css_tokenizer::OwnedToken;
use crate::css::css_tokenizer::OwnedTokenKind;
use crate::css::css_tokenizer::TokenizerInput;
use crate::css::css_tokenizer::tokenize_owned;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::syntax::{SyntaxNode, parse_syntax, parse_with_syntax};
use crate::css::parser::value_parser::{FfiValueParsingContext, FfiValueParsingContextKind, ParseContext};
use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::RetainedUtf16FlyString;
use crate::css::style_value::StyleValueData;

trait Utf16SliceExt {
    fn eq_ignore_ascii_case(&self, expected: &str) -> bool;
    fn starts_with_ascii(&self, expected: &str) -> bool;
}

impl Utf16SliceExt for [u16] {
    fn eq_ignore_ascii_case(&self, expected: &str) -> bool {
        self.len() == expected.len()
            && self
                .iter()
                .zip(expected.bytes())
                .all(|(&left, right)| left <= 0x7f && (left as u8).eq_ignore_ascii_case(&right))
    }

    fn starts_with_ascii(&self, expected: &str) -> bool {
        self.len() >= expected.len()
            && self[..expected.len()]
                .iter()
                .zip(expected.bytes())
                .all(|(&left, right)| left == u16::from(right))
    }
}

#[repr(C)]
pub struct FfiCustomPropertyStoreEntry {
    pub name_raw: usize,
    pub name: FfiUtf16View,
    pub important: bool,
    pub data: *const c_void,
}

struct CustomPropertyEntry {
    _name: RetainedUtf16FlyString,
    value: RetainedStyleValueData,
    important: bool,
}

pub struct CustomPropertyStore {
    own_values: HashMap<usize, CustomPropertyEntry>,
    own_names: HashMap<Vec<u16>, usize>,
    parent: Option<Rc<CustomPropertyStore>>,
    inheritance_parent: Option<Rc<CustomPropertyStore>>,
}

pub struct CustomPropertyRegistry {
    registrations: HashMap<Vec<u16>, RegisteredCustomProperty>,
    document_url: Vec<u8>,
    document_base_url: Vec<u8>,
    intern_utf16_fly_string: Option<unsafe extern "C" fn(*const u16, usize) -> usize>,
}

struct RegisteredCustomProperty {
    syntax: SyntaxNode,
    inherits: bool,
    initial_source: Option<Vec<u16>>,
}

#[repr(C)]
pub struct FfiCustomPropertyRegistration {
    pub name: FfiUtf16View,
    pub syntax: FfiUtf16View,
    pub inherits: bool,
    pub has_initial_value: bool,
    pub initial_value: FfiUtf16View,
}

#[repr(C)]
pub struct FfiCustomPropertyRegistryContext {
    pub document_url: *const u8,
    pub document_url_length: usize,
    pub document_base_url: *const u8,
    pub document_base_url_length: usize,
    pub intern_utf16_fly_string: Option<unsafe extern "C" fn(*const u16, usize) -> usize>,
}

impl CustomPropertyRegistry {
    fn parse_context(&self, random_function_index: &mut usize) -> ParseContext {
        ParseContext {
            in_quirks_mode: false,
            is_svg_presentation_attribute: false,
            is_substituted_value: false,
            contains_attr_tainted_values: false,
            value_contexts: std::ptr::null(),
            value_context_count: 0,
            document_url: self.document_url.as_ptr(),
            document_url_length: self.document_url.len(),
            document_base_url: self.document_base_url.as_ptr(),
            document_base_url_length: self.document_base_url.len(),
            intern_utf16_fly_string: self.intern_utf16_fly_string,
            normalize_svg_path_data: None,
            font_format_is_supported: None,
            font_tech_is_supported: None,
            random_function_index,
        }
    }
}

impl CustomPropertyStore {
    fn get(&self, name_raw: usize) -> Option<&CustomPropertyEntry> {
        self.own_values
            .get(&name_raw)
            .or_else(|| self.parent.as_ref()?.get(name_raw))
    }

    fn get_by_name_with_owner(&self, name: &[u16]) -> Option<(&CustomPropertyEntry, &CustomPropertyStore)> {
        self.own_names
            .get(name)
            .and_then(|name_raw| self.own_values.get(name_raw))
            .map(|entry| (entry, self))
            .or_else(|| self.parent.as_ref()?.get_by_name_with_owner(name))
    }

    fn get_own_by_name(&self, name: &[u16]) -> Option<&CustomPropertyEntry> {
        self.own_names
            .get(name)
            .and_then(|name_raw| self.own_values.get(name_raw))
    }
}

const MAX_SUBSTITUTED_TOKEN_COUNT: usize = 16384;
const MAX_SUBSTITUTION_RECURSION_DEPTH: u32 = 64;

pub(crate) enum NativeVarResolution {
    Resolved(Vec<u16>),
    Invalid,
    NotHandled,
}

enum TokenResolution {
    Resolved(Vec<OwnedToken>),
    Invalid,
    Cyclic,
    NotHandled,
}

#[derive(Default)]
struct VarResolutionContext<'a> {
    active_names: Vec<Vec<u16>>,
    cyclic_names: HashSet<Vec<u16>>,
    inheritance_store: Option<&'a CustomPropertyStore>,
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

fn single_css_wide_keyword(tokens: &[OwnedToken]) -> Option<&[u16]> {
    let [
        OwnedToken {
            kind: OwnedTokenKind::Ident(keyword),
            ..
        },
    ] = trim_whitespace(tokens)
    else {
        return None;
    };
    is_single_css_wide_keyword(tokens).then_some(keyword.as_slice())
}

fn tokens_for_custom_property_value(data: &StyleValueData) -> Option<(Vec<OwnedToken>, bool)> {
    if let Some((source, includes_var)) = data.unresolved_var_source() {
        return Some((tokenize_owned(source), includes_var));
    }
    if matches!(data, StyleValueData::Unresolved { .. }) {
        return None;
    }
    let source = crate::css::serialize::serialize_style_value_to_utf16(data)?;
    Some((tokenize_owned(&source), false))
}

fn registration_accepts_tokens(
    registry: &CustomPropertyRegistry,
    registration: &RegisteredCustomProperty,
    tokens: &[OwnedToken],
) -> bool {
    if matches!(registration.syntax, SyntaxNode::Universal) {
        return true;
    }
    let source = serialize_tokens(tokens);
    let mut random_function_index = 0;
    let value_context = FfiValueParsingContext {
        kind: FfiValueParsingContextKind::Property,
        value: crate::css::property_metadata::property_id::CUSTOM,
        secondary_value: 0,
        name: Default::default(),
        allowed_channels: 0,
    };
    let mut context = registry.parse_context(&mut random_function_index);
    context.value_contexts = &raw const value_context;
    context.value_context_count = 1;
    parse_with_syntax(&context, &source, &registration.syntax).is_some()
}

fn registered_property_fallback(
    inheritance_store: Option<&CustomPropertyStore>,
    registry: &CustomPropertyRegistry,
    registration: &RegisteredCustomProperty,
    name: &[u16],
    recursion_depth: u32,
) -> TokenResolution {
    if registration.inherits
        && let Some(parent) = inheritance_store
    {
        return resolve_custom_property_with_lookup(
            Some(parent),
            Some(registry),
            name,
            &mut VarResolutionContext::default(),
            recursion_depth + 1,
            CustomPropertyLookup::ExplicitInheritance,
        );
    }
    registration
        .initial_source
        .as_ref()
        .map_or(TokenResolution::Invalid, |source| {
            TokenResolution::Resolved(tokenize_owned(source))
        })
}

fn resolve_css_wide_keyword(
    owner: &CustomPropertyStore,
    registry: Option<&CustomPropertyRegistry>,
    name: &[u16],
    keyword: &[u16],
    context: &mut VarResolutionContext,
    recursion_depth: u32,
    lookup: CustomPropertyLookup,
) -> TokenResolution {
    let registration = registry.and_then(|registry| registry.registrations.get(name));
    // Mirror StyleComputer::resolve_css_wide_keyword_for_custom_property(). Revert keywords
    // remain unresolved there pending custom-property revert support; typed `revert` then reaches
    // the invalid-value fallback below.
    if keyword.eq_ignore_ascii_case("initial") {
        return registration
            .and_then(|registration| registration.initial_source.as_ref())
            .map_or(TokenResolution::Invalid, |source| {
                TokenResolution::Resolved(tokenize_owned(source))
            });
    }
    if keyword.eq_ignore_ascii_case("inherit")
        || keyword.eq_ignore_ascii_case("unset") && registration.is_none_or(|registration| registration.inherits)
    {
        let inheritance_store = match lookup {
            CustomPropertyLookup::Normal => context.inheritance_store,
            CustomPropertyLookup::ExplicitInheritance => owner.inheritance_parent.as_deref(),
        };
        return resolve_custom_property_with_lookup(
            inheritance_store,
            registry,
            name,
            context,
            recursion_depth + 1,
            CustomPropertyLookup::ExplicitInheritance,
        );
    }
    if keyword.eq_ignore_ascii_case("unset") {
        return registration
            .and_then(|registration| registration.initial_source.as_ref())
            .map_or(TokenResolution::Invalid, |source| {
                TokenResolution::Resolved(tokenize_owned(source))
            });
    }
    // NB: The C++ oracle sends typed registered `revert` through the invalid-value fallback,
    //     while `revert-layer` remains unresolved pending custom-property revert support.
    if keyword.eq_ignore_ascii_case("revert")
        && let (Some(registry), Some(registration)) = (registry, registration)
        && !matches!(registration.syntax, SyntaxNode::Universal)
    {
        let inheritance_store = match lookup {
            CustomPropertyLookup::Normal => context.inheritance_store,
            CustomPropertyLookup::ExplicitInheritance => owner.inheritance_parent.as_deref(),
        };
        return registered_property_fallback(inheritance_store, registry, registration, name, recursion_depth);
    }
    TokenResolution::Resolved(tokenize_owned(crate::css::css_tokenizer::TokenizerInput::Utf16(
        keyword,
    )))
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum CustomPropertyLookup {
    Normal,
    ExplicitInheritance,
}

fn resolve_custom_property(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    name: &[u16],
    context: &mut VarResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    resolve_custom_property_with_lookup(
        store,
        registry,
        name,
        context,
        recursion_depth,
        CustomPropertyLookup::Normal,
    )
}

fn resolve_custom_property_with_lookup(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    name: &[u16],
    context: &mut VarResolutionContext,
    recursion_depth: u32,
    lookup: CustomPropertyLookup,
) -> TokenResolution {
    let registration = registry.and_then(|registry| registry.registrations.get(name));
    let entry_and_owner = store.and_then(|store| {
        if lookup == CustomPropertyLookup::ExplicitInheritance {
            return store.get_by_name_with_owner(name);
        }
        match registration {
            Some(registration) if !registration.inherits => store.get_own_by_name(name).map(|entry| (entry, store)),
            _ => store.get_by_name_with_owner(name),
        }
    });
    let Some((entry, owner)) = entry_and_owner else {
        return registration
            .and_then(|registration| registration.initial_source.as_ref())
            .map_or(TokenResolution::Invalid, |source| {
                TokenResolution::Resolved(tokenize_owned(source))
            });
    };
    let data = entry.value.data();
    if matches!(data, StyleValueData::GuaranteedInvalid) {
        return TokenResolution::Invalid;
    }
    let Some((source, includes_var)) = tokens_for_custom_property_value(data) else {
        return TokenResolution::NotHandled;
    };
    if context.cyclic_names.contains(name) {
        return TokenResolution::Invalid;
    }
    if let Some(cycle_start) = context.active_names.iter().position(|active_name| active_name == name) {
        // https://drafts.csswg.org/css-variables-1/#cycles
        // If there is a cycle in the dependency graph, all the custom properties in the cycle
        // are invalid at computed-value time.
        context
            .cyclic_names
            .extend(context.active_names[cycle_start..].iter().cloned());
        return TokenResolution::Cyclic;
    }
    context.active_names.push(name.to_owned());
    let result = if includes_var {
        substitute_tokens(store, registry, &source, context, recursion_depth + 1)
    } else {
        TokenResolution::Resolved(source)
    };
    let active_name = context.active_names.pop().expect("active custom property");
    debug_assert_eq!(active_name, name);
    if let TokenResolution::Resolved(tokens) = &result
        && let Some(keyword) = single_css_wide_keyword(tokens)
    {
        return resolve_css_wide_keyword(owner, registry, name, keyword, context, recursion_depth, lookup);
    }
    if let (Some(registry), Some(registration)) = (registry, registration) {
        let inheritance_store = match lookup {
            CustomPropertyLookup::Normal => context.inheritance_store,
            CustomPropertyLookup::ExplicitInheritance => owner.inheritance_parent.as_deref(),
        };
        if context.cyclic_names.contains(name) {
            if !context.active_names.is_empty() {
                return TokenResolution::Cyclic;
            }
            return registered_property_fallback(inheritance_store, registry, registration, name, recursion_depth);
        }
        return match result {
            TokenResolution::Resolved(tokens) if registration_accepts_tokens(registry, registration, &tokens) => {
                TokenResolution::Resolved(tokens)
            }
            TokenResolution::Resolved(_) | TokenResolution::Invalid | TokenResolution::Cyclic => {
                registered_property_fallback(inheritance_store, registry, registration, name, recursion_depth)
            }
            TokenResolution::NotHandled => TokenResolution::NotHandled,
        };
    }
    result
}

fn replace_var_function(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    arguments: &[OwnedToken],
    context: &mut VarResolutionContext,
    recursion_depth: u32,
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
    if !name.starts_with_ascii("--") {
        return TokenResolution::Invalid;
    }

    // 2. Substitute arbitrary substitution functions in first arg, then parse it as a <custom-property-name>.
    //    If parsing returned a <custom-property-name>, let result be the computed value of the corresponding custom
    //    property on el. Otherwise, let result be the guaranteed-invalid value.
    match resolve_custom_property(store, registry, name, context, recursion_depth) {
        TokenResolution::Invalid => {}
        TokenResolution::Cyclic
            if context
                .active_names
                .last()
                .is_some_and(|active_name| context.cyclic_names.contains(active_name)) =>
        {
            return TokenResolution::Cyclic;
        }
        TokenResolution::Cyclic => {}
        result => return result,
    }
    let Some(comma) = comma else {
        return TokenResolution::Invalid;
    };
    // 4. If result contains the guaranteed-invalid value, and second arg was provided, set result to the result of
    //    substitute arbitrary substitution functions on second arg.
    substitute_tokens(store, registry, &arguments[comma + 1..], context, recursion_depth + 1)
}

fn substitute_tokens(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    tokens: &[OwnedToken],
    context: &mut VarResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    if recursion_depth > MAX_SUBSTITUTION_RECURSION_DEPTH {
        return TokenResolution::Invalid;
    }

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
                replace_var_function(store, registry, contents, context, recursion_depth)
            }
            _ => substitute_tokens(store, registry, contents, context, recursion_depth + 1),
        };
        let resolved = match resolved {
            TokenResolution::Resolved(resolved) => resolved,
            TokenResolution::Invalid => return TokenResolution::Invalid,
            TokenResolution::Cyclic => return TokenResolution::Cyclic,
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

fn serialize_tokens(tokens: &[OwnedToken]) -> Vec<u16> {
    let mut output = Vec::new();
    for (index, token) in tokens.iter().enumerate() {
        token.source.append_to(&mut output);
        if let Some(next) = tokens.get(index + 1)
            && needs_comment_between(&token.kind, &next.kind)
        {
            output.extend(b"/**/".iter().copied().map(u16::from));
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
    let mut context = VarResolutionContext {
        inheritance_store: store.and_then(|store| store.inheritance_parent.as_deref()),
        ..Default::default()
    };
    match substitute_tokens(store, registry, &tokenize_owned(source), &mut context, 0) {
        TokenResolution::Resolved(tokens) => NativeVarResolution::Resolved(serialize_tokens(&tokens)),
        TokenResolution::Invalid | TokenResolution::Cyclic => NativeVarResolution::Invalid,
        TokenResolution::NotHandled => NativeVarResolution::NotHandled,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn utf16(value: &str) -> Vec<u16> {
        value.encode_utf16().collect()
    }

    fn substitute_without_custom_properties(source: &str) -> TokenResolution {
        substitute_tokens(
            None,
            None,
            &tokenize_owned(source.as_bytes()),
            &mut VarResolutionContext::default(),
            0,
        )
    }

    #[test]
    fn missing_variable_uses_fallback() {
        let TokenResolution::Resolved(tokens) = substitute_without_custom_properties("calc(var(--missing, 1px) + 2px)")
        else {
            panic!("expected fallback substitution");
        };
        assert_eq!(serialize_tokens(&tokens), utf16("calc( 1px + 2px)"));
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
        assert_eq!(serialize_tokens(&tokens), utf16(" 1/**/px"));
    }

    #[test]
    fn recognizes_substituted_css_wide_keywords() {
        assert!(is_single_css_wide_keyword(&tokenize_owned(b"  inherit ")));
        assert!(!is_single_css_wide_keyword(&tokenize_owned(b"inherit green")));
    }

    #[test]
    fn deeply_nested_functions_are_invalid() {
        let nesting = MAX_SUBSTITUTION_RECURSION_DEPTH + 1;
        let source = format!(
            "{}var(--missing, 1px){}",
            "calc(".repeat(nesting as usize),
            ")".repeat(nesting as usize)
        );
        assert!(matches!(
            substitute_without_custom_properties(&source),
            TokenResolution::Invalid
        ));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_custom_property_registry_create() -> *mut c_void {
    abort_on_panic(|| {
        Box::into_raw(Box::new(CustomPropertyRegistry {
            registrations: HashMap::new(),
            document_url: Vec::new(),
            document_base_url: Vec::new(),
            intern_utf16_fly_string: None,
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
    context: *const FfiCustomPropertyRegistryContext,
    registrations: *const FfiCustomPropertyRegistration,
    registration_count: usize,
) {
    abort_on_panic(|| {
        let Some(context) = (unsafe { context.as_ref() }) else {
            return;
        };
        let registrations = if registration_count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(registrations, registration_count) }
        };
        let registry = unsafe { &mut *registry.cast::<CustomPropertyRegistry>() };
        registry.document_url = unsafe { crate::bytes_from_raw(context.document_url, context.document_url_length) }
            .unwrap_or_default()
            .to_vec();
        registry.document_base_url =
            unsafe { crate::bytes_from_raw(context.document_base_url, context.document_base_url_length) }
                .unwrap_or_default()
                .to_vec();
        registry.intern_utf16_fly_string = context.intern_utf16_fly_string;
        registry.registrations.clear();
        registry.registrations.reserve(registrations.len());
        for registration in registrations {
            let name = unsafe { registration.name.to_utf16() }.expect("invalid registered custom property name");
            let initial_source = if registration.has_initial_value {
                Some(
                    unsafe { registration.initial_value.to_utf16() }
                        .expect("invalid registered custom property initial value"),
                )
            } else {
                None
            };
            let Some(syntax) = unsafe { registration.syntax.to_utf16() }.and_then(|syntax| parse_syntax(&syntax, true))
            else {
                continue;
            };
            registry.registrations.insert(
                name,
                RegisteredCustomProperty {
                    syntax,
                    inherits: registration.inherits,
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

/// Creates one Rust store node. Each entry transfers a leaked fly-string reference and a
/// strong style value data handle. The structural and inheritance parents are other Rc raw
/// pointers; they can differ when the C++ store has compacted its structural chain.
///
/// # Safety
/// `entries` must point at `entry_count` valid entries. Both parent pointers must be null or
/// pointers returned by this function that remain live for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_custom_property_store_create(
    entries: *const FfiCustomPropertyStoreEntry,
    entry_count: usize,
    parent: *const c_void,
    inheritance_parent: *const c_void,
) -> *const c_void {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::CustomPropertyStoreLifecycleEntry);
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
        let inheritance_parent = if inheritance_parent.is_null() {
            None
        } else {
            let inheritance_parent = inheritance_parent.cast::<CustomPropertyStore>();
            unsafe { Rc::increment_strong_count(inheritance_parent) };
            Some(unsafe { Rc::from_raw(inheritance_parent) })
        };
        let mut own_names = HashMap::with_capacity(entries.len());
        let own_values = entries
            .iter()
            .map(|entry| {
                let name = unsafe { entry.name.to_utf16() }.expect("invalid custom property name");
                own_names.insert(name, entry.name_raw);
                (
                    entry.name_raw,
                    CustomPropertyEntry {
                        _name: unsafe { RetainedUtf16FlyString::from_leaked_raw(entry.name_raw) },
                        value: unsafe { RetainedStyleValueData::from_retained_pointer(entry.data.cast()) },
                        important: entry.important,
                    },
                )
            })
            .collect();
        Rc::into_raw(Rc::new(CustomPropertyStore {
            own_values,
            own_names,
            parent,
            inheritance_parent,
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::CustomPropertyStoreLifecycleEntry);
    abort_on_panic(|| drop(unsafe { Rc::from_raw(store.cast::<CustomPropertyStore>()) }));
}

#[repr(C)]
pub struct FfiCustomPropertyStoreValue {
    pub found: bool,
    pub important: bool,
    pub data: *const c_void,
    pub token_source_ascii: *const u8,
    pub token_source_utf16: *const u16,
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
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::CustomPropertyStoreQueryEntry);
    abort_on_panic(|| {
        let store = unsafe { &*store.cast::<CustomPropertyStore>() };
        let Some(entry) = store.get(name_raw) else {
            return FfiCustomPropertyStoreValue {
                found: false,
                important: false,
                data: std::ptr::null(),
                token_source_ascii: std::ptr::null(),
                token_source_utf16: std::ptr::null(),
                token_source_length: 0,
            };
        };
        let token_source = entry.value.data().unresolved_token_source().unwrap_or_default();
        let (token_source_ascii, token_source_utf16) = match token_source {
            TokenizerInput::Ascii(units) => (units.as_ptr(), std::ptr::null()),
            TokenizerInput::Utf16(units) => (std::ptr::null(), units.as_ptr()),
        };
        FfiCustomPropertyStoreValue {
            found: true,
            important: entry.important,
            data: entry.value.data() as *const StyleValueData as *const c_void,
            token_source_ascii,
            token_source_utf16,
            token_source_length: token_source.len(),
        }
    })
}
