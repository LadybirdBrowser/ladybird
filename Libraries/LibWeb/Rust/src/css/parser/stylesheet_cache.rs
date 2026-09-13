/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::syntax_parser::ParsedStyleSheet;
use super::value_parser::{FfiValueParsingContext, ParseContext};
use crate::css::css_tokenizer::TokenizerInput;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::style_compute::{FfiFontMetrics, FfiLengthResolutionContext};
use std::cell::Cell;
use std::collections::{HashMap, hash_map::RandomState};
use std::hash::{BuildHasher, Hash, Hasher};
use std::sync::{Arc, Mutex, OnceLock, Weak};

#[derive(PartialEq, Eq, Hash)]
struct LengthResolutionKey {
    viewport: [u64; 2],
    font: [u64; 5],
    root_font: [u64; 5],
    container: [u64; 2],
    flags: [bool; 7],
}

impl LengthResolutionKey {
    fn new(context: &FfiLengthResolutionContext) -> Option<Self> {
        let FfiLengthResolutionContext {
            viewport_width,
            viewport_height,
            font_metrics,
            root_font_metrics,
            font_metrics_depend_on_viewport_metrics,
            root_font_metrics_depend_on_viewport_metrics,
            has_container_width_basis,
            has_container_height_basis,
            container_width_basis,
            container_height_basis,
            container_width_basis_depends_on_viewport_metrics,
            container_height_basis_depends_on_viewport_metrics,
            subject_inline_axis_is_horizontal,
            resolved_viewport_relative_length,
        } = *context;
        // A cache hit cannot reproduce writes to an external dependency-tracking flag.
        if !resolved_viewport_relative_length.is_null() {
            return None;
        }
        let font_key = |font: &FfiFontMetrics| {
            let FfiFontMetrics {
                font_size,
                x_height,
                cap_height,
                zero_advance,
                line_height,
            } = *font;
            [
                font_size.to_bits(),
                x_height.to_bits(),
                cap_height.to_bits(),
                zero_advance.to_bits(),
                line_height.to_bits(),
            ]
        };
        Some(Self {
            viewport: [viewport_width.to_bits(), viewport_height.to_bits()],
            font: font_key(&font_metrics),
            root_font: font_key(&root_font_metrics),
            container: [container_width_basis.to_bits(), container_height_basis.to_bits()],
            flags: [
                font_metrics_depend_on_viewport_metrics,
                root_font_metrics_depend_on_viewport_metrics,
                has_container_width_basis,
                has_container_height_basis,
                container_width_basis_depends_on_viewport_metrics,
                container_height_basis_depends_on_viewport_metrics,
                subject_inline_axis_is_horizontal,
            ],
        })
    }
}

#[derive(PartialEq, Eq, Hash)]
struct ValueContextKey {
    kind: u8,
    value: u16,
    secondary_value: u16,
    name: Box<[u16]>,
}

#[derive(PartialEq, Eq, Hash)]
struct ParsingInputs {
    flags: [bool; 5],
    value_contexts: Vec<ValueContextKey>,
    declared_namespaces: Vec<Box<[u16]>>,
    random_function_index: Option<usize>,
}

struct ContextKey {
    inputs: ParsingInputs,
    document_base_url: Box<[u16]>,
    length_resolution: Option<LengthResolutionKey>,
}

#[derive(Clone, Copy, Default)]
struct ParseDependencies {
    base_url: bool,
    length_resolution: bool,
}

impl ParseDependencies {
    fn index(self) -> usize {
        usize::from(self.base_url) | (usize::from(self.length_resolution) << 1)
    }

    fn record(self) {
        let previous = PARSE_DEPENDENCIES.get();
        PARSE_DEPENDENCIES.set(Self {
            base_url: previous.base_url || self.base_url,
            length_resolution: previous.length_resolution || self.length_resolution,
        });
    }
}

thread_local! {
    static PARSE_DEPENDENCIES: Cell<ParseDependencies> = const { Cell::new(ParseDependencies {
        base_url: false,
        length_resolution: false,
    }) };
}

