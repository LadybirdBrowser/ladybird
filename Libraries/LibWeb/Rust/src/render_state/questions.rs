/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The questions the host asks a document's render state, and the answers it waits for: reads of the layout arena and
//! of the style engine, writes the host pays for, and the rows as of every change the host queued. Each question has
//! the slot its own answer moves into, so the host waits for exactly what it asked.

use super::devtools::{DevToolsAnswer, DevToolsQuery};
use super::{DocumentHost, RenderMessage, RenderWait, ReplyTo, wait_for_render_state};
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

/// A question the host asks about a document's render state, answered as of every change it queued before.
pub(crate) trait Question {
    /// What the render state answers the question with.
    type Answer;

    /// The question as the render state reads it, with `reply`, where its answer goes.
    fn with_reply(self, reply: ReplyTo<'_, Self::Answer>) -> Query<'_>;
}

/// A question the host waits for the answer to, as a render message carries it, with where the answer goes.
pub(crate) enum Query<'a> {
    /// A read of the document's layout arena.
    Arena(ArenaQuery, ReplyTo<'a, ArenaAnswer>),
    /// A read of the document's style engine the host's style code makes.
    Engine(StyleQuery, ReplyTo<'a, StyleAnswer>),
    /// A read for tests and debugging.
    DevTools(DevToolsQuery, ReplyTo<'a, DevToolsAnswer>),
    /// A write to the document's layout tree the host waits for, answered with what it owes the host.
    Write(LayoutWrite, ReplyTo<'a, LayoutWritten>),
    /// The document's rows, which the render state publishes.
    CommittedRows(CommittedRows, ReplyTo<'a, RowSnapshot>),
}

/// The document's rows, which the render state publishes: with every row's scrollable overflow measured first where
/// `measure_overflow`.
pub(crate) struct CommittedRows {
    pub(crate) measure_overflow: bool,
}

macro_rules! question {
    ($question:ty, $answer:ty, $variant:ident) => {
        impl Question for $question {
            type Answer = $answer;

            fn with_reply(self, reply: ReplyTo<'_, $answer>) -> Query<'_> {
                Query::$variant(self, reply)
            }
        }
    };
}

question!(ArenaQuery, ArenaAnswer, Arena);
question!(StyleQuery, StyleAnswer, Engine);
question!(DevToolsQuery, DevToolsAnswer, DevTools);
question!(LayoutWrite, LayoutWritten, Write);
question!(CommittedRows, RowSnapshot, CommittedRows);

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

impl Query<'_> {
    /// Answers the question from `arena` and `engine`, the arena and style engine of the document it was asked about.
    ///
    /// # Safety
    ///
    /// `engine` must name the live style engine `arena` links, which nothing else borrows meanwhile.
    pub(super) unsafe fn answer(self, arena: &mut LayoutNodeArena, engine: StyleEngineHandle) {
        match self {
            Self::Arena(query, reply) => reply.answer(|| query.answer(arena)),
            // SAFETY: Guaranteed by the caller. An engine question reaches the engine only through this borrow.
            Self::Engine(query, reply) => reply.answer(|| query.answer(unsafe { engine.get_mut() })),
            Self::DevTools(query, reply) => reply.answer(|| query.answer(arena)),
            Self::Write(write, reply) => reply.answer(|| write.apply(arena)),
            Self::CommittedRows(CommittedRows { measure_overflow }, reply) => {
                reply.answer(|| arena.publish_row_snapshot(measure_overflow));
            }
        }
    }
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

/// Asks the render state of `host`'s document `question`, spending `wait`, and answers what it answered.
pub(crate) fn ask<Q: Question>(wait: impl RenderWait, host: &DocumentHost, question: Q) -> Q::Answer {
    let document = host.document();
    wait_for_render_state(wait, host, |reply| RenderMessage::Ask {
        document,
        query: question.with_reply(reply),
    })
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
        unsafe { &mut *crate::render_state::arena_for_unconverted_entry(host.document()).cast::<LayoutNodeArena>() }
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
            CommittedRows { measure_overflow: true },
        );
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
        written
            .host_work
            .apply(&crate::stage::MainThread::for_test(), arena_of(host));
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
}
