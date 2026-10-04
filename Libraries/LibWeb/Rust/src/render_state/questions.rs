/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The questions the host asks a document's render state, and their answers: reads of the layout arena and of the
//! style engine, writes the host pays for, and the rows as of every change the host queued. Each question answers a
//! type of its own, so the host gets exactly what it asked.

use super::devtools::{DevToolsAnswer, DevToolsQuery};
use super::{DocumentHost, RenderWait};
use crate::css::style::StyleEngineHandle;
use crate::css::style::engine_calls::{StyleAnswer, StyleQuery};
use crate::css::style::tree::StyleNodeID;
use crate::layout::LayoutNodeArena;
use crate::layout::counters::CounterOwner;
use crate::layout::layout_changes::{LayoutWrite, LayoutWritten};
use crate::layout::node_data::NodeSlotId;
use crate::layout::rendered_text::FfiTextSourceRange;
use crate::layout::row_reads::RowSnapshot;
use crate::layout::text_queries::FfiDomTextRange;
use crate::painting::paint_passes::{PendingPreparation, rendering_preparation_pending};

/// A question the host asks about a document's render state, answered as of every change it queued before.
pub(crate) trait Question {
    /// What the render state answers the question with.
    type Answer;

    /// Whether answering the question leaves the paint and hit testing properties the host prepared from the render
    /// state current: it reads, or writes only what the passes that prepare them do not read until a message of the
    /// host's reaches the render state. Overflow a question measures as it reads is not part of this: the host checks
    /// what each measurement leaves for the preparation as it answers.
    const LEAVES_PAINT_PREPARATION_CURRENT: bool;

    /// Answers the question from `arena` and `engine`, the arena and style engine of the document it was asked about.
    ///
    /// # Safety
    ///
    /// `engine` must name the live style engine `arena` links, which nothing else borrows meanwhile.
    unsafe fn answer(self, arena: &mut LayoutNodeArena, engine: StyleEngineHandle) -> Self::Answer;
}

/// The document's rows, which the render state publishes: with every row's scrollable overflow measured first where
/// `measure_overflow`. It answers none where `held`, the version of the rows the host holds, still reads as the arena.
pub(crate) struct CommittedRows {
    pub(crate) measure_overflow: bool,
    pub(crate) held: Option<crate::layout::RowsVersion>,
}

/// Whether preparing the document for rendering has something to do, answered with the proof that it has.
pub(crate) struct PreparationPending;

/// Implements [`Question`] for `$question`, whose answer is `$answer`, as `$body` answers it from `$arena`, a question
/// that only reads or writes the arena, leaving the paint preparation current where `$leaves_current`.
macro_rules! arena_question {
    ($question:ty, $answer:ty, $leaves_current:expr, |$self:ident, $arena:ident| $body:expr) => {
        impl Question for $question {
            type Answer = $answer;
            const LEAVES_PAINT_PREPARATION_CURRENT: bool = $leaves_current;

            unsafe fn answer($self, $arena: &mut LayoutNodeArena, _: StyleEngineHandle) -> $answer {
                $body
            }
        }
    };
}