pub(super) fn record_base_url_dependency() {
    ParseDependencies {
        base_url: true,
        ..Default::default()
    }
    .record();
}

pub(super) fn record_length_resolution_dependency() {
    ParseDependencies {
        length_resolution: true,
        ..Default::default()
    }
    .record();
}

struct ParseDependencyScope {
    previous: ParseDependencies,
}

impl ParseDependencyScope {
    fn new() -> Self {
        Self {
            previous: PARSE_DEPENDENCIES.replace(ParseDependencies::default()),
        }
    }

    fn dependencies(&self) -> ParseDependencies {
        PARSE_DEPENDENCIES.get()
    }
}

impl Drop for ParseDependencyScope {
    fn drop(&mut self) {
        // Propagate dependencies if a parser invokes another cached parse on this thread.
        self.previous.record();
    }
}

fn own_text(source: TokenizerInput<'_>) -> Box<[u16]> {
    let mut text = Vec::with_capacity(source.len());
    source.append_to(&mut text);
    text.into_boxed_slice()
}

unsafe fn own_view(view: FfiUtf16View) -> Option<Box<[u16]>> {
    Some(own_text(unsafe { view.units() }?))
}

unsafe fn own_url(pointer: *const u8, length: usize) -> Option<Box<[u16]>> {
    if length == 0 {
        return Some(Box::default());
    }
    // The legacy URL context is UTF-8. Keep the cache's owned strings in native UTF-16.
    let bytes = unsafe { std::slice::from_raw_parts(pointer, length) };
    Some(std::str::from_utf8(bytes).ok()?.encode_utf16().collect())
}

impl ContextKey {
    unsafe fn new(context: &ParseContext) -> Option<Self> {
        // Keep this exhaustive so new parsing inputs cannot silently bypass the cache key.
        let ParseContext {
            in_quirks_mode,
            is_svg_presentation_attribute,
            is_substituted_value,
            contains_attr_tainted_values,
            is_ua_style_sheet,
            value_contexts: borrowed_value_contexts,
            value_context_count,
            declared_namespaces: borrowed_declared_namespaces,
            // Parsed values do not use the document URL. Image values capture the base URL.
            document_url: _,
            document_url_length: _,
            document_base_url,
            document_base_url_length,
            length_resolution_context,
            random_function_index,
        } = *context;
        let mut value_contexts = Vec::with_capacity(value_context_count);
        for index in 0..value_context_count {
            let FfiValueParsingContext {
                kind,
                value,
                secondary_value,
                name,
            } = unsafe { *borrowed_value_contexts.add(index) };
            value_contexts.push(ValueContextKey {
                kind: kind as u8,
                value,
                secondary_value,
                name: unsafe { own_view(name) }?,
            });
        }
        let declared_namespaces = unsafe { borrowed_declared_namespaces.as_ref() }
            .into_iter()
            .flat_map(|namespaces| namespaces.prefixes.iter())
            .map(|namespace| namespace.units().into())
            .collect();
        let length_resolution = match unsafe { length_resolution_context.cast::<FfiLengthResolutionContext>().as_ref() }
        {
            Some(context) => Some(LengthResolutionKey::new(context)?),
            None => None,
        };
        Some(Self {
            inputs: ParsingInputs {
                flags: [
                    in_quirks_mode,
                    is_svg_presentation_attribute,
                    is_substituted_value,
                    contains_attr_tainted_values,
                    is_ua_style_sheet,
                ],
                value_contexts,
                declared_namespaces,
                random_function_index: unsafe { random_function_index.as_ref() }.copied(),
            },
            document_base_url: unsafe { own_url(document_base_url, document_base_url_length) }?,
            length_resolution,
        })
    }
}

struct StyleSheetKey {
    text: Box<[u16]>,
    context: Option<ContextKey>,
}

impl Hash for StyleSheetKey {
    fn hash<H: Hasher>(&self, state: &mut H) {
        self.text.hash(state);
        self.context.as_ref().map(|context| &context.inputs).hash(state);
    }
}

