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
use std::collections::{HashMap, hash_map::RandomState};
use std::hash::BuildHasher;
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
struct ContextKey {
    flags: [bool; 5],
    value_contexts: Vec<ValueContextKey>,
    declared_namespaces: Vec<Box<[u16]>>,
    document_url: Box<[u16]>,
    document_base_url: Box<[u16]>,
    length_resolution: Option<LengthResolutionKey>,
    random_function_index: Option<usize>,
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
            document_url,
            document_url_length,
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
            flags: [
                in_quirks_mode,
                is_svg_presentation_attribute,
                is_substituted_value,
                contains_attr_tainted_values,
                is_ua_style_sheet,
            ],
            value_contexts,
            declared_namespaces,
            document_url: unsafe { own_url(document_url, document_url_length) }?,
            document_base_url: unsafe { own_url(document_base_url, document_base_url_length) }?,
            length_resolution,
            random_function_index: unsafe { random_function_index.as_ref() }.copied(),
        })
    }
}

#[derive(PartialEq, Eq, Hash)]
struct StyleSheetKey {
    text: Box<[u16]>,
    context: Option<ContextKey>,
}

pub(super) struct CachedParseMetadata {
    key: StyleSheetKey,
    final_random_function_index: Option<usize>,
}

type Entries = HashMap<u64, Vec<Weak<ParsedStyleSheet>>>;

fn find(entries: &Entries, hash: u64, key: &StyleSheetKey) -> Option<Arc<ParsedStyleSheet>> {
    entries.get(&hash)?.iter().filter_map(Weak::upgrade).find(|sheet| {
        sheet
            .cache_metadata
            .as_ref()
            .is_some_and(|metadata| &metadata.key == key)
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
    let cache = CACHE.get_or_init(Mutex::default);
    if let Some(sheet) = find(&cache.lock().unwrap(), hash, &key) {
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
    let mut sheet = parse();
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
        final_random_function_index: context
            .and_then(|context| unsafe { context.random_function_index.as_ref() })
            .copied(),
    });
    let sheet = Arc::new(sheet);
    entries.entry(hash).or_default().push(Arc::downgrade(&sheet));
    sheet
}
