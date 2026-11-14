/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The items the generated content of the pseudo-element boxes a walk plans puts in them, and what they show.
//!
//! The walk makes the rows of the items as it places each box: a text whose characters it knows is spelled at once,
//! one that shows a counter value or the quote depth waits. Each walk records, in tree order, the boxes whose content
//! moves the quote depth or shows it, the boxes it left to jobs, and the boxes that scope quotes to their subtrees.
//! Once the build resolved the counters of every owner, one pass over those records down the jobs hands each box the
//! depth before its content, and every waiting text is spelled. The commit then writes the texts to the rows in bulk,
//! and takes the rows of the items that turned out to make nothing out of the tree.

use super::counter_walk::{CounterPlan, CounterSource};
use crate::css::computed_value_views::QuoteTransfer;
use crate::css::counter_representation::CounterStyle;
use crate::css::css_string::CssString;
use crate::css::style::RetainedState;
use crate::css::style::tree::StyleNodeID;
use crate::layout::counters::{Counter, CounterOwner, CountersSet, LIST_ITEM_COUNTER_NAME};
use crate::layout::generated_content::{
    ContentItem, ContentRecord, CounterValues, marker_counter_text, resolve_content,
};
use crate::layout::layout_node_arena::LayoutNodeArena;
use crate::layout::node_data::pseudo_kind_of;
use libcompositing_rust::node_slot_id::NodeSlotId;
use std::sync::Arc;

/// One step of the quote depth of a walk, in tree order.
#[derive(Clone, Copy, Debug)]
pub(crate) enum QuoteStep {
    /// A pseudo-element box whose content moves the depth by `transfer`, and the content of [`ContentPlan::pending`]
    /// that shows the depth, if any.
    Content {
        pending: Option<u32>,
        transfer: QuoteTransfer,
    },
    /// Root `root` of job `job` plans the subtree that goes here.
    Hole {
        job: u32,
        root: u32,
    },
    /// A box with style containment starts here: what its subtree does to the depth stays in it.
    Save,
    Restore,
    /// A box of an element a child list planned again keeps, with every box below it, whose content moves the depth as
    /// the builds before made it.
    KeptElement(StyleNodeID),
    /// The kept box of a pseudo-element of the element whose child list is planned again.
    KeptPseudoElement(StyleNodeID, u8),
    /// The place of a step the walk had not decided yet when it got there, which turned out to be nothing.
    Nothing,
}

/// Where the values of the counters a pseudo-element's content shows come from.
#[derive(Clone, Copy, Debug)]
pub(crate) enum ContentCounters {
    /// The counters the walk resolved, see [`CounterSource`].
    Walk(CounterSource),
    /// No owner holds a counter: every counter the content shows is instantiated at 0 by the pseudo-element.
    Zero,
    /// The counters of the top layer and of SVG resource content, which the build resolves after the commit, one owner
    /// at a time: the content is spelled then, see `PlanEvent::FillGeneratedContent`.
    AfterCommit,
}

/// What a content waits for to be spelled.
#[derive(Clone, Debug)]
pub(crate) enum PendingKind {
    /// A pseudo-element's `content`, resolved again with the depth and the values (see [`resolve_content`]).
    Content,
    /// The marker string of a list marker whose content is `normal`, which shows the `list-item` counter in the counter
    /// style, or in decimal for none.
    MarkerCounter(Option<Arc<CounterStyle>>),
}

/// A content whose texts wait for counter values or the quote depth.
pub(crate) struct PendingContent {
    pub(crate) owner: CounterOwner,
    pub(crate) kind: PendingKind,
    /// The pseudo-element's box, or the list marker box a list-item pseudo-element nests.
    pub(crate) box_row: NodeSlotId,
    /// The row of each item the content resolved to, in order, or the invalid slot for an item that makes no row.
    pub(crate) rows: Vec<NodeSlotId>,
    /// Whether a text that turns out empty stays: that of an inline box carries its fragment.
    pub(crate) keeps_empty_text: bool,
    /// The flags of `box_row` before the walk put the items in it, which it takes again if every item leaves it.
    pub(crate) flags_before_items: u32,
    pub(crate) counters: ContentCounters,
    pub(crate) tree_scope: u32,
    /// The depth before the content, once known.
    pub(crate) quote_depth: Option<u32>,
}