impl StyleSheetKey {
    fn matches(&self, other: &Self, dependencies: ParseDependencies) -> bool {
        self.text == other.text
            && match (&self.context, &other.context) {
                (None, None) => true,
                (Some(left), Some(right)) => {
                    left.inputs == right.inputs
                        && (!dependencies.base_url || left.document_base_url == right.document_base_url)
                        && (!dependencies.length_resolution || left.length_resolution == right.length_resolution)
                }
                _ => false,
            }
    }
}

pub(super) struct CachedParseMetadata {
    key: StyleSheetKey,
    dependencies: ParseDependencies,
    final_random_function_index: Option<usize>,
}

type Entries = HashMap<u64, Vec<Weak<ParsedStyleSheet>>>;

fn find(entries: &Entries, hash: u64, key: &StyleSheetKey) -> Option<Arc<ParsedStyleSheet>> {
    entries.get(&hash)?.iter().filter_map(Weak::upgrade).find(|sheet| {
        sheet
            .cache_metadata
            .as_ref()
            .is_some_and(|metadata| metadata.key.matches(key, metadata.dependencies))
    })
}

/// # Safety
/// All pointers in `context` must remain valid for this call, just as for parsing.
pub(super) unsafe fn parse_with_cache(
    source: TokenizerInput<'_>,
    context: *const ParseContext,
    parse: impl FnOnce() -> ParsedStyleSheet,
) -> Arc<ParsedStyleSheet> {
    let context = unsafe { context.as_ref() };
    let context_key = match context {
        Some(context) => match unsafe { ContextKey::new(context) } {
            Some(key) => Some(key),
            None => return Arc::new(parse()),
        },
        None => None,
    };
    let key = StyleSheetKey {
        text: own_text(source),
        context: context_key,
    };
    static HASHER: OnceLock<RandomState> = OnceLock::new();
    static CACHE: OnceLock<Mutex<Entries>> = OnceLock::new();
    let hash = HASHER.get_or_init(RandomState::new).hash_one(&key);
    // Keep context-dependent variants in separate buckets so many documents using one
    // contextual sheet do not create a linear scan. Independent sheets use the common key.
    let hasher = HASHER.get().unwrap();
    let base_url = key.context.as_ref().map(|context| &context.document_base_url);
    let lengths = key.context.as_ref().map(|context| &context.length_resolution);
    let hashes = [
        hash,
        hasher.hash_one((hash, 1_u8, base_url)),
        hasher.hash_one((hash, 2_u8, lengths)),
        hasher.hash_one((hash, 3_u8, base_url, lengths)),
    ];
    let cache = CACHE.get_or_init(Mutex::default);
    let cached = {
        let entries = cache.lock().unwrap();
        hashes.iter().find_map(|&hash| find(&entries, hash, &key))
    };
    if let Some(sheet) = cached {
        sheet.cache_metadata.as_ref().unwrap().dependencies.record();
        if let Some(counter) = context.and_then(|context| unsafe { context.random_function_index.as_mut() }) {
            *counter = sheet
                .cache_metadata
                .as_ref()
                .unwrap()
                .final_random_function_index
                .unwrap();
        }
        return sheet;
    }

    // Never hold the cache lock while parsing. Workers parsing different sheets run independently.
    let dependency_scope = ParseDependencyScope::new();
    let mut sheet = parse();
    let dependencies = dependency_scope.dependencies();
    drop(dependency_scope);
    let hash = hashes[dependencies.index()];
    let mut entries = cache.lock().unwrap();
    // Another worker may have published the same sheet while this worker was parsing.
    if let Some(sheet) = find(&entries, hash, &key) {
        return sheet;
    }
    entries.retain(|_, bucket| {
        bucket.retain(|sheet| sheet.strong_count() != 0);
        !bucket.is_empty()
    });
    sheet.cache_metadata = Some(CachedParseMetadata {
        key,
        dependencies,
        final_random_function_index: context
            .and_then(|context| unsafe { context.random_function_index.as_ref() })
            .copied(),
    });
    let sheet = Arc::new(sheet);
    entries.entry(hash).or_default().push(Arc::downgrade(&sheet));
    sheet
}
