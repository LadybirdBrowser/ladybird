/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A document's style transaction as a job its render state runs: the host hands it the transaction's root and the
//! document's computation inputs, and the job answers with the reactions the host applies, which the host keeps until
//! it ends the transaction.

use super::StyleEngine;
use super::bridge::{
    FfiCustomFunctionEntry, FfiDocumentStyleComputationInputs, FfiHostHandle, FfiStyleSheetResourceContextEntry,
    FfiStyleTransactionOutput, FfiStyleTransactionView, take_style_transaction,
};
use super::engine_calls::{StyleAnswer, StyleQuery, ask_engine};
use super::identities::StyleNodeIdAllocator;
use super::tree::StyleNodeID;
use crate::css::custom_properties::{CustomPropertyRegistry, retain_custom_property_registry};
use crate::css::parser::query_parser::FfiMediaFeatureValue;
use crate::css::rule::CompiledFunction;
use crate::css::style_compute::FfiLengthResolutionContext;
use crate::render_state::{
    DocumentHost, DocumentId, RenderJob, RenderMessage, ReplyTo, SpentWait, StyleJobPermit, force_read, run_job,
};
use std::sync::Arc;

/// Takes the pending style transaction under `root`, with the document computation inputs the host sealed for it.
pub(crate) struct StyleJob {
    root: StyleNodeID,
    computation_inputs: SealedStyleInputs,
}

/// A style transaction's document computation inputs, sealed on the host's thread: the job owns a copy of every buffer
/// the host lent with them, and a reference to each custom function and to the custom-property registry they name, so
/// it reads nothing the host goes on writing.
pub(crate) struct SealedStyleInputs {
    /// The inputs, whose borrowed fields name the buffers below.
    inputs: FfiDocumentStyleComputationInputs,
    _document_base_url: Box<[u8]>,
    _resource_context_urls: Box<[u8]>,
    _resource_contexts: Box<[FfiStyleSheetResourceContextEntry]>,
    _media_feature_values: Box<[FfiMediaFeatureValue]>,
    _media_length_resolution_context: Option<Box<FfiLengthResolutionContext>>,
    _custom_functions: Box<[FfiCustomFunctionEntry]>,
    _retained_functions: Box<[Arc<CompiledFunction>]>,
    _custom_property_registry: Option<Arc<CustomPropertyRegistry>>,
}

// SAFETY: Every pointer the sealed inputs hold names one of their own buffers, a custom function they hold a
// reference to, or the registry they hold one to, none of which the host writes.
unsafe impl Send for SealedStyleInputs {}

/// The `count` values at `values`, or none.
///
/// # Safety
///
/// `values` must be null or point at `count` readable values.
unsafe fn lent_values<'a, T>(values: *const T, count: usize) -> &'a [T] {
    if values.is_null() || count == 0 {
        return &[];
    }
    // SAFETY: Guaranteed by the caller.
    unsafe { std::slice::from_raw_parts(values, count) }
}

impl SealedStyleInputs {
    /// Seals the inputs `lent` and what their borrowed fields name.
    ///
    /// # Safety
    ///
    /// The borrowed fields of `lent` must name live buffers of their stated lengths, live custom functions and the
    /// document's live custom-property registry, or nothing, for this call.
    unsafe fn seal(lent: FfiDocumentStyleComputationInputs) -> Self {
        // SAFETY: Guaranteed by the caller.
        let (document_base_url, lent_contexts, media_feature_values, lent_functions) = unsafe {
            (
                Box::<[u8]>::from(lent_bytes(lent.document_base_url, lent.document_base_url_length)),
                lent_values(
                    lent.style_sheet_resource_contexts
                        .as_pointer()
                        .cast::<FfiStyleSheetResourceContextEntry>(),
                    lent.style_sheet_resource_context_count,
                ),
                Box::<[FfiMediaFeatureValue]>::from(lent_values(
                    lent.media_feature_values.as_pointer().cast::<FfiMediaFeatureValue>(),
                    lent.media_feature_value_count,
                )),
                lent_values(
                    lent.custom_functions.as_pointer().cast::<FfiCustomFunctionEntry>(),
                    lent.custom_function_count,
                ),
            )
        };
        // Every context's base URL goes into one buffer.
        let resource_context_urls: Box<[u8]> = lent_contexts
            .iter()
            // SAFETY: Guaranteed by the caller.
            .flat_map(|entry| unsafe { lent_values(entry.base_url, entry.base_url_length) })
            .copied()
            .collect();
        let mut offset = 0;
        let resource_contexts: Box<[_]> = lent_contexts
            .iter()
            .map(|entry| {
                let base_url = resource_context_urls[offset..].as_ptr();
                offset += entry.base_url_length;
                FfiStyleSheetResourceContextEntry {
                    source_identity: entry.source_identity,
                    base_url,
                    base_url_length: entry.base_url_length,
                    has_base_url: entry.has_base_url,
                    origin_clean: entry.origin_clean,
                }
            })
            .collect();
        // The output flag of the length context is the host's, and the engine keeps none.
        // SAFETY: Guaranteed by the caller.
        let media_length_resolution_context = unsafe {
            lent.media_length_resolution_context
                .as_pointer()
                .cast::<FfiLengthResolutionContext>()
                .as_ref()
        }
        .map(|&context| {
            Box::new(FfiLengthResolutionContext {
                resolved_viewport_relative_length: std::ptr::null_mut(),
                ..context
            })
        });
        let custom_functions: Box<[_]> = lent_functions
            .iter()
            .map(|entry| FfiCustomFunctionEntry {
                function: entry.function,
                caller_scope: entry.caller_scope,
                definition_scope: entry.definition_scope,
                tree_scope: entry.tree_scope,
            })
            .collect();
        let retained_functions = lent_functions
            .iter()
            .filter(|entry| !entry.function.is_null())
            .map(|entry| {
                let function = entry.function.cast::<CompiledFunction>();
                // SAFETY: Guaranteed by the caller: the host lends a live compiled function, shared by reference count.
                unsafe {
                    Arc::increment_strong_count(function);
                    Arc::from_raw(function)
                }
            })
            .collect();
        // SAFETY: Guaranteed by the caller.
        let custom_property_registry =
            unsafe { retain_custom_property_registry(lent.custom_property_registry.as_pointer()) };
        let inputs = FfiDocumentStyleComputationInputs {
            document_base_url: FfiHostHandle::from_pointer(document_base_url.as_ptr().cast()),
            style_sheet_resource_contexts: FfiHostHandle::from_pointer(resource_contexts.as_ptr().cast()),
            media_feature_values: FfiHostHandle::from_pointer(media_feature_values.as_ptr().cast()),
            media_length_resolution_context: media_length_resolution_context
                .as_deref()
                .map_or_else(FfiHostHandle::default, |context| {
                    FfiHostHandle::from_pointer(std::ptr::from_ref(context).cast())
                }),
            custom_functions: FfiHostHandle::from_pointer(custom_functions.as_ptr().cast()),
            ..lent
        };
        Self {
            inputs,
            _document_base_url: document_base_url,
            _resource_context_urls: resource_context_urls,
            _resource_contexts: resource_contexts,
            _media_feature_values: media_feature_values,
            _media_length_resolution_context: media_length_resolution_context,
            _custom_functions: custom_functions,
            _retained_functions: retained_functions,
            _custom_property_registry: custom_property_registry,
        }
    }
}