/// The texts of the items of the generated content a walk planned, and what waits to be spelled.
#[derive(Default)]
pub(crate) struct ContentPlan {
    /// Each row of a generated text the walk made, in the order it made them.
    pub(crate) text_rows: Vec<NodeSlotId>,
    /// The text of each such row that is spelled.
    pub(crate) texts: Vec<(NodeSlotId, Vec<u16>)>,
    /// The rows of the items that turned out to make nothing.
    pub(crate) removed_items: Vec<RemovedItem>,
    /// What each content the walk resolved records, see [`ContentRecord`].
    pub(crate) records: Vec<(CounterOwner, ContentRecord)>,
    pub(crate) pending: Vec<PendingContent>,
    pub(crate) quote_steps: Vec<QuoteStep>,
    /// The sets the contents of the walk instantiated counters in where no owner holds a counter, see
    /// [`ContentCounters::Zero`].
    pub(crate) instantiated_counters: Vec<(CounterOwner, Option<CountersSet>)>,
    /// Whether the walk notes quote steps: some record may open or close a quote.
    pub(crate) notes_quote_steps: bool,
}

impl ContentPlan {
    /// Whether a content of the walk shows the quote depth.
    pub(crate) fn shows_quote_depth(&self) -> bool {
        self.quote_steps
            .iter()
            .any(|step| matches!(step, QuoteStep::Content { pending: Some(_), .. }))
    }

    /// Notes `step`, in the place reserved for it at `slot` if any.
    pub(crate) fn note_quote_step(&mut self, step: QuoteStep, slot: Option<usize>) {
        // OPTIMIZATION: Where no record opens or closes a quote, no content shows the depth.
        if !self.notes_quote_steps {
            return;
        }
        match slot {
            Some(slot) => self.quote_steps[slot] = step,
            None => self.quote_steps.push(step),
        }
    }

    /// Reserves the place of a step the walk decides later, see [`Self::note_quote_step`].
    pub(crate) fn reserve_quote_step(&mut self) -> usize {
        self.note_quote_step(QuoteStep::Nothing, None);
        self.quote_steps.len().saturating_sub(1)
    }
}

/// What the depth before a child list planned again reads of the boxes around it, which the builds before made.
pub(crate) trait RetainedQuotes {
    /// The depth before the first box the walk reached.
    fn entry_depth(&mut self) -> u32;
    /// The depth after the kept box of `element` and every box below it, from `depth` before.
    fn after_kept_element(&mut self, element: StyleNodeID, depth: u32) -> u32;
    /// The depth after the kept box of a pseudo-element, from `depth` before.
    fn after_kept_pseudo_element(&mut self, element: StyleNodeID, generated_for: u8, depth: u32) -> u32;
}

/// The depth before a document: nothing comes before it, and nothing is kept in it.
pub(crate) struct DocumentStart;

impl RetainedQuotes for DocumentStart {
    fn entry_depth(&mut self) -> u32 {
        0
    }

    fn after_kept_element(&mut self, _: StyleNodeID, _: u32) -> u32 {
        unreachable!("a build of the whole document keeps no box")
    }

    fn after_kept_pseudo_element(&mut self, _: StyleNodeID, _: u8, _: u32) -> u32 {
        unreachable!("a build of the whole document keeps no box")
    }
}

/// The values of the counters of an owner the walk placed.
struct WalkCounterValues<'a> {
    counters: &'a CounterPlan,
    source: CounterSource,
}

impl CounterValues for WalkCounterValues<'_> {
    fn innermost(&mut self, name: &[u16]) -> i32 {
        // NB: The walk instantiated every counter the content uses where the owner held none.
        self.counters.values_of(self.source, name).last().copied().unwrap_or(0)
    }

    fn every(&mut self, name: &[u16]) -> Vec<i32> {
        let values = self.counters.values_of(self.source, name);
        if values.is_empty() { vec![0] } else { values }
    }
}

/// The counters a content instantiates where no owner holds one, each at 0.
struct InstantiatedCounterValues {
    names: Vec<Vec<u16>>,
}

impl CounterValues for InstantiatedCounterValues {
    fn innermost(&mut self, name: &[u16]) -> i32 {
        if !self.names.iter().any(|instantiated| instantiated == name) {
            self.names.push(name.to_vec());
        }
        0
    }

    fn every(&mut self, name: &[u16]) -> Vec<i32> {
        vec![self.innermost(name)]
    }
}

