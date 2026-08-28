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
use std::sync::Arc;

use crate::abort_on_panic;
use crate::css::css_tokenizer::OwnedToken;
use crate::css::css_tokenizer::OwnedTokenKind;
use crate::css::css_tokenizer::TokenizerInput;
use crate::css::css_tokenizer::tokenize_owned;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::syntax::{SyntaxNode, clone_syntax_handle, parse_syntax, parse_with_syntax};
use crate::css::parser::value_parser::{FfiValueParsingContext, FfiValueParsingContextKind, ParseContext};
use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::RetainedUtf16FlyString;
use crate::css::style_value::StyleValueData;

include!(concat!(env!("OUT_DIR"), "/environment_variables_generated.rs"));

trait Utf16SliceExt {
    fn eq_ignore_ascii_case(&self, expected: &str) -> bool;
    fn eq_ignore_ascii_case_utf16(&self, expected: &[u16]) -> bool;
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

    fn eq_ignore_ascii_case_utf16(&self, expected: &[u16]) -> bool {
        self.len() == expected.len()
            && self.iter().zip(expected).all(|(&left, &right)| {
                left == right
                    || u8::try_from(left)
                        .ok()
                        .zip(u8::try_from(right).ok())
                        .is_some_and(|(left, right)| left.eq_ignore_ascii_case(&right))
            })
    }
}

fn scoped_name(scope: usize, name: &[u16]) -> Vec<u16> {
    let mut digits = Vec::new();
    let mut value = scope;
    loop {
        digits.push(u16::from(b'0') + (value % 10) as u16);
        value /= 10;
        if value == 0 {
            break;
        }
    }
    digits.reverse();
    digits.push(u16::from(b':'));
    digits.extend_from_slice(name);
    digits
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
    parent: Option<Arc<CustomPropertyStore>>,
    inheritance_parent: Option<Arc<CustomPropertyStore>>,
}

// SAFETY: Store nodes and their entries are immutable after construction. Style workers only
// borrow the graph while resolving substitution text; the C++-owned raw Arc reference remains
// alive until every worker in the blocking batch has joined, so destruction stays on the main
// thread. The retained style values and names are likewise only read by workers.
unsafe impl Send for CustomPropertyStore {}
unsafe impl Sync for CustomPropertyStore {}

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

#[derive(Clone)]
struct CustomFunctionParameter {
    name: Vec<u16>,
    syntax: SyntaxNode,
    default_tokens: Option<Vec<OwnedToken>>,
}

#[derive(Clone)]
struct CustomFunctionDefinition {
    identity: usize,
    scope_identity: usize,
    name: Vec<u16>,
    parameters: Vec<CustomFunctionParameter>,
    return_syntax: SyntaxNode,
    declarations: Vec<(Vec<u16>, Vec<OwnedToken>, bool)>,
}

struct CustomFunctionRegistry {
    caller_scope_identity: usize,
    definitions: Vec<CustomFunctionDefinition>,
}

#[derive(Clone)]
struct FunctionLocalRegistration {
    syntax: SyntaxNode,
    initial_tokens: Option<Vec<OwnedToken>>,
    is_result: bool,
}

#[derive(Clone)]
struct FunctionLocalValue {
    tokens: Vec<OwnedToken>,
    includes_substitution: bool,
}

struct FunctionLocalScope {
    values: HashMap<Vec<u16>, FunctionLocalValue>,
    registrations: HashMap<Vec<u16>, FunctionLocalRegistration>,
}

