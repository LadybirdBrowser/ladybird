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
    FfiDocumentStyleComputationInputs, FfiStyleTransactionOutput, FfiStyleTransactionView, take_style_transaction,
};
use super::engine_calls::{StyleAnswer, StyleQuery, ask_engine};
use super::identities::StyleNodeIdAllocator;
use super::tree::StyleNodeID;
use crate::render_state::{
    DocumentHost, DocumentId, Lent, RenderJob, RenderMessage, ReplyTo, SpentWait, StyleJobPermit, force_read, run_job,
};

/// Takes the pending style transaction under `root`, with the document computation inputs the host gathered, which
/// it lends the job.
pub(crate) struct StyleJob {
    root: StyleNodeID,
    computation_inputs: Lent<FfiDocumentStyleComputationInputs>,
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
    ///
    /// # Safety
    ///
    /// The host must lend the computation inputs, and the buffers they name, until the job is over.
    pub(crate) unsafe fn run(self, engine: &mut StyleEngine) -> StyleJobAnswer {
        // SAFETY: Guaranteed by the caller.
        StyleJobAnswer(unsafe { take_style_transaction(engine, self.root, *self.computation_inputs.get()) })
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
        computation_inputs: Lent::new(&computation_inputs),
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