/// The row of an item of generated content that turned out to make nothing, which the commit takes out of the tree.
#[derive(Clone, Copy, Debug)]
pub(crate) struct RemovedItem {
    pub(crate) row: NodeSlotId,
    /// The pseudo-element's box, or the list marker box it nests, which takes the flags it had before its items went
    /// in where it holds none any more.
    pub(crate) box_row: NodeSlotId,
    pub(crate) flags_before_items: u32,
}

/// Spells the texts of `pending`, a content of a pseudo-element, with `values` for its counters, as `(row, text)` in
/// `texts`, and lists the rows of the items that make nothing in `removed`. Answers what the content records, if it
/// records anything.
pub(crate) fn fill_pending_content(
    engine: &RetainedState,
    arena: &LayoutNodeArena,
    pending: &PendingContent,
    values: &mut dyn CounterValues,
    texts: &mut Vec<(NodeSlotId, Vec<u16>)>,
    removed: &mut Vec<RemovedItem>,
) -> Option<ContentRecord> {
    let mut record = None;
    let mut item_texts: Vec<Option<Vec<u16>>> = Vec::with_capacity(pending.rows.len());
    let mut makes = Vec::with_capacity(pending.rows.len());
    match &pending.kind {
        PendingKind::MarkerCounter(counter_style) => {
            let value = values.innermost(&LIST_ITEM_COUNTER_NAME);
            item_texts.push(Some(marker_counter_text(
                arena,
                pending.tree_scope,
                counter_style.as_deref(),
                value,
            )));
            makes.push(true);
        }
        PendingKind::Content => {
            let owner = pending.owner;
            let style = engine
                .pseudo_published_style_view(owner.element, pseudo_kind_of(owner.generated_for))
                .expect("a pseudo-element with a box has a style");
            let resolved = resolve_content(
                arena,
                pending.tree_scope,
                style,
                Some(pending.quote_depth.unwrap_or(0)),
                Some(values),
            );
            for item in resolved.items {
                match item {
                    ContentItem::Text { text, made } => {
                        item_texts.push(text);
                        makes.push(made);
                    }
                    ContentItem::Image(_) => {
                        item_texts.push(None);
                        makes.push(true);
                    }
                }
            }
            record = Some(ContentRecord {
                counter_styles: resolved.counter_styles,
                accessible_text: resolved.accessible_text,
            });
        }
    }
    assert_eq!(
        item_texts.len(),
        pending.rows.len(),
        "a content resolves to the items it was planned with"
    );
    for ((&row, text), made) in pending.rows.iter().zip(item_texts).zip(makes) {
        let Some(text) = text else {
            continue;
        };
        if row.is_invalid() {
            continue;
        }
        if !made || (!pending.keeps_empty_text && text.is_empty()) {
            removed.push(RemovedItem {
                row,
                box_row: pending.box_row,
                flags_before_items: pending.flags_before_items,
            });
            continue;
        }
        texts.push((row, text));
    }
    record
}

/// The set a content made where no owner holds a counter, with the counters `names` it instantiated at 0.
fn instantiated_set(owner: CounterOwner, names: Vec<Vec<u16>>) -> Option<(CounterOwner, Option<CountersSet>)> {
    if names.is_empty() {
        return None;
    }
    let counters = names
        .into_iter()
        .map(|name| Counter::new(CssString::from_utf16(&name), owner, false, 0))
        .collect();
    Some((owner, Some(Arc::new(counters))))
}

/// Spells every waiting text of a plan, whose counters are resolved, but those of contents whose counters are resolved
/// after the commit.
pub(crate) fn fill_plan_contents(
    engine: &RetainedState,
    arena: &LayoutNodeArena,
    content: &mut ContentPlan,
    counters: &CounterPlan,
) {
    let ContentPlan {
        texts,
        removed_items,
        records,
        pending,
        instantiated_counters,
        ..
    } = content;
    for pending in pending.iter() {
        let record = match pending.counters {
            ContentCounters::AfterCommit => continue,
            ContentCounters::Walk(source) => fill_pending_content(
                engine,
                arena,
                pending,
                &mut WalkCounterValues { counters, source },
                texts,
                removed_items,
            ),
            ContentCounters::Zero => {
                let mut values = InstantiatedCounterValues { names: Vec::new() };
                let record = fill_pending_content(engine, arena, pending, &mut values, texts, removed_items);
                instantiated_counters.extend(instantiated_set(pending.owner, values.names));
                record
            }
        };
        if let Some(record) = record {
            records.push((pending.owner, record));
        }
    }
}