#[repr(C)]
pub struct FfiCustomPropertyRegistration {
    pub name: FfiUtf16View,
    pub syntax: *const c_void,
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

#[repr(C)]
pub struct FfiSubstitutionAttribute {
    pub name: FfiUtf16View,
    pub value: FfiUtf16View,
}

#[repr(C)]
pub struct FfiSubstitutionFunctionParameter {
    pub name: FfiUtf16View,
    pub syntax: *const c_void,
    pub default_data: *const c_void,
}

#[repr(C)]
pub struct FfiSubstitutionFunctionDeclaration {
    pub name: FfiUtf16View,
    pub data: *const c_void,
}

#[repr(C)]
pub struct FfiSubstitutionFunctionDefinition {
    pub identity: usize,
    pub scope_identity: usize,
    pub name: FfiUtf16View,
    pub parameters: *const FfiSubstitutionFunctionParameter,
    pub parameter_count: usize,
    pub return_syntax: *const c_void,
    pub declarations: *const FfiSubstitutionFunctionDeclaration,
    pub declaration_count: usize,
}

impl CustomPropertyRegistry {
    fn parse_context(&self, random_function_index: &mut usize) -> ParseContext {
        ParseContext {
            in_quirks_mode: false,
            is_svg_presentation_attribute: false,
            is_substituted_value: false,
            contains_attr_tainted_values: false,
            is_ua_style_sheet: false,
            value_contexts: std::ptr::null(),
            value_context_count: 0,
            document_url: self.document_url.as_ptr(),
            document_url_length: self.document_url.len(),
            document_base_url: self.document_base_url.as_ptr(),
            document_base_url_length: self.document_base_url.len(),
            intern_utf16_fly_string: self.intern_utf16_fly_string,
            normalize_svg_path_data: None,
            precomputed_svg_paths: std::ptr::null(),
            precomputed_svg_path_count: 0,
            font_format_is_supported: None,
            font_tech_is_supported: None,
            descriptor_integer_resolution_context: std::ptr::null(),
            resolve_descriptor_integer: None,
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
    Resolved {
        source: Vec<u16>,
        contains_attr_tainted_values: bool,
    },
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
    active_attributes: Vec<Vec<u16>>,
    cyclic_attributes: HashSet<Vec<u16>>,
    attributes: Option<&'a HashMap<Vec<u16>, Vec<u16>>>,
    inheritance_store: Option<&'a CustomPropertyStore>,
    inheritance_store_overrides: Vec<Option<Arc<CustomPropertyStore>>>,
    attribute_names_are_ascii_case_insensitive: bool,
    contains_attr_tainted_values: bool,
    custom_functions: Option<&'a CustomFunctionRegistry>,
    resolve_custom_function: Option<unsafe extern "C" fn(usize, FfiUtf16View) -> usize>,
    condition_context: *mut c_void,
    evaluate_condition: Option<unsafe extern "C" fn(*mut c_void, u8, FfiUtf16View) -> u8>,
    final_custom_properties: Option<&'a HashMap<Vec<u16>, *const c_void>>,
    active_functions: Vec<usize>,
    cyclic_functions: HashSet<usize>,
    function_local_scopes: Vec<FunctionLocalScope>,
    token_cache: Option<&'a mut CustomPropertyTokenCache>,
    resolution_stats: Option<&'a VarResolutionStats>,
}

type CustomPropertyTokenCache = HashMap<*const StyleValueData, (Arc<[OwnedToken]>, bool, bool)>;

pub(crate) struct VarResolutionEnvironment {
    attributes: HashMap<Vec<u16>, Vec<u16>>,
    custom_functions: CustomFunctionRegistry,
    token_cache: CustomPropertyTokenCache,
    resolution_stats: VarResolutionStats,
}

#[derive(Default)]
struct VarResolutionStats {
    final_value_hits: std::cell::Cell<u64>,
    final_value_misses: std::cell::Cell<u64>,
}

impl VarResolutionEnvironment {
    pub(crate) fn final_value_hits(&self) -> u64 {
        self.resolution_stats.final_value_hits.get()
    }

    pub(crate) fn final_value_misses(&self) -> u64 {
        self.resolution_stats.final_value_misses.get()
    }
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

fn find_top_level_source(tokens: &[OwnedToken], source: &[u8]) -> Option<usize> {
    let mut index = 0;
    while index < tokens.len() {
        if tokens[index].source.equals_ascii(source) {
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

fn source_ends_with_comment(source: crate::css::css_tokenizer::TokenizerInput<'_>) -> bool {
    match source {
        crate::css::css_tokenizer::TokenizerInput::Ascii(source) => source
            .iter()
            .rposition(|unit| !unit.is_ascii_whitespace())
            .is_some_and(|end| end > 0 && source[end - 1..=end] == *b"*/"),
        crate::css::css_tokenizer::TokenizerInput::Utf16(source) => source
            .iter()
            .rposition(|unit| !matches!(*unit, 0x09 | 0x0a | 0x0c | 0x0d | 0x20))
            .is_some_and(|end| end > 0 && source[end - 1..=end] == [u16::from(b'*'), u16::from(b'/')]),
    }
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

fn tokens_for_custom_property_value(data: &StyleValueData) -> Option<(Vec<OwnedToken>, bool, bool)> {
    if let StyleValueData::Unresolved {
        presence_attr,
        presence_dashed_function,
        presence_env,
        presence_if,
        presence_inherit,
        presence_var,
        contains_attr_tainted_values,
        ..
    } = data
    {
        let mut tokens = tokenize_owned(data.unresolved_token_source().unwrap_or_default());
        let authored_source = data.unresolved_authored_source().unwrap_or_default();
        if source_ends_with_comment(authored_source) {
            while matches!(tokens.last().map(|token| &token.kind), Some(OwnedTokenKind::Whitespace)) {
                tokens.pop();
            }
        }
        return Some((
            tokens,
            *presence_var
                || *presence_attr
                || *presence_dashed_function
                || *presence_env
                || *presence_if
                || *presence_inherit,
            *contains_attr_tainted_values,
        ));
    }
    let source = crate::css::serialize::serialize_style_value_to_utf16(data)?;
    Some((tokenize_owned(&source), false, false))
}

fn cached_tokens_for_custom_property_value(
    data: &StyleValueData,
    context: &mut VarResolutionContext<'_>,
) -> Option<(Arc<[OwnedToken]>, bool, bool)> {
    let key = std::ptr::from_ref(data);
    if let Some(cached) = context.token_cache.as_deref().and_then(|cache| cache.get(&key)) {
        return Some(cached.clone());
    }
    let (tokens, includes_substitution, contains_attr_tainted_values) = tokens_for_custom_property_value(data)?;
    let result = (Arc::from(tokens), includes_substitution, contains_attr_tainted_values);
    if let Some(cache) = context.token_cache.as_deref_mut() {
        cache.insert(key, result.clone());
    }
    Some(result)
}

fn tokens_for_function_value(data: &StyleValueData) -> Option<(Vec<OwnedToken>, bool)> {
    if let StyleValueData::Unresolved {
        presence_attr,
        presence_dashed_function,
        presence_env,
        presence_if,
        presence_inherit,
        presence_var,
        ..
    } = data
    {
        return Some((
            tokenize_owned(data.unresolved_token_source().unwrap_or_default()),
            *presence_var
                || *presence_attr
                || *presence_dashed_function
                || *presence_env
                || *presence_if
                || *presence_inherit,
        ));
    }
    let source = crate::css::serialize::serialize_style_value_to_utf16(data)?;
    Some((tokenize_owned(&source), false))
}

unsafe fn custom_function_registry_from_ffi(
    definitions: *const FfiSubstitutionFunctionDefinition,
    definition_count: usize,
    caller_scope_identity: usize,
) -> Option<CustomFunctionRegistry> {
    let definitions = if definition_count == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(definitions, definition_count) }
    };
    let mut parsed_definitions = Vec::with_capacity(definitions.len());
    for definition in definitions {
        let name = unsafe { definition.name.to_utf16() }?;
        let return_syntax = unsafe { clone_syntax_handle(definition.return_syntax) }?;
        let parameters = if definition.parameter_count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(definition.parameters, definition.parameter_count) }
        };
        let mut parsed_parameters = Vec::with_capacity(parameters.len());
        for parameter in parameters {
            let name = unsafe { parameter.name.to_utf16() }?;
            let syntax = unsafe { clone_syntax_handle(parameter.syntax) }?;
            let default_tokens = if parameter.default_data.is_null() {
                None
            } else {
                let data = unsafe { &*parameter.default_data.cast::<StyleValueData>() };
                Some(tokens_for_function_value(data)?.0)
            };
            parsed_parameters.push(CustomFunctionParameter {
                name,
                syntax,
                default_tokens,
            });
        }
        let declarations = if definition.declaration_count == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(definition.declarations, definition.declaration_count) }
        };
        let mut parsed_declarations = Vec::with_capacity(declarations.len());
        for declaration in declarations {
            let name = unsafe { declaration.name.to_utf16() }?;
            let data = unsafe { &*declaration.data.cast::<StyleValueData>() };
            let (tokens, includes_substitution) = tokens_for_function_value(data)?;
            parsed_declarations.push((name, tokens, includes_substitution));
        }
        parsed_definitions.push(CustomFunctionDefinition {
            identity: definition.identity,
            scope_identity: definition.scope_identity,
            name,
            parameters: parsed_parameters,
            return_syntax,
            declarations: parsed_declarations,
        });
    }
    Some(CustomFunctionRegistry {
        caller_scope_identity,
        definitions: parsed_definitions,
    })
}

/// Builds the element-wide immutable substitution inputs once for all declarations in a cascade.
///
/// # Safety
/// Every FFI pointer must remain readable for this call.
pub(crate) unsafe fn prepare_var_resolution_environment(
    attributes: *const FfiSubstitutionAttribute,
    attribute_count: usize,
    custom_functions: *const FfiSubstitutionFunctionDefinition,
    custom_function_count: usize,
    custom_function_scope_identity: usize,
) -> Option<VarResolutionEnvironment> {
    let attributes = if attribute_count == 0 {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(attributes, attribute_count) }
    };
    let attributes = attributes
        .iter()
        .filter_map(|attribute| {
            Some((unsafe { attribute.name.to_utf16() }?, unsafe {
                attribute.value.to_utf16()
            }?))
        })
        .collect();
    let custom_functions = unsafe {
        custom_function_registry_from_ffi(custom_functions, custom_function_count, custom_function_scope_identity)
    }?;
    Some(VarResolutionEnvironment {
        attributes,
        custom_functions,
        token_cache: HashMap::new(),
        resolution_stats: VarResolutionStats::default(),
    })
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
    // NB: Typed registered `revert` uses the invalid-value fallback, while `revert-layer` remains
    //     unresolved pending custom-property revert support.
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

fn normalize_function_tokens(
    registry: Option<&CustomPropertyRegistry>,
    syntax: &SyntaxNode,
    tokens: &[OwnedToken],
) -> TokenResolution {
    if matches!(syntax, SyntaxNode::Universal) {
        return TokenResolution::Resolved(tokens.to_vec());
    }
    let Some(registry) = registry else {
        return TokenResolution::NotHandled;
    };
    let source = serialize_tokens(tokens);
    let mut random_function_index = 0;
    let context = registry.parse_context(&mut random_function_index);
    let Some(parsed) = parse_with_syntax(&context, &source, syntax) else {
        return TokenResolution::Invalid;
    };
    let computed = if matches!(parsed, StyleValueData::Calculated { .. }) {
        crate::css::calc::collapse_calculated_without_context(&parsed)
    } else {
        None
    };
    let Some(source) =
        crate::css::serialize::serialize_resolved_style_value_to_utf16(computed.as_ref().unwrap_or(&parsed))
    else {
        return TokenResolution::Invalid;
    };
    TokenResolution::Resolved(tokenize_owned(&source))
}

fn resolve_function_local_property(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    name: &[u16],
    value: Option<FunctionLocalValue>,
    registration: FunctionLocalRegistration,
    context: &mut VarResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    let Some(value) = value else {
        return registration
            .initial_tokens
            .map_or(TokenResolution::Invalid, TokenResolution::Resolved);
    };
    let active_name = scoped_name(context.function_local_scopes.len(), name);
    if context.cyclic_names.contains(&active_name) {
        return TokenResolution::Invalid;
    }
    if let Some(cycle_start) = context.active_names.iter().position(|name| name == &active_name) {
        context
            .cyclic_names
            .extend(context.active_names[cycle_start..].iter().cloned());
        return TokenResolution::Cyclic;
    }
    context.active_names.push(active_name.clone());
    let mut result = if value.includes_substitution {
        substitute_tokens(store, registry, &value.tokens, context, recursion_depth + 1)
    } else {
        TokenResolution::Resolved(value.tokens)
    };
    let popped = context.active_names.pop().expect("active function-local property");
    debug_assert_eq!(popped, active_name);
    if context.cyclic_names.contains(&active_name) {
        return TokenResolution::Cyclic;
    }
    if let TokenResolution::Resolved(tokens) = &result
        && let Some(keyword) = single_css_wide_keyword(tokens)
    {
        if registration.is_result && matches!(registration.syntax, SyntaxNode::Universal) {
            return result;
        }
        if keyword.eq_ignore_ascii_case("initial") {
            return registration
                .initial_tokens
                .map_or(TokenResolution::Invalid, TokenResolution::Resolved);
        }
        if keyword.eq_ignore_ascii_case("inherit") {
            let local_scope = context
                .function_local_scopes
                .pop()
                .expect("function-local property scope");
            result = resolve_custom_property(store, registry, name, context, recursion_depth + 1);
            context.function_local_scopes.push(local_scope);
        } else {
            return TokenResolution::Invalid;
        }
    }
    match result {
        TokenResolution::Resolved(tokens) => normalize_function_tokens(registry, &registration.syntax, &tokens),
        other => other,
    }
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
    let local_scope_index = context
        .function_local_scopes
        .iter()
        .rposition(|local_scope| local_scope.values.contains_key(name) || local_scope.registrations.contains_key(name));
    if let Some(local_scope_index) = local_scope_index {
        let value = context.function_local_scopes[local_scope_index]
            .values
            .get(name)
            .cloned();
        let registration = context.function_local_scopes[local_scope_index]
            .registrations
            .get(name)
            .cloned();
        let child_scopes = context.function_local_scopes.split_off(local_scope_index + 1);
        let result = resolve_function_local_property(
            store,
            registry,
            name,
            value,
            registration.unwrap_or(FunctionLocalRegistration {
                syntax: SyntaxNode::Universal,
                initial_tokens: None,
                is_result: false,
            }),
            context,
            recursion_depth,
        );
        context.function_local_scopes.extend(child_scopes);
        return result;
    }
    if lookup == CustomPropertyLookup::Normal {
        if context.cyclic_names.contains(name) {
            return TokenResolution::Cyclic;
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
    }
    if lookup == CustomPropertyLookup::Normal
        && let Some(final_values) = context.final_custom_properties
    {
        if let Some(data) = final_values
            .get(name)
            .and_then(|data| unsafe { data.cast::<StyleValueData>().as_ref() })
        {
            if let Some(stats) = context.resolution_stats {
                stats.final_value_hits.set(stats.final_value_hits.get() + 1);
            }
            if matches!(data, StyleValueData::GuaranteedInvalid) {
                return TokenResolution::Invalid;
            }
            let Some((tokens, includes_substitution, contains_attr_tainted_values)) =
                cached_tokens_for_custom_property_value(data, context)
            else {
                return TokenResolution::NotHandled;
            };
            context.contains_attr_tainted_values |= contains_attr_tainted_values;
            if !includes_substitution {
                return TokenResolution::Resolved(tokens.to_vec());
            }
        } else if let Some(stats) = context.resolution_stats {
            stats.final_value_misses.set(stats.final_value_misses.get() + 1);
        }
    }
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
    let Some((source, includes_var, contains_attr_tainted_values)) =
        cached_tokens_for_custom_property_value(data, context)
    else {
        return TokenResolution::NotHandled;
    };
    context.contains_attr_tainted_values |= contains_attr_tainted_values;
    context.active_names.push(name.to_owned());
    if lookup == CustomPropertyLookup::ExplicitInheritance {
        context
            .inheritance_store_overrides
            .push(owner.inheritance_parent.clone());
    }
    let result = if includes_var {
        substitute_tokens(store, registry, &source, context, recursion_depth + 1)
    } else {
        TokenResolution::Resolved(source.to_vec())
    };
    if lookup == CustomPropertyLookup::ExplicitInheritance {
        context.inheritance_store_overrides.pop();
    }
    let active_name = context.active_names.pop().expect("active custom property");
    debug_assert_eq!(active_name, name);
    if registration.is_none() && context.cyclic_names.contains(name) {
        return TokenResolution::Cyclic;
    }
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
    let first_argument = &arguments[..comma.unwrap_or(arguments.len())];

    // 2. Substitute arbitrary substitution functions in first arg, then parse it as a <custom-property-name>.
    //    If parsing returned a <custom-property-name>, let result be the computed value of the corresponding custom
    //    property on el. Otherwise, let result be the guaranteed-invalid value.
    let substituted_first = match substitute_tokens(store, registry, first_argument, context, recursion_depth + 1) {
        TokenResolution::Resolved(tokens) => tokens,
        TokenResolution::Invalid | TokenResolution::Cyclic => Vec::new(),
        TokenResolution::NotHandled => return TokenResolution::NotHandled,
    };
    let name = match trim_whitespace(&substituted_first) {
        [
            OwnedToken {
                kind: OwnedTokenKind::Ident(name),
                ..
            },
        ] if name.starts_with_ascii("--") => Some(name.as_slice()),
        _ => None,
    };
    if let Some(name) = name {
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
    }
    let Some(comma) = comma else {
        return TokenResolution::Invalid;
    };
    // 4. If result contains the guaranteed-invalid value, and second arg was provided, set result to the result of
    //    substitute arbitrary substitution functions on second arg.
    substitute_tokens(store, registry, &arguments[comma + 1..], context, recursion_depth + 1)
}

fn replace_inherit_function(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    arguments: &[OwnedToken],
    context: &mut VarResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    // https://drafts.csswg.org/css-values-5/#replace-an-inherit-function
    let comma = find_top_level_comma(arguments);
    let first_argument = &arguments[..comma.unwrap_or(arguments.len())];
    let substituted_first = match substitute_tokens(store, registry, first_argument, context, recursion_depth + 1) {
        TokenResolution::Resolved(tokens) => tokens,
        TokenResolution::Invalid | TokenResolution::Cyclic => Vec::new(),
        TokenResolution::NotHandled => return TokenResolution::NotHandled,
    };
    let name = match trim_whitespace(&substituted_first) {
        [
            OwnedToken {
                kind: OwnedTokenKind::Ident(name),
                ..
            },
        ] if name.starts_with_ascii("--") => Some(name.as_slice()),
        _ => None,
    };
    if let Some(name) = name {
        let local_scope = context.function_local_scopes.pop();
        let inherited_same_name = context
            .active_names
            .iter()
            .rposition(|active_name| active_name == name)
            .map(|index| (index, context.active_names.remove(index)));
        let inherited_store_override = context.inheritance_store_overrides.last().cloned().flatten();
        let inherited_store = if local_scope.is_some() {
            store
        } else {
            inherited_store_override.as_deref().or(context.inheritance_store)
        };
        let result = resolve_custom_property_with_lookup(
            inherited_store,
            registry,
            name,
            context,
            recursion_depth + 1,
            if local_scope.is_some() {
                CustomPropertyLookup::Normal
            } else {
                CustomPropertyLookup::ExplicitInheritance
            },
        );
        if let Some(local_scope) = local_scope {
            context.function_local_scopes.push(local_scope);
        }
        if let Some((index, inherited_same_name)) = inherited_same_name {
            context.active_names.insert(index, inherited_same_name);
        }
        match result {
            TokenResolution::Invalid | TokenResolution::Cyclic => {}
            result => return result,
        }
    }
    let Some(comma) = comma else {
        return TokenResolution::Invalid;
    };
    substitute_tokens(store, registry, &arguments[comma + 1..], context, recursion_depth + 1)
}

fn replace_env_function(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    arguments: &[OwnedToken],
    context: &mut VarResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    // https://drafts.csswg.org/css-env/#substitute-an-env
    let comma = find_top_level_comma(arguments);
    let first_argument = &arguments[..comma.unwrap_or(arguments.len())];
    let substituted_first = match substitute_tokens(store, registry, first_argument, context, recursion_depth + 1) {
        TokenResolution::Resolved(tokens) => tokens,
        TokenResolution::Invalid => return TokenResolution::Invalid,
        TokenResolution::Cyclic => return TokenResolution::Cyclic,
        TokenResolution::NotHandled => return TokenResolution::NotHandled,
    };
    let first_argument = trim_whitespace(&substituted_first);
    let Some(name) = (match first_argument.first() {
        Some(OwnedToken {
            kind: OwnedTokenKind::Ident(name),
            ..
        }) => Some(name.as_slice()),
        _ => None,
    }) else {
        return TokenResolution::Invalid;
    };
    let Some(indices) = (|| {
        let mut indices = Vec::new();
        for token in trim_whitespace(&first_argument[1..]) {
            if matches!(token.kind, OwnedTokenKind::Whitespace) {
                continue;
            }
            if !matches!(token.kind, OwnedTokenKind::Number) {
                return None;
            }
            let source = token.source.to_vec();
            let index = source.iter().try_fold(0i32, |value, &unit| {
                let digit = unit.checked_sub(u16::from(b'0'))?;
                (digit <= 9).then_some(value.checked_mul(10)?.checked_add(i32::from(digit))?)
            })?;
            if index < 0 {
                return None;
            }
            indices.push(index);
        }
        Some(indices)
    })() else {
        return TokenResolution::Invalid;
    };

    let variable = ENVIRONMENT_VARIABLES
        .iter()
        .find(|(known_name, _, _)| name.eq_ignore_ascii_case(known_name));
    if let Some((_, dimension_count, value_type)) = variable
        && *dimension_count == indices.len()
    {
        if *value_type == "<number>" {
            return TokenResolution::Resolved(tokenize_owned(b"1"));
        }
        if *dimension_count == 0 {
            return TokenResolution::Resolved(tokenize_owned(b"0px"));
        }
        // The C++ document oracle currently exposes no viewport segments, so every
        // recognized two-dimensional viewport-segment lookup is guaranteed-invalid.
        return TokenResolution::Invalid;
    }
    let Some(comma) = comma else {
        return TokenResolution::Invalid;
    };
    substitute_tokens(store, registry, &arguments[comma + 1..], context, recursion_depth + 1)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ConditionEvaluation {
    Match(bool),
    Invalid,
    Cyclic,
    NotHandled,
}

#[derive(Debug)]
enum ParsedBooleanExpression {
    Test(Vec<OwnedToken>),
    Not(Box<ParsedBooleanExpression>),
    And(Vec<ParsedBooleanExpression>),
    Or(Vec<ParsedBooleanExpression>),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ConditionValidation {
    Valid,
    Invalid,
}

fn parse_boolean_expression(
    tokens: &[OwnedToken],
    validate_test: &mut impl FnMut(&[OwnedToken]) -> ConditionValidation,
) -> Result<ParsedBooleanExpression, ConditionValidation> {
    let tokens = trim_whitespace(tokens);
    if tokens.is_empty() {
        return Err(ConditionValidation::Invalid);
    }
    match validate_test(tokens) {
        ConditionValidation::Valid => return Ok(ParsedBooleanExpression::Test(tokens.to_vec())),
        ConditionValidation::Invalid => {}
    }
    if matches!(
        tokens.first(),
        Some(OwnedToken {
            kind: OwnedTokenKind::Ident(keyword),
            ..
        }) if keyword.eq_ignore_ascii_case("not")
    ) {
        return Ok(ParsedBooleanExpression::Not(Box::new(parse_boolean_group(
            &tokens[1..],
            validate_test,
        )?)));
    }

    let mut combinator = None;
    let mut groups = Vec::new();
    let mut group_start = 0;
    let mut index = 0;
    while index < tokens.len() {
        if matching_close(&tokens[index].kind).is_some() {
            let Some(close) = find_matching_close(tokens, index) else {
                return Err(ConditionValidation::Invalid);
            };
            index = close + 1;
            continue;
        }
        let current = match &tokens[index].kind {
            OwnedTokenKind::Ident(keyword) if keyword.eq_ignore_ascii_case("and") => Some(true),
            OwnedTokenKind::Ident(keyword) if keyword.eq_ignore_ascii_case("or") => Some(false),
            _ => None,
        };
        if let Some(current) = current {
            if combinator.is_some_and(|combinator| combinator != current) {
                return Err(ConditionValidation::Invalid);
            }
            combinator = Some(current);
            groups.push(parse_boolean_group(&tokens[group_start..index], validate_test)?);
            group_start = index + 1;
        }
        index += 1;
    }
    let Some(combinator) = combinator else {
        return parse_boolean_group(tokens, validate_test);
    };
    groups.push(parse_boolean_group(&tokens[group_start..], validate_test)?);
    Ok(if combinator {
        ParsedBooleanExpression::And(groups)
    } else {
        ParsedBooleanExpression::Or(groups)
    })
}

fn parse_boolean_group(
    tokens: &[OwnedToken],
    validate_test: &mut impl FnMut(&[OwnedToken]) -> ConditionValidation,
) -> Result<ParsedBooleanExpression, ConditionValidation> {
    let tokens = trim_whitespace(tokens);
    if matches!(tokens.first().map(|token| &token.kind), Some(OwnedTokenKind::OpenParen))
        && find_matching_close(tokens, 0) == Some(tokens.len() - 1)
    {
        return parse_boolean_expression(&tokens[1..tokens.len() - 1], validate_test);
    }
    match validate_test(tokens) {
        ConditionValidation::Valid => Ok(ParsedBooleanExpression::Test(tokens.to_vec())),
        validation => Err(validation),
    }
}

fn evaluate_parsed_boolean_expression(
    expression: &ParsedBooleanExpression,
    evaluate_test: &mut impl FnMut(&[OwnedToken]) -> ConditionEvaluation,
) -> ConditionEvaluation {
    match expression {
        ParsedBooleanExpression::Test(tokens) => evaluate_test(tokens),
        ParsedBooleanExpression::Not(child) => match evaluate_parsed_boolean_expression(child, evaluate_test) {
            ConditionEvaluation::Match(value) => ConditionEvaluation::Match(!value),
            other => other,
        },
        ParsedBooleanExpression::And(children) => {
            for child in children {
                match evaluate_parsed_boolean_expression(child, evaluate_test) {
                    ConditionEvaluation::Match(true) => {}
                    ConditionEvaluation::Match(false) => return ConditionEvaluation::Match(false),
                    other => return other,
                }
            }
            ConditionEvaluation::Match(true)
        }
        ParsedBooleanExpression::Or(children) => {
            for child in children {
                match evaluate_parsed_boolean_expression(child, evaluate_test) {
                    ConditionEvaluation::Match(false) => {}
                    ConditionEvaluation::Match(true) => return ConditionEvaluation::Match(true),
                    other => return other,
                }
            }
            ConditionEvaluation::Match(false)
        }
    }
}

fn registered_style_query_values_are_equal(
    registry: &CustomPropertyRegistry,
    syntax: &SyntaxNode,
    computed_tokens: &[OwnedToken],
    query_tokens: &[OwnedToken],
) -> bool {
    let mut random_function_index = 0;
    let context = registry.parse_context(&mut random_function_index);
    let Some(computed) = parse_with_syntax(&context, &serialize_tokens(computed_tokens), syntax) else {
        return false;
    };
    let Some(query) = parse_with_syntax(&context, &serialize_tokens(query_tokens), syntax) else {
        return false;
    };
    let computed_color = crate::css::color_resolution::to_color(&computed, &crate::css::color_resolution::EMPTY_INPUT);
    let query_color = crate::css::color_resolution::to_color(&query, &crate::css::color_resolution::EMPTY_INPUT);
    if computed_color.is_some() || query_color.is_some() {
        return computed_color.is_some() && computed_color == query_color;
    }
    computed == query
}

fn validate_style_feature(tokens: &[OwnedToken]) -> ConditionValidation {
    let tokens = trim_whitespace(tokens);
    if tokens.iter().any(|token| {
        token.source.equals_ascii(b"<") || token.source.equals_ascii(b">") || token.source.equals_ascii(b"=")
    }) {
        return ConditionValidation::Valid;
    }
    let colon = find_top_level_source(tokens, b":");
    let name_tokens = trim_whitespace(&tokens[..colon.unwrap_or(tokens.len())]);
    if !matches!(
        name_tokens,
        [OwnedToken {
            kind: OwnedTokenKind::Ident(_),
            ..
        }]
    ) {
        return ConditionValidation::Invalid;
    }
    if let Some(colon) = colon {
        let query = trim_whitespace(&tokens[colon + 1..]);
        if query.iter().any(|token| {
            token.source.equals_ascii(b"<") || token.source.equals_ascii(b">") || token.source.equals_ascii(b"=")
        }) {
            return ConditionValidation::Invalid;
        }
    }
    ConditionValidation::Valid
}

fn evaluate_style_feature(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    tokens: &[OwnedToken],
    context: &mut VarResolutionContext,
    recursion_depth: u32,
) -> ConditionEvaluation {
    let tokens = trim_whitespace(tokens);
    if tokens.iter().any(|token| {
        token.source.equals_ascii(b"<") || token.source.equals_ascii(b">") || token.source.equals_ascii(b"=")
    }) {
        let Some(evaluate) = context.evaluate_condition else {
            return ConditionEvaluation::NotHandled;
        };
        let source = serialize_tokens(tokens);
        return match unsafe {
            evaluate(
                context.condition_context,
                2,
                FfiUtf16View {
                    ascii: std::ptr::null(),
                    utf16: source.as_ptr(),
                    length: source.len(),
                },
            )
        } {
            0 => ConditionEvaluation::Match(false),
            1 => ConditionEvaluation::Match(true),
            2 => ConditionEvaluation::Invalid,
            3 => ConditionEvaluation::Cyclic,
            _ => ConditionEvaluation::NotHandled,
        };
    }
    let colon = find_top_level_source(tokens, b":");
    let name_tokens = trim_whitespace(&tokens[..colon.unwrap_or(tokens.len())]);
    let [
        OwnedToken {
            kind: OwnedTokenKind::Ident(name),
            ..
        },
    ] = name_tokens
    else {
        return ConditionEvaluation::Invalid;
    };
    if !name.starts_with_ascii("--") {
        return ConditionEvaluation::Match(false);
    }

    let local_registration = context
        .function_local_scopes
        .last()
        .and_then(|scope| scope.registrations.get(name))
        .cloned();
    let registration = context
        .function_local_scopes
        .is_empty()
        .then(|| registry.and_then(|registry| registry.registrations.get(name)))
        .flatten();
    let computed = resolve_custom_property(store, registry, name, context, recursion_depth + 1);
    let computed = match computed {
        TokenResolution::Resolved(tokens) => Some(tokens),
        TokenResolution::Invalid => None,
        TokenResolution::Cyclic
            if context
                .active_names
                .first()
                .is_some_and(|root_name| context.cyclic_names.contains(root_name)) =>
        {
            return ConditionEvaluation::Cyclic;
        }
        TokenResolution::Cyclic => None,
        TokenResolution::NotHandled => return ConditionEvaluation::NotHandled,
    };
    if registration.is_some()
        && !context.active_names.iter().any(|active_name| active_name == name)
        && let Some(evaluate) = context.evaluate_condition
    {
        let source = serialize_tokens(tokens);
        return match unsafe {
            evaluate(
                context.condition_context,
                2,
                FfiUtf16View {
                    ascii: std::ptr::null(),
                    utf16: source.as_ptr(),
                    length: source.len(),
                },
            )
        } {
            0 => ConditionEvaluation::Match(false),
            1 => ConditionEvaluation::Match(true),
            2 => ConditionEvaluation::Invalid,
            3 => ConditionEvaluation::Cyclic,
            _ => ConditionEvaluation::NotHandled,
        };
    }
    let Some(colon) = colon else {
        return ConditionEvaluation::Match(computed.is_some());
    };
    let query = trim_whitespace(&tokens[colon + 1..]);

    if let Some(keyword) = single_css_wide_keyword(query) {
        if keyword.eq_ignore_ascii_case("revert") || keyword.eq_ignore_ascii_case("revert-layer") {
            return ConditionEvaluation::Match(false);
        }
        let expected = if keyword.eq_ignore_ascii_case("initial")
            || keyword.eq_ignore_ascii_case("unset") && registration.is_some_and(|registration| !registration.inherits)
        {
            local_registration
                .as_ref()
                .and_then(|registration| registration.initial_tokens.clone())
                .or_else(|| {
                    registration.and_then(|registration| registration.initial_source.as_ref().map(tokenize_owned))
                })
        } else {
            let local_scope = local_registration
                .as_ref()
                .and_then(|_| context.function_local_scopes.pop());
            let inherited_store = local_scope.as_ref().map_or(context.inheritance_store, |_| store);
            let result = resolve_custom_property(inherited_store, registry, name, context, recursion_depth + 1);
            if let Some(local_scope) = local_scope {
                context.function_local_scopes.push(local_scope);
            }
            match result {
                TokenResolution::Resolved(tokens) => Some(tokens),
                TokenResolution::Invalid => None,
                TokenResolution::Cyclic => return ConditionEvaluation::Cyclic,
                TokenResolution::NotHandled => return ConditionEvaluation::NotHandled,
            }
        };
        return ConditionEvaluation::Match(match (computed, expected) {
            (None, None) => true,
            (Some(computed), Some(expected)) if local_registration.is_some() || registration.is_some() => {
                registered_style_query_values_are_equal(
                    registry.expect("registered syntax requires registry"),
                    local_registration
                        .as_ref()
                        .map(|registration| &registration.syntax)
                        .unwrap_or_else(|| &registration.expect("registered property").syntax),
                    &computed,
                    &expected,
                )
            }
            (Some(computed), Some(expected)) => trim_whitespace(&computed) == trim_whitespace(&expected),
            _ => false,
        });
    }

    let Some(computed) = computed else {
        return ConditionEvaluation::Match(false);
    };
    if let (Some(registry), Some(syntax)) = (
        registry,
        local_registration
            .as_ref()
            .map(|registration| &registration.syntax)
            .or_else(|| registration.map(|registration| &registration.syntax)),
    ) {
        return ConditionEvaluation::Match(registered_style_query_values_are_equal(
            registry, syntax, &computed, query,
        ));
    }
    ConditionEvaluation::Match(serialize_tokens(trim_whitespace(&computed)) == serialize_tokens(query))
}

fn evaluate_style_query(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    tokens: &[OwnedToken],
    context: &mut VarResolutionContext,
    recursion_depth: u32,
) -> ConditionEvaluation {
    let expression = match parse_boolean_expression(tokens, &mut validate_style_feature) {
        Ok(expression) => expression,
        Err(ConditionValidation::Invalid) => return ConditionEvaluation::Invalid,
        Err(ConditionValidation::Valid) => unreachable!(),
    };
    evaluate_parsed_boolean_expression(&expression, &mut |feature| {
        evaluate_style_feature(store, registry, feature, context, recursion_depth + 1)
    })
}

fn validate_if_test(tokens: &[OwnedToken]) -> ConditionValidation {
    let test = trim_whitespace(tokens);
    let Some(OwnedTokenKind::Function(name)) = test.first().map(|token| &token.kind) else {
        return ConditionValidation::Invalid;
    };
    let Some(close) = find_matching_close(test, 0) else {
        return ConditionValidation::Invalid;
    };
    if close != test.len() - 1 {
        return ConditionValidation::Invalid;
    }
    if name.eq_ignore_ascii_case("style") {
        return match parse_boolean_expression(&test[1..close], &mut validate_style_feature) {
            Ok(_) => ConditionValidation::Valid,
            Err(validation) => validation,
        };
    }
    if name.eq_ignore_ascii_case("media") || name.eq_ignore_ascii_case("supports") {
        return ConditionValidation::Valid;
    }
    ConditionValidation::Valid
}

fn evaluate_if_condition(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    tokens: &[OwnedToken],
    context: &mut VarResolutionContext,
    recursion_depth: u32,
) -> ConditionEvaluation {
    if matches!(
        trim_whitespace(tokens),
        [OwnedToken {
            kind: OwnedTokenKind::Ident(keyword),
            ..
        }] if keyword.eq_ignore_ascii_case("else")
    ) {
        return ConditionEvaluation::Match(true);
    }
    let expression = match parse_boolean_expression(tokens, &mut validate_if_test) {
        Ok(expression) => expression,
        Err(ConditionValidation::Invalid) => return ConditionEvaluation::Invalid,
        Err(ConditionValidation::Valid) => unreachable!(),
    };
    evaluate_parsed_boolean_expression(&expression, &mut |test| {
        let test = trim_whitespace(test);
        let OwnedTokenKind::Function(name) = &test[0].kind else {
            unreachable!("validated if condition test")
        };
        let close = test.len() - 1;
        if name.eq_ignore_ascii_case("style") {
            return evaluate_style_query(store, registry, &test[1..close], context, recursion_depth + 1);
        }
        if name.eq_ignore_ascii_case("media") || name.eq_ignore_ascii_case("supports") {
            let Some(evaluate) = context.evaluate_condition else {
                return ConditionEvaluation::NotHandled;
            };
            let source = serialize_tokens(&test[1..close]);
            return match unsafe {
                evaluate(
                    context.condition_context,
                    u8::from(name.eq_ignore_ascii_case("supports")),
                    FfiUtf16View {
                        ascii: std::ptr::null(),
                        utf16: source.as_ptr(),
                        length: source.len(),
                    },
                )
            } {
                0 => ConditionEvaluation::Match(false),
                1 => ConditionEvaluation::Match(true),
                2 => ConditionEvaluation::Invalid,
                3 => ConditionEvaluation::Cyclic,
                _ => ConditionEvaluation::NotHandled,
            };
        }
        ConditionEvaluation::Match(false)
    })
}

fn split_function_arguments(tokens: &[OwnedToken]) -> Option<Vec<&[OwnedToken]>> {
    if trim_whitespace(tokens).is_empty() {
        return Some(Vec::new());
    }
    let mut arguments = Vec::new();
    let mut start = 0;
    loop {
        let remaining = &tokens[start..];
        let Some(comma) = find_top_level_comma(remaining) else {
            arguments.push(remaining);
            break;
        };
        arguments.push(&remaining[..comma]);
        start += comma + 1;
        if start > tokens.len() {
            return None;
        }
    }
    Some(arguments)
}

fn replace_custom_function(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    name: &[u16],
    arguments: &[OwnedToken],
    context: &mut VarResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    let Some(functions) = context.custom_functions else {
        return TokenResolution::NotHandled;
    };
    let caller_scope_identity = context
        .active_functions
        .last()
        .and_then(|identity| {
            functions
                .definitions
                .iter()
                .find(|definition| definition.identity == *identity)
                .map(|definition| definition.scope_identity)
        })
        .unwrap_or(functions.caller_scope_identity);
    let resolved_identity = context.resolve_custom_function.map(|resolve| unsafe {
        resolve(
            caller_scope_identity,
            FfiUtf16View {
                ascii: std::ptr::null(),
                utf16: name.as_ptr(),
                length: name.len(),
            },
        )
    });
    let definition = resolved_identity
        .and_then(|identity| {
            functions
                .definitions
                .iter()
                .find(|definition| definition.identity == identity)
        })
        .or_else(|| {
            resolved_identity.is_none().then(|| {
                functions
                    .definitions
                    .iter()
                    .find(|definition| definition.name == name && definition.scope_identity == caller_scope_identity)
            })?
        })
        .cloned();
    let Some(definition) = definition else {
        return TokenResolution::Invalid;
    };
    if let Some(cycle_start) = context
        .active_functions
        .iter()
        .position(|identity| *identity == definition.identity)
    {
        context
            .cyclic_functions
            .extend(context.active_functions[cycle_start..].iter().copied());
        return TokenResolution::Cyclic;
    }
    let Some(argument_slices) = split_function_arguments(arguments) else {
        return TokenResolution::Invalid;
    };
    if argument_slices.len() > definition.parameters.len() {
        return TokenResolution::Invalid;
    }
    let mut substituted_arguments = Vec::with_capacity(argument_slices.len());
    for argument in argument_slices {
        let mut argument = trim_whitespace(argument);
        if matches!(
            argument.first().map(|token| &token.kind),
            Some(OwnedTokenKind::OpenCurly)
        ) && find_matching_close(argument, 0) == Some(argument.len() - 1)
        {
            argument = &argument[1..argument.len() - 1];
        }
        substituted_arguments.push(substitute_tokens(
            store,
            registry,
            argument,
            context,
            recursion_depth + 1,
        ));
    }

    let mut argument_values = HashMap::new();
    let mut argument_registrations = HashMap::new();
    for (index, parameter) in definition.parameters.iter().enumerate() {
        if index >= substituted_arguments.len() && parameter.default_tokens.is_none() {
            return TokenResolution::Invalid;
        }
        let argument = substituted_arguments.get(index);
        let normalized_argument = match argument {
            Some(TokenResolution::Resolved(tokens)) => normalize_function_tokens(registry, &parameter.syntax, tokens),
            Some(TokenResolution::NotHandled) => return TokenResolution::NotHandled,
            Some(TokenResolution::Cyclic) => TokenResolution::Invalid,
            Some(TokenResolution::Invalid) | None => TokenResolution::Invalid,
        };
        let value = match normalized_argument {
            TokenResolution::Resolved(tokens) => Some(FunctionLocalValue {
                tokens,
                includes_substitution: false,
            }),
            TokenResolution::NotHandled => return TokenResolution::NotHandled,
            TokenResolution::Invalid | TokenResolution::Cyclic => {
                parameter.default_tokens.as_ref().map(|tokens| FunctionLocalValue {
                    tokens: trim_whitespace(tokens).to_vec(),
                    includes_substitution: true,
                })
            }
        };
        if let Some(value) = value {
            argument_values.insert(parameter.name.clone(), value);
        }
        argument_registrations.insert(
            parameter.name.clone(),
            FunctionLocalRegistration {
                syntax: parameter.syntax.clone(),
                initial_tokens: None,
                is_result: false,
            },
        );
    }

    context.active_functions.push(definition.identity);
    context.function_local_scopes.push(FunctionLocalScope {
        values: argument_values,
        registrations: argument_registrations,
    });
    let mut resolved_arguments = HashMap::new();
    for parameter in &definition.parameters {
        if let TokenResolution::Resolved(tokens) =
            resolve_custom_property(store, registry, &parameter.name, context, recursion_depth + 1)
        {
            resolved_arguments.insert(parameter.name.clone(), tokens);
        }
    }
    context.function_local_scopes.pop();

    let mut values = HashMap::new();
    let mut registrations = HashMap::new();
    for parameter in &definition.parameters {
        let initial_tokens = resolved_arguments.get(&parameter.name).cloned();
        if let Some(tokens) = &initial_tokens {
            values.insert(
                parameter.name.clone(),
                FunctionLocalValue {
                    tokens: tokens.clone(),
                    includes_substitution: false,
                },
            );
        }
        registrations.insert(
            parameter.name.clone(),
            FunctionLocalRegistration {
                syntax: parameter.syntax.clone(),
                initial_tokens,
                is_result: false,
            },
        );
    }
    registrations.insert(
        b"result".iter().copied().map(u16::from).collect(),
        FunctionLocalRegistration {
            syntax: definition.return_syntax.clone(),
            initial_tokens: None,
            is_result: true,
        },
    );
    for (name, tokens, includes_substitution) in &definition.declarations {
        values.insert(
            name.clone(),
            FunctionLocalValue {
                tokens: tokens.clone(),
                includes_substitution: *includes_substitution,
            },
        );
    }
    context
        .function_local_scopes
        .push(FunctionLocalScope { values, registrations });
    let result_name: Vec<u16> = b"result".iter().copied().map(u16::from).collect();
    for (name, _, _) in &definition.declarations {
        if name != &result_name {
            let _ = resolve_custom_property(store, registry, name, context, recursion_depth + 1);
        }
    }
    let result = resolve_custom_property(store, registry, &result_name, context, recursion_depth + 1);
    context.function_local_scopes.pop();
    let active_function = context.active_functions.pop().expect("active custom function");
    debug_assert_eq!(active_function, definition.identity);
    if context.cyclic_functions.contains(&definition.identity) {
        TokenResolution::Cyclic
    } else {
        result
    }
}

fn replace_if_function(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    arguments: &[OwnedToken],
    context: &mut VarResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    // https://drafts.csswg.org/css-values-5/#replace-an-if-function
    let mut branch_start = 0;
    while branch_start < arguments.len() {
        let branch_length =
            find_top_level_source(&arguments[branch_start..], b";").unwrap_or(arguments.len() - branch_start);
        let branch_end = branch_start + branch_length;
        let branch = &arguments[branch_start..branch_end];
        let Some(colon) = find_top_level_source(branch, b":") else {
            return TokenResolution::Invalid;
        };
        let condition = match substitute_tokens(store, registry, &branch[..colon], context, recursion_depth + 1) {
            TokenResolution::Resolved(tokens) => tokens,
            TokenResolution::Invalid => branch[..colon].to_vec(),
            TokenResolution::Cyclic
                if context
                    .active_names
                    .first()
                    .is_some_and(|root_name| context.cyclic_names.contains(root_name)) =>
            {
                return TokenResolution::Cyclic;
            }
            TokenResolution::Cyclic => branch[..colon].to_vec(),
            TokenResolution::NotHandled => return TokenResolution::NotHandled,
        };
        match evaluate_if_condition(store, registry, &condition, context, recursion_depth + 1) {
            ConditionEvaluation::Match(true) => {
                return substitute_tokens(store, registry, &branch[colon + 1..], context, recursion_depth + 1);
            }
            ConditionEvaluation::Match(false) | ConditionEvaluation::Invalid => {}
            ConditionEvaluation::Cyclic => return TokenResolution::Cyclic,
            ConditionEvaluation::NotHandled => return TokenResolution::NotHandled,
        }
        branch_start = branch_end + 1;
    }
    TokenResolution::Resolved(Vec::new())
}

enum AttrSyntax {
    Omitted,
    RawString,
    Number,
    Unit(Vec<u16>),
    Syntax(SyntaxNode),
}

enum AttrSyntaxParseFailure {
    SyntaxOmitted,
    SyntaxSpecified,
}

fn parse_attr_syntax(tokens: &[OwnedToken]) -> Result<(Vec<u16>, AttrSyntax), AttrSyntaxParseFailure> {
    let tokens = trim_whitespace(tokens);
    let Some(first) = tokens.first() else {
        return Err(AttrSyntaxParseFailure::SyntaxOmitted);
    };
    let OwnedTokenKind::Ident(name) = &first.kind else {
        return Err(AttrSyntaxParseFailure::SyntaxOmitted);
    };
    let rest = trim_whitespace(&tokens[1..]);
    if rest.is_empty() {
        return Ok((name.clone(), AttrSyntax::Omitted));
    }
    if let OwnedTokenKind::Ident(syntax) = &rest[0].kind {
        let syntax = if syntax.eq_ignore_ascii_case("raw-string") {
            AttrSyntax::RawString
        } else if syntax.eq_ignore_ascii_case("number") {
            AttrSyntax::Number
        } else if crate::css::parser::value_parser::is_dimension_unit(syntax) {
            AttrSyntax::Unit(syntax.clone())
        } else {
            return Err(AttrSyntaxParseFailure::SyntaxOmitted);
        };
        if rest.len() != 1 {
            return Err(AttrSyntaxParseFailure::SyntaxSpecified);
        }
        return Ok((name.clone(), syntax));
    }
    if matches!(rest[0].kind, OwnedTokenKind::Delim(37)) {
        if rest.len() != 1 {
            return Err(AttrSyntaxParseFailure::SyntaxSpecified);
        }
        return Ok((name.clone(), AttrSyntax::Unit(vec![u16::from(b'%')])));
    }
    if matches!(&rest[0].kind, OwnedTokenKind::Function(function) if function.eq_ignore_ascii_case("type"))
        && let Some(close) = find_matching_close(rest, 0)
    {
        let Some(syntax) = parse_syntax(&serialize_tokens(&rest[1..close]), false) else {
            return Err(AttrSyntaxParseFailure::SyntaxOmitted);
        };
        if close != rest.len() - 1 {
            return Err(AttrSyntaxParseFailure::SyntaxSpecified);
        }
        return Ok((name.clone(), AttrSyntax::Syntax(syntax)));
    }
    Err(AttrSyntaxParseFailure::SyntaxOmitted)
}

fn parse_attr_value_with_syntax(
    registry: Option<&CustomPropertyRegistry>,
    source: &[u16],
    syntax: &SyntaxNode,
) -> Option<Vec<OwnedToken>> {
    let registry = registry?;
    let mut random_function_index = 0;
    let parsed = parse_with_syntax(&registry.parse_context(&mut random_function_index), source, syntax)?;
    let serialized = crate::css::serialize::serialize_style_value_to_utf16(&parsed)?;
    Some(tokenize_owned(&serialized))
}

fn attr_fallback(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    arguments: &[OwnedToken],
    comma: Option<usize>,
    syntax_was_omitted: bool,
    context: &mut VarResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    let Some(comma) = comma else {
        if syntax_was_omitted {
            return TokenResolution::Resolved(tokenize_owned(b"\"\""));
        }
        return TokenResolution::Invalid;
    };
    substitute_tokens(store, registry, &arguments[comma + 1..], context, recursion_depth + 1)
}

fn replace_attr_function(
    store: Option<&CustomPropertyStore>,
    registry: Option<&CustomPropertyRegistry>,
    arguments: &[OwnedToken],
    context: &mut VarResolutionContext,
    recursion_depth: u32,
) -> TokenResolution {
    // https://drafts.csswg.org/css-values-5/#replace-an-attr-function
    let comma = find_top_level_comma(arguments);
    let first_argument = &arguments[..comma.unwrap_or(arguments.len())];
    let substituted_first = match substitute_tokens(store, registry, first_argument, context, recursion_depth + 1) {
        TokenResolution::Resolved(tokens) => tokens,
        TokenResolution::Invalid | TokenResolution::Cyclic => {
            return attr_fallback(store, registry, arguments, comma, true, context, recursion_depth);
        }
        TokenResolution::NotHandled => return TokenResolution::NotHandled,
    };
    let (attribute_name, syntax) = match parse_attr_syntax(&substituted_first) {
        Ok(parsed) => parsed,
        Err(failure) => {
            return attr_fallback(
                store,
                registry,
                arguments,
                comma,
                matches!(failure, AttrSyntaxParseFailure::SyntaxOmitted),
                context,
                recursion_depth,
            );
        }
    };
    let syntax_was_omitted = matches!(syntax, AttrSyntax::Omitted);
    let attribute_value = context.attributes.and_then(|attributes| {
        attributes.get(&attribute_name).or_else(|| {
            context.attribute_names_are_ascii_case_insensitive.then(|| {
                attributes
                    .iter()
                    .find(|(name, _)| name.eq_ignore_ascii_case_utf16(&attribute_name))
                    .map(|(_, value)| value)
            })?
        })
    });
    let Some(attribute_value) = attribute_value else {
        return attr_fallback(
            store,
            registry,
            arguments,
            comma,
            syntax_was_omitted,
            context,
            recursion_depth,
        );
    };

    let resolved = match syntax {
        AttrSyntax::Omitted | AttrSyntax::RawString => tokenize_owned(
            crate::css::css_tokenizer::TokenizerInput::Utf16(&crate::css::serialize::serialize_string(attribute_value)),
        ),
        AttrSyntax::Number => {
            if !matches!(
                trim_whitespace(&tokenize_owned(attribute_value)),
                [OwnedToken {
                    kind: OwnedTokenKind::Number,
                    ..
                }]
            ) {
                return attr_fallback(store, registry, arguments, comma, false, context, recursion_depth);
            }
            let syntax = SyntaxNode::Type(crate::css::parser::syntax::SyntaxType::Number);
            let Some(tokens) = parse_attr_value_with_syntax(registry, attribute_value, &syntax) else {
                return attr_fallback(store, registry, arguments, comma, false, context, recursion_depth);
            };
            tokens
        }
        AttrSyntax::Unit(unit) => {
            if !matches!(
                trim_whitespace(&tokenize_owned(attribute_value)),
                [OwnedToken {
                    kind: OwnedTokenKind::Number,
                    ..
                }]
            ) {
                return attr_fallback(store, registry, arguments, comma, false, context, recursion_depth);
            }
            let number_syntax = SyntaxNode::Type(crate::css::parser::syntax::SyntaxType::Number);
            let Some(number) = parse_attr_value_with_syntax(registry, attribute_value, &number_syntax) else {
                return attr_fallback(store, registry, arguments, comma, false, context, recursion_depth);
            };
            let mut source = serialize_tokens(&number);
            source.extend_from_slice(&unit);
            tokenize_owned(crate::css::css_tokenizer::TokenizerInput::Utf16(&source))
        }
        AttrSyntax::Syntax(syntax) => {
            if context.cyclic_attributes.contains(&attribute_name) {
                return attr_fallback(store, registry, arguments, comma, false, context, recursion_depth);
            }
            if let Some(cycle_start) = context
                .active_attributes
                .iter()
                .position(|active_name| active_name == &attribute_name)
            {
                context
                    .cyclic_attributes
                    .extend(context.active_attributes[cycle_start..].iter().cloned());
                return TokenResolution::Cyclic;
            }
            context.active_attributes.push(attribute_name.clone());
            let substituted = match substitute_tokens(
                store,
                registry,
                &tokenize_owned(attribute_value),
                context,
                recursion_depth + 1,
            ) {
                TokenResolution::Resolved(tokens) => Some(serialize_tokens(&tokens)),
                TokenResolution::Invalid | TokenResolution::Cyclic => None,
                TokenResolution::NotHandled => {
                    context.active_attributes.pop();
                    return TokenResolution::NotHandled;
                }
            };
            let active_attribute = context.active_attributes.pop().expect("active attribute");
            debug_assert_eq!(active_attribute, attribute_name);
            let Some(substituted) = substituted.filter(|_| !context.cyclic_attributes.contains(&attribute_name)) else {
                return attr_fallback(store, registry, arguments, comma, false, context, recursion_depth);
            };
            let Some(tokens) = parse_attr_value_with_syntax(registry, &substituted, &syntax) else {
                return attr_fallback(store, registry, arguments, comma, false, context, recursion_depth);
            };
            tokens
        }
    };
    context.contains_attr_tainted_values = true;
    TokenResolution::Resolved(resolved)
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
    let mut is_cyclic = false;
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
            OwnedTokenKind::Function(name) if name.eq_ignore_ascii_case("attr") => {
                replace_attr_function(store, registry, contents, context, recursion_depth)
            }
            OwnedTokenKind::Function(name) if name.eq_ignore_ascii_case("inherit") => {
                replace_inherit_function(store, registry, contents, context, recursion_depth)
            }
            OwnedTokenKind::Function(name) if name.eq_ignore_ascii_case("env") => {
                replace_env_function(store, registry, contents, context, recursion_depth)
            }
            OwnedTokenKind::Function(name) if name.eq_ignore_ascii_case("if") => {
                replace_if_function(store, registry, contents, context, recursion_depth)
            }
            OwnedTokenKind::Function(name) if name.starts_with_ascii("--") => {
                replace_custom_function(store, registry, name, contents, context, recursion_depth)
            }
            _ => substitute_tokens(store, registry, contents, context, recursion_depth + 1),
        };
        let resolved = match resolved {
            TokenResolution::Resolved(resolved) => resolved,
            TokenResolution::Invalid => return TokenResolution::Invalid,
            TokenResolution::Cyclic => {
                is_cyclic = true;
                index = close_index + 1;
                continue;
            }
            TokenResolution::NotHandled => return TokenResolution::NotHandled,
        };

        if !matches!(tokens[index].kind, OwnedTokenKind::Function(ref name) if name.starts_with_ascii("--") || name.eq_ignore_ascii_case("var") || name.eq_ignore_ascii_case("attr") || name.eq_ignore_ascii_case("env") || name.eq_ignore_ascii_case("if") || name.eq_ignore_ascii_case("inherit"))
        {
            output.push(tokens[index].clone());
        }
        let resolved_start = usize::from(
            matches!(output.last().map(|token| &token.kind), Some(OwnedTokenKind::Whitespace))
                && matches!(
                    resolved.first().map(|token| &token.kind),
                    Some(OwnedTokenKind::Whitespace)
                ),
        );
        output.extend(resolved.into_iter().skip(resolved_start));
        if !matches!(tokens[index].kind, OwnedTokenKind::Function(ref name) if name.starts_with_ascii("--") || name.eq_ignore_ascii_case("var") || name.eq_ignore_ascii_case("attr") || name.eq_ignore_ascii_case("env") || name.eq_ignore_ascii_case("if") || name.eq_ignore_ascii_case("inherit"))
        {
            output.push(tokens[close_index].clone());
        }
        let remaining_token_count = tokens.len() - close_index - 1;
        if output.len() + remaining_token_count > MAX_SUBSTITUTED_TOKEN_COUNT {
            return TokenResolution::Invalid;
        }
        index = close_index + 1;
    }
    if is_cyclic {
        TokenResolution::Cyclic
    } else {
        TokenResolution::Resolved(output)
    }
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

#[allow(clippy::too_many_arguments)]
pub(crate) unsafe fn resolve_vars(
    store: *const c_void,
    inheritance_store: *const c_void,
    registry: *const c_void,
    root_custom_property_name: FfiUtf16View,
    value_data: *const c_void,
    environment: &mut VarResolutionEnvironment,
    attribute_names_are_ascii_case_insensitive: bool,
    resolve_custom_function: Option<unsafe extern "C" fn(usize, FfiUtf16View) -> usize>,
    condition_context: *mut c_void,
    evaluate_condition: Option<unsafe extern "C" fn(*mut c_void, u8, FfiUtf16View) -> u8>,
    final_custom_properties: Option<&HashMap<Vec<u16>, *const c_void>>,
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
    let inheritance_store = if inheritance_store.is_null() {
        None
    } else {
        Some(unsafe { &*inheritance_store.cast::<CustomPropertyStore>() })
    };
    let value_data = unsafe { &*value_data.cast::<StyleValueData>() };
    let active_names = unsafe { root_custom_property_name.to_utf16() }
        .filter(|name| !name.is_empty())
        .into_iter()
        .collect();
    let VarResolutionEnvironment {
        attributes,
        custom_functions,
        token_cache,
        resolution_stats,
    } = environment;
    let mut context = VarResolutionContext {
        active_names,
        attributes: Some(attributes),
        inheritance_store,
        attribute_names_are_ascii_case_insensitive,
        contains_attr_tainted_values: false,
        custom_functions: Some(custom_functions),
        resolve_custom_function,
        condition_context,
        evaluate_condition,
        final_custom_properties,
        token_cache: Some(token_cache),
        resolution_stats: Some(resolution_stats),
        ..Default::default()
    };
    let Some((source, includes_substitution, contains_attr_tainted_values)) =
        cached_tokens_for_custom_property_value(value_data, &mut context)
    else {
        return NativeVarResolution::NotHandled;
    };
    if !includes_substitution {
        return NativeVarResolution::NotHandled;
    }
    context.contains_attr_tainted_values = contains_attr_tainted_values;
    let result = substitute_tokens(store, registry, &source, &mut context, 0);
    if context
        .active_names
        .first()
        .is_some_and(|root_name| context.cyclic_names.contains(root_name))
    {
        return NativeVarResolution::Invalid;
    }
    match result {
        TokenResolution::Resolved(tokens) => NativeVarResolution::Resolved {
            source: serialize_tokens(&tokens),
            contains_attr_tainted_values: context.contains_attr_tainted_values,
        },
        TokenResolution::Invalid | TokenResolution::Cyclic => NativeVarResolution::Invalid,
        TokenResolution::NotHandled => NativeVarResolution::NotHandled,
    }
}

#[cfg(test)]
#[allow(clippy::items_after_test_module)]
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

    fn substitute_with_attributes(source: &str, attributes: &[(&str, &str)]) -> (TokenResolution, bool) {
        let attributes: HashMap<_, _> = attributes
            .iter()
            .map(|(name, value)| (utf16(name), utf16(value)))
            .collect();
        let mut context = VarResolutionContext {
            attributes: Some(&attributes),
            ..Default::default()
        };
        let result = substitute_tokens(None, None, &tokenize_owned(source.as_bytes()), &mut context, 0);
        (result, context.contains_attr_tainted_values)
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
    fn root_custom_property_cycles_do_not_take_var_fallbacks() {
        let mut context = VarResolutionContext {
            active_names: vec![utf16("--root")],
            ..Default::default()
        };
        assert!(matches!(
            substitute_tokens(None, None, &tokenize_owned(b"var(--root, fallback)"), &mut context, 0),
            TokenResolution::Cyclic
        ));
    }

    #[test]
    fn substitutes_environment_values_and_fallbacks() {
        let TokenResolution::Resolved(tokens) = substitute_without_custom_properties("env(safe-area-inset-top)") else {
            panic!("expected safe-area environment substitution");
        };
        assert_eq!(serialize_tokens(&tokens), utf16("0px"));

        let TokenResolution::Resolved(tokens) =
            substitute_without_custom_properties("env(unknown-environment-variable, 4px)")
        else {
            panic!("expected environment fallback");
        };
        assert_eq!(serialize_tokens(&tokens), utf16(" 4px"));

        assert!(matches!(
            substitute_without_custom_properties("env(\"unknown-environment-variable\", 4px)"),
            TokenResolution::Invalid
        ));
        assert!(matches!(
            substitute_without_custom_properties("env(safe-area-inset-top 1.5, 4px)"),
            TokenResolution::Invalid
        ));
    }

    #[test]
    fn substitutes_unconditional_if_branches() {
        let TokenResolution::Resolved(tokens) = substitute_without_custom_properties("if(else: 5px)") else {
            panic!("expected unconditional branch substitution");
        };
        assert_eq!(serialize_tokens(&tokens), utf16(" 5px"));
    }

    #[test]
    fn substitutes_custom_functions_from_a_snapshot() {
        let functions = CustomFunctionRegistry {
            caller_scope_identity: 1,
            definitions: vec![CustomFunctionDefinition {
                identity: 2,
                scope_identity: 1,
                name: utf16("--echo"),
                parameters: vec![CustomFunctionParameter {
                    name: utf16("--value"),
                    syntax: SyntaxNode::Universal,
                    default_tokens: None,
                }],
                return_syntax: SyntaxNode::Universal,
                declarations: vec![(utf16("result"), tokenize_owned(b"var(--value)"), true)],
            }],
        };
        let mut context = VarResolutionContext {
            custom_functions: Some(&functions),
            ..Default::default()
        };
        let TokenResolution::Resolved(tokens) =
            substitute_tokens(None, None, &tokenize_owned(b"--echo(12px)"), &mut context, 0)
        else {
            panic!("expected custom function substitution");
        };
        assert_eq!(serialize_tokens(&tokens), utf16("12px"));
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
    fn substitutes_raw_attributes_and_marks_taint() {
        let (TokenResolution::Resolved(tokens), contains_attr_tainted_values) =
            substitute_with_attributes("attr(data-value)", &[("data-value", "hello")])
        else {
            panic!("expected attribute substitution");
        };
        assert_eq!(serialize_tokens(&tokens), utf16("\"hello\""));
        assert!(contains_attr_tainted_values);
    }

    #[test]
    fn missing_untyped_attribute_becomes_an_empty_string() {
        let (TokenResolution::Resolved(tokens), contains_attr_tainted_values) =
            substitute_with_attributes("attr(data-value)", &[])
        else {
            panic!("expected missing-attribute substitution");
        };
        assert_eq!(serialize_tokens(&tokens), utf16("\"\""));
        assert!(!contains_attr_tainted_values);
    }

    #[test]
    fn accepts_all_dimension_units_and_rejects_unknown_units() {
        let Ok((_, AttrSyntax::Unit(unit))) = parse_attr_syntax(&tokenize_owned(b"data-value FR")) else {
            panic!("expected flex unit syntax");
        };
        assert_eq!(unit, utf16("FR"));
        assert!(parse_attr_syntax(&tokenize_owned(b"data-value unknown-unit")).is_err());
    }

    #[test]
    fn html_attribute_names_are_ascii_case_insensitive() {
        let attributes = HashMap::from([(utf16("data-value"), utf16("hello"))]);
        let mut context = VarResolutionContext {
            attributes: Some(&attributes),
            attribute_names_are_ascii_case_insensitive: true,
            ..Default::default()
        };
        let TokenResolution::Resolved(tokens) =
            substitute_tokens(None, None, &tokenize_owned(b"attr(DATA-VALUE)"), &mut context, 0)
        else {
            panic!("expected case-insensitive attribute substitution");
        };
        assert_eq!(serialize_tokens(&tokens), utf16("\"hello\""));
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
            let Some(syntax) = (unsafe { clone_syntax_handle(registration.syntax) }) else {
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
/// strong style value data handle. The structural and inheritance parents are other Arc raw
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
            unsafe { Arc::increment_strong_count(parent) };
            Some(unsafe { Arc::from_raw(parent) })
        };
        let inheritance_parent = if inheritance_parent.is_null() {
            None
        } else {
            let inheritance_parent = inheritance_parent.cast::<CustomPropertyStore>();
            unsafe { Arc::increment_strong_count(inheritance_parent) };
            Some(unsafe { Arc::from_raw(inheritance_parent) })
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
        Arc::into_raw(Arc::new(CustomPropertyStore {
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
    abort_on_panic(|| drop(unsafe { Arc::from_raw(store.cast::<CustomPropertyStore>()) }));
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