/// The `length` bytes at the host's `bytes`, or none.
///
/// # Safety
///
/// `bytes` must be null or name `length` readable bytes.
unsafe fn lent_bytes<'a>(bytes: FfiHostHandle, length: usize) -> &'a [u8] {
    // SAFETY: Guaranteed by the caller.
    unsafe { lent_values(bytes.as_pointer().cast::<u8>(), length) }
}

/// What a style job answers: the reactions of the transaction, owned, which the host keeps until it ends the
/// transaction.
pub(crate) struct StyleJobAnswer(FfiStyleTransactionOutput);

impl RenderJob for StyleJob {
    type Answer = StyleJobAnswer;
    type Permit = StyleJobPermit;
    const IS_STYLE: bool = true;

    fn message(self, document: DocumentId, reply: ReplyTo<'_, StyleJobAnswer>, spent: SpentWait) -> RenderMessage<'_> {
        RenderMessage::Style {
            document,
            job: self,
            reply,
            _spent: spent,
        }
    }
}

impl StyleJob {
    /// Runs the job on `engine`, the engine of the document it was sent for.
    pub(crate) fn run(self, engine: &mut StyleEngine) -> StyleJobAnswer {
        // SAFETY: The sealed inputs name only what they own, and live until the transaction has taken them in.
        StyleJobAnswer(unsafe { take_style_transaction(engine, self.root, self.computation_inputs.inputs) })
    }
}

/// Runs a style job for the transaction under `root`, and answers what the host reads of its reactions, which stays
/// valid until the host ends the transaction.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and the buffers `computation_inputs` names must be
/// live for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_take_style_transaction(
    host: *mut DocumentHost,
    root: u32,
    computation_inputs: FfiDocumentStyleComputationInputs,
) -> FfiStyleTransactionView {
    let Some(root) = StyleNodeID::from_raw(root) else {
        return FfiStyleTransactionView::default();
    };
    assert!(!host.is_null(), "document host is null");
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { &*host };
    let job = StyleJob {
        root,
        // SAFETY: Guaranteed by the caller.
        computation_inputs: unsafe { SealedStyleInputs::seal(computation_inputs) },
    };
    // The first style transaction of a read the host waits for is the read's first job; any other, a rendering
    // update's or a later wave's, is a style update's.
    let answer = match host.take_unstyled_read() {
        Some(read) => force_read(read, host, job),
        None => run_job(StyleJobPermit::of_style_update(), host, job),
    };
    host.keep_style_transaction(answer).0.view()
}

/// Ends the transaction the host took last, and has `allocator` mint the identities its end released again first.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `allocator` its document's allocator.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_end_style_transaction(
    host: *mut DocumentHost,
    allocator: *mut StyleNodeIdAllocator,
) {
    assert!(!host.is_null() && !allocator.is_null());
    // SAFETY: Guaranteed by the caller.
    unsafe { &*host }.end_style_transaction();
    // SAFETY: As above.
    let StyleAnswer::Nodes(released) = (unsafe { ask_engine(host, StyleQuery::EndTransaction) }) else {
        unreachable!("the end of a transaction is answered with the identities it released");
    };
    // SAFETY: As above.
    unsafe { &mut *allocator }.release(&released);
}