arena_question!(ArenaQuery, ArenaAnswer, true, |self, arena| self.answer(arena));
arena_question!(DevToolsQuery, DevToolsAnswer, true, |self, arena| self.answer(arena));
arena_question!(LayoutWrite<'_>, LayoutWritten, false, |self, arena| self.apply(arena));
arena_question!(CommittedRows, Option<RowSnapshot>, true, |self, arena| {
    if self.held.is_some_and(|held| held == arena.rows_version()) {
        return None;
    }
    Some(arena.publish_row_snapshot(self.measure_overflow))
});
arena_question!(PreparationPending, Option<PendingPreparation>, true, |self, arena| {
    rendering_preparation_pending(arena)
});

impl Question for StyleQuery {
    type Answer = StyleAnswer;
    // NB: The passes that prepare the paint properties read no style engine.
    const LEAVES_PAINT_PREPARATION_CURRENT: bool = true;

    unsafe fn answer(self, _: &mut LayoutNodeArena, engine: StyleEngineHandle) -> StyleAnswer {
        // SAFETY: Guaranteed by the caller. An engine question reaches the engine only through this borrow.
        self.answer(unsafe { engine.get_mut() })
    }
}

/// A read of a document's layout arena that `read` answers from the arena and `args`, which the host hands over by
/// value: the read reaches nothing of the host's, so the render owner answers it. It may bring what it reads up to date
/// first, as only the render state can.
pub(crate) struct ArenaRead<A, R> {
    read: fn(&mut LayoutNodeArena, A) -> R,
    args: A,
}

impl<A, R> ArenaRead<A, R> {
    pub(crate) fn new(args: A, read: fn(&mut LayoutNodeArena, A) -> R) -> Self {
        Self { read, args }
    }
}

// SAFETY: The host waits for the answer to the read, so what its arguments lend of the host's stays live and unwritten
// until the render owner has answered, as with [`Lent`].
unsafe impl<A, R> Send for ArenaRead<A, R> {}

impl<A, R> Question for ArenaRead<A, R> {
    type Answer = Waited<R>;
    // NB: What a read brings up to date first, a preparation leaves up to date, or only a layout round reads, except
    //     for overflow it measures, which the host checks as it answers.
    const LEAVES_PAINT_PREPARATION_CURRENT: bool = true;

    unsafe fn answer(self, arena: &mut LayoutNodeArena, _: StyleEngineHandle) -> Waited<R> {
        Waited((self.read)(arena, self.args))
    }
}

/// A call of the host into a document's style engine that the host waits for: `call` reaches the engine, reads the
/// layout arena beside it, and reaches what the host lends it for the call, its arrays and its callbacks among them,
/// which the render owner reaches while the host waits.
pub(crate) struct EngineCall<F>(pub(crate) F);

// SAFETY: The host waits for the answer to the call, so what the call borrows of the host's stays live and unwritten
// until the render owner has answered, as with [`Lent`]. The callbacks the call makes into the host's DOM run on the
// owner while the host waits, as a style transaction's do.
unsafe impl<F> Send for EngineCall<F> {}

impl<R, F: FnOnce(&mut crate::css::style::StyleEngine, &LayoutNodeArena) -> R> Question for EngineCall<F> {
    type Answer = Waited<R>;
    // NB: The passes that prepare the paint properties read no style engine, and the call only reads the arena.
    const LEAVES_PAINT_PREPARATION_CURRENT: bool = true;

    unsafe fn answer(self, arena: &mut LayoutNodeArena, engine: StyleEngineHandle) -> Waited<R> {
        // SAFETY: Guaranteed by the caller. The call reaches the engine only through this borrow.
        Waited((self.0)(unsafe { engine.get_mut() }, arena))
    }
}

/// What an [`ArenaRead`] or an [`EngineCall`] answers, which goes back to the host that waits for it.
pub(crate) struct Waited<R>(pub(crate) R);

// SAFETY: What the answer names of the host's, of the arena's or of the engine's, the host reads once it has it, and
// before its next job of the render state, which is what may write it.
unsafe impl<R> Send for Waited<R> {}

/// A read of a document's layout arena.
pub(crate) enum ArenaQuery {
    /// The text a pseudo-element's generated content resolved to when its box was built: its alt text when it has one,
    /// otherwise every string in it.
    GeneratedContentAccessibleText(CounterOwner),
    /// The characters the generated text row renders.
    GeneratedText(NodeSlotId),
    /// The text the rows of the text node whose primary row is `primary` render, with whitespace collapsed where their
    /// style collapses it if `collapse_whitespace`.
    RenderedText {
        primary: NodeSlotId,
        collapse_whitespace: bool,
    },
    /// The DOM range of the word at `dom_offset` in the text the rows of the text node whose primary row is `primary`
    /// render.
    WordRange { primary: NodeSlotId, dom_offset: usize },
    /// The DOM text nodes whose rows below `viewport` find-in-page would search, where the document has no searchable
    /// text for it.
    SearchCandidates { viewport: NodeSlotId },
    /// Where `query` occurs in the searchable text below `viewport`, which leaves out the DOM text nodes in `excluded`,
    /// sorted, where it is built for the query.
    FindText {
        viewport: NodeSlotId,
        query: Lent<[u16]>,
        case_sensitive: bool,
        excluded: Lent<[StyleNodeID]>,
    },
}

/// What the host lends the render state with a message it waits for the answer to.
pub(crate) struct Lent<T: ?Sized + 'static>(std::ptr::NonNull<T>);

// SAFETY: The host waits for the answer to the message that carries the loan, which keeps what it lends live and
// unwritten until the render state has answered.
unsafe impl<T: ?Sized + Sync> Send for Lent<T> {}

impl<T: ?Sized> Lent<T> {
    pub(crate) fn new(value: &T) -> Self {
        Self(std::ptr::NonNull::from(value))
    }

    /// # Safety
    ///
    /// Only in answering the message that carries the loan, which the host waits for.
    pub(crate) unsafe fn get<'a>(self) -> &'a T {
        // SAFETY: Guaranteed by the caller.
        unsafe { self.0.as_ref() }
    }
}

/// The answer to an [`ArenaQuery`].
#[derive(Debug, PartialEq)]
pub(crate) enum ArenaAnswer {
    Text(Vec<u16>),
    Range(FfiTextSourceRange),
    TextNodes(Vec<StyleNodeID>),
    TextRanges(Vec<FfiDomTextRange>),
}

impl ArenaQuery {
    fn answer(self, arena: &mut LayoutNodeArena) -> ArenaAnswer {
        use crate::layout::text_queries;
        match self {
            Self::GeneratedContentAccessibleText(owner) => {
                ArenaAnswer::Text(arena.generated_content().borrow().accessible_text(owner).to_vec())
            }
            Self::GeneratedText(row) => {
                ArenaAnswer::Text(arena.published_text_source(row, false).data.to_utf16().into_owned())
            }
            Self::RenderedText {
                primary,
                collapse_whitespace,
            } => ArenaAnswer::Text(text_queries::rendered_text(arena, primary, collapse_whitespace)),
            Self::WordRange { primary, dom_offset } => {
                ArenaAnswer::Range(text_queries::text_word_range(arena, primary, dom_offset))
            }
            Self::SearchCandidates { viewport } => {
                ArenaAnswer::TextNodes(text_queries::search_candidates(arena, viewport))
            }
            Self::FindText {
                viewport,
                query,
                case_sensitive,
                excluded,
            } => {
                // SAFETY: The host waits for the answer, keeping what it lent live.
                let (query, excluded) = unsafe { (query.get(), excluded.get()) };
                ArenaAnswer::TextRanges(text_queries::find_matching_text(
                    arena,
                    viewport,
                    query,
                    case_sensitive,
                    excluded,
                ))
            }
        }
    }
}

/// Asks the render state of `host`'s document `question`, spending `wait`, and answers what it answered as of every
/// change the host queued before: the render owner answers it, and the host waits.
pub(crate) fn ask<Q: Question + Send>(wait: impl RenderWait, host: &DocumentHost, question: Q) -> Q::Answer
where
    Q::Answer: Send,
{
    host.ask(wait, question)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::node_data::NodeKind;
    use crate::render_state::ScriptForcedRead;

    fn host() -> (*mut DocumentHost, &'static DocumentHost) {
        let pointer = crate::render_state::document_host::document_host_create(0);
        // SAFETY: The host lives until the test destroys it.
        (pointer, unsafe { &*pointer })
    }

    fn arena_of(host: &DocumentHost) -> &'static mut LayoutNodeArena {
        // SAFETY: The arena lives as long as the host's render state, and the test reaches it only between questions.
        unsafe { &mut *host.arena_for_test() }
    }

    fn destroy(pointer: *mut DocumentHost) {
        // SAFETY: The host is destroyed once, and nothing reaches it after.
        unsafe { crate::render_state::document_host::document_host_destroy(pointer) };
    }

    #[test]
    fn a_question_about_the_rows_answers_them_as_of_the_arena() {
        let (pointer, host) = host();
        let row = arena_of(host).allocate_for_test().slot;
        let rows = ask(
            ScriptForcedRead::for_test(),
            host,
            CommittedRows {
                measure_overflow: true,
                held: None,
            },
        )
        .expect("the host holds no rows");
        assert!(rows.node(row).is_some());
        assert!(rows.overflow_is_measured());
        arena_of(host)
            .free_subtree(row)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        destroy(pointer);
    }

    #[test]
    fn a_write_is_answered_with_what_it_owes_the_host() {
        let (pointer, host) = host();
        let arena = arena_of(host);
        let parent = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        arena.insert_child(parent, child, NodeSlotId::INVALID);
        let written = ask(
            ScriptForcedRead::for_test(),
            host,
            LayoutWrite::DropSubtree { root: parent },
        );
        assert!(!written.was_attached);
        written.host_work.pay(&crate::stage::MainThread::for_test());
        assert_eq!(arena_of(host).live_slot_count(), 0);
        destroy(pointer);
    }

    #[test]
    fn an_arena_question_answers_the_text_a_generated_row_shows() {
        let (pointer, host) = host();
        let arena = arena_of(host);
        let row = arena.allocate_for_test().slot;
        arena.write_shape(row).set_kind(NodeKind::GeneratedTextNode);
        let text: Vec<u16> = "Hello".encode_utf16().collect();
        arena.set_generated_text(row, ak::Utf16String::from(ak::Utf16FlyString::from_utf16(&text)));
        let answer = ask(ScriptForcedRead::for_test(), host, ArenaQuery::GeneratedText(row));
        assert_eq!(answer, ArenaAnswer::Text(text));
        arena_of(host)
            .free_subtree(row)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        destroy(pointer);
    }

    #[test]
    fn a_read_that_first_measures_a_scrollability_flip_leaves_the_paint_preparation_stale() {
        use crate::render_state::document_host::{
            document_host_note_paint_preparation_is_current, document_host_paint_preparation_is_current,
        };
        fn measure(arena: &mut LayoutNodeArena, (): ()) {
            arena.measure_scrollable_overflow();
        }

        let (pointer, host) = host();
        let arena = arena_of(host);
        let viewport = arena.allocate_for_test().slot;
        arena.write_shape(viewport).set_kind(NodeKind::Viewport);
        arena.populate_paintable_row(viewport);
        arena.scrollable_overflow.viewport.set(Some(viewport));
        // The viewport had scrollable overflow before it was laid out again, and a read measures it first since.
        arena
            .committed_side_data_mut(viewport)
            .overflow_relative_to_padding_box
            .has_scrollable_overflow = true;
        arena.note_row_overflow_unmeasured(viewport);

        // SAFETY: The host is live, on this thread.
        unsafe { document_host_note_paint_preparation_is_current(pointer) };
        ask(ScriptForcedRead::for_test(), host, ArenaRead::new((), measure));
        // SAFETY: As above.
        assert!(!unsafe { document_host_paint_preparation_is_current(pointer) });
        assert!(arena_of(host).scrollable_overflow.scrollability_changed.get());

        // A read that measures nothing leaves the preparation as it is.
        // SAFETY: As above.
        unsafe { document_host_note_paint_preparation_is_current(pointer) };
        ask(ScriptForcedRead::for_test(), host, ArenaRead::new((), measure));
        // SAFETY: As above.
        assert!(unsafe { document_host_paint_preparation_is_current(pointer) });

        arena_of(host)
            .free_subtree(viewport)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        destroy(pointer);
    }
}
