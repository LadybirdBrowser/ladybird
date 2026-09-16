/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Assembles a frame from the retained paint-order tree and the pushed damage. Scopes on a
//! damaged path are walked, clean children are copied from the published frame by range, and
//! damaged producers are recorded again under explicit contexts. Nothing is validated on the
//! way: when assembly starts, the damage set is complete by construction.

// The recorder switches over to assembly in a later change; until then only its tests use it.
#![allow(dead_code)]

use super::order_tree::{ChildEntry, ChildKey, OutputSize, PaintOrderTree, ProducerKind, ScopeId};
use crate::layout::node_data::NodeSlotId;
use crate::painting::paint_order_plan::{PaintScope, PaintScopeKind};
use crate::painting::record::damage::PaintDamage;
use smallvec::SmallVec;
use std::ops::Range;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ScopePlanItem {
    Producer(ProducerKind),
    Scope(PaintScope),
}

/// The items of one scope in paint order. Every producer belongs to the scope's owner. An
/// inactive plan is a stacking context that does not paint; its scope records nothing.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct ScopePlan {
    pub active: bool,
    pub items: SmallVec<[ScopePlanItem; 16]>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ProducerOutcome {
    // Recorded again every frame; the entry and every scope above it stay live.
    pub live: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ScopeAction {
    // Walked with the published child list.
    Assembled,
    // The plan was rebuilt and matched against the published children.
    Replanned,
    // Recorded without a published counterpart.
    Recorded,
    // The stacking context does not paint.
    Inactive,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum AssemblyEvent {
    ScopeBegin(ScopeId, ScopeAction),
    ScopeEnd(ScopeId),
    ScopeCopied(ScopeId),
    ProducerRecorded(NodeSlotId, ProducerKind, OutputSize),
    ProducerCopied(NodeSlotId, ProducerKind),
}

/// The end of the output being assembled: command bytes, hit-test items and blocking wheel
/// regions so far.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct OutputPosition {
    pub bytes: u32,
    pub hits: u32,
    pub blocking_wheel_event_regions: u32,
}

/// What assembly needs from the recorder, which owns the output and the published frame.
pub(crate) trait AssemblyHost {
    fn plan_scope(&mut self, scope: PaintScope) -> ScopePlan;

    /// Records one producer of `owner` under the contexts it needs.
    fn record_producer(&mut self, owner: NodeSlotId, kind: ProducerKind) -> ProducerOutcome;

    fn output_position(&self) -> OutputPosition;

    /// Appends the published frame's command bytes and hit-test items in the given ranges.
    fn copy_published(&mut self, bytes: Range<u32>, hits: Range<u32>, blocking_wheel_event_regions: u32);

    /// Every damaged row, including the rows of subtrees whose root moved.
    fn damaged_rows(&mut self) -> Vec<NodeSlotId>;

    /// The damage of a row with a moved ancestor's subtree expansion folded in.
    fn effective_damage(&mut self, row: NodeSlotId) -> PaintDamage;

    fn producer_reads_descendants(&mut self, owner: NodeSlotId, kind: ProducerKind) -> bool;

    fn observe(&mut self, _event: AssemblyEvent) {}
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct SourceCursor {
    bytes: u32,
    hits: u32,
}

impl SourceCursor {
    fn advance(&mut self, output: OutputSize) {
        self.bytes += output.bytes;
        self.hits += output.hits;
    }
}

// Consecutive clean children are copied with one range append each for commands and items.
#[derive(Clone, Copy)]
struct PendingCopy {
    start: SourceCursor,
    end: SourceCursor,
    blocking_wheel_event_regions: u32,
}

/// Whether the published frame can be returned as it is: nothing was pushed, nothing in it is
/// recorded every frame, and there is a published tree at all.
pub(crate) fn frame_is_unchanged(tree: &PaintOrderTree, has_damage: bool) -> bool {
    !tree.root().is_none() && !has_damage && !tree.root_entry().is_live()
}

pub(crate) struct Assembler<'a, H: AssemblyHost> {
    host: &'a mut H,
    tree: &'a mut PaintOrderTree,
    // The byte offset of the published root scope, when a published frame can be copied from.
    source_prologue_bytes: Option<u32>,
    pending_copy: Option<PendingCopy>,
}

impl<'a, H: AssemblyHost> Assembler<'a, H> {
    pub(crate) fn new(host: &'a mut H, tree: &'a mut PaintOrderTree, source_prologue_bytes: Option<u32>) -> Self {
        Self {
            host,
            tree,
            source_prologue_bytes,
            pending_copy: None,
        }
    }

    /// Records the frame's root scope after the prologue and returns its entry. The tree holds
    /// the assembled structure as pending changes for the caller to publish or discard.
    pub(crate) fn assemble_root(mut self, root_scope: PaintScope) -> ChildEntry {
        self.tree.begin_recording();
        for row in self.host.damaged_rows() {
            for scope in self.tree.scopes_owned_by(row) {
                self.tree.mark_path_to_root_damaged(scope);
            }
        }
        let published_root = self.tree.root();
        let entry = match self.source_prologue_bytes {
            Some(prologue_bytes) if !published_root.is_none() && self.tree.scope_of(published_root) == root_scope => {
                let cursor = SourceCursor {
                    bytes: prologue_bytes,
                    hits: 0,
                };
                let entry = self.assemble_scope(published_root, cursor);
                self.flush_pending_copy();
                self.tree.finish_recording(published_root, entry);
                entry
            }
            _ => {
                if !published_root.is_none() {
                    self.tree.detach_scope(published_root);
                }
                let root = self.tree.allocate_scope(root_scope, ScopeId::NONE);
                let entry = self.record_scope_fresh(root);
                self.tree.finish_recording(root, entry);
                entry
            }
        };
        debug_assert!(self.pending_copy.is_none());
        entry
    }

    fn assemble_scope(&mut self, id: ScopeId, cursor: SourceCursor) -> ChildEntry {
        let scope = self.tree.scope_of(id);
        let damage = self.host.effective_damage(scope.owner);
        let order_damage = match scope.kind {
            PaintScopeKind::PaintedAsStackingContext => {
                PaintDamage::ORDER | PaintDamage::CONTEXT_ORDER | PaintDamage::ELIGIBILITY
            }
            PaintScopeKind::Descendants(_) => PaintDamage::ORDER,
        };
        if damage.intersects(order_damage) {
            return self.rebuild_scope(id, cursor, damage);
        }
        self.host.observe(AssemblyEvent::ScopeBegin(id, ScopeAction::Assembled));
        let published: SmallVec<[ChildEntry; 16]> = self.tree.children(id).iter().copied().collect();
        let mut entries: SmallVec<[ChildEntry; 16]> = SmallVec::with_capacity(published.len());
        let mut total = OutputSize::default();
        let mut child_cursor = cursor;
        for entry in published {
            let published_output = entry.output();
            let assembled = match entry.key() {
                ChildKey::Scope(child) => {
                    if self.tree.is_on_damaged_path(child) || entry.is_live() {
                        self.assemble_scope(child, child_cursor)
                    } else {
                        self.copy(child_cursor, published_output);
                        self.host.observe(AssemblyEvent::ScopeCopied(child));
                        entry
                    }
                }
                ChildKey::Producer(kind) => {
                    if self.producer_is_damaged(scope.owner, kind, damage, &entry) {
                        self.record_producer(scope.owner, kind)
                    } else {
                        self.copy(child_cursor, published_output);
                        self.host.observe(AssemblyEvent::ProducerCopied(scope.owner, kind));
                        entry
                    }
                }
            };
            child_cursor.advance(published_output);
            total.add(assembled.output());
            entries.push(assembled);
        }
        self.tree.set_children(id, &entries);
        self.host.observe(AssemblyEvent::ScopeEnd(id));
        ChildEntry::scope(id, total)
    }

    fn producer_is_damaged(
        &mut self,
        owner: NodeSlotId,
        kind: ProducerKind,
        damage: PaintDamage,
        entry: &ChildEntry,
    ) -> bool {
        damage.contains(kind.damage())
            || entry.is_live()
            || (damage.contains(PaintDamage::DESCENDANT_READERS) && self.host.producer_reads_descendants(owner, kind))
    }

    // The plan is rebuilt; published children it still lists are copied or walked from their
    // old positions, the rest are recorded, and published children it no longer lists retire.
    fn rebuild_scope(&mut self, id: ScopeId, cursor: SourceCursor, damage: PaintDamage) -> ChildEntry {
        let scope = self.tree.scope_of(id);
        let plan = self.host.plan_scope(scope);
        let published: SmallVec<[ChildEntry; 16]> = self.tree.children(id).iter().copied().collect();
        let mut published_cursors: SmallVec<[SourceCursor; 16]> = SmallVec::with_capacity(published.len());
        let mut child_cursor = cursor;
        for entry in &published {
            published_cursors.push(child_cursor);
            child_cursor.advance(entry.output());
        }
        let mut matched: SmallVec<[bool; 16]> = SmallVec::from_elem(false, published.len());
        if !plan.active {
            self.host.observe(AssemblyEvent::ScopeBegin(id, ScopeAction::Inactive));
            self.detach_unmatched(&published, &matched);
            self.tree.set_children(id, &[]);
            self.host.observe(AssemblyEvent::ScopeEnd(id));
            return ChildEntry::scope(id, OutputSize::default());
        }
        self.host.observe(AssemblyEvent::ScopeBegin(id, ScopeAction::Replanned));
        let mut entries: SmallVec<[ChildEntry; 16]> = SmallVec::with_capacity(plan.items.len());
        let mut total = OutputSize::default();
        for item in plan.items {
            let assembled = match item {
                ScopePlanItem::Producer(kind) => {
                    let found = published
                        .iter()
                        .position(|entry| entry.key() == ChildKey::Producer(kind));
                    match found {
                        Some(index) if !self.producer_is_damaged(scope.owner, kind, damage, &published[index]) => {
                            matched[index] = true;
                            self.copy(published_cursors[index], published[index].output());
                            self.host.observe(AssemblyEvent::ProducerCopied(scope.owner, kind));
                            published[index]
                        }
                        Some(index) => {
                            matched[index] = true;
                            self.record_producer(scope.owner, kind)
                        }
                        None => self.record_producer(scope.owner, kind),
                    }
                }
                ScopePlanItem::Scope(child_scope) => {
                    let found = self.tree.published_scope(child_scope, id).and_then(|child| {
                        published
                            .iter()
                            .position(|entry| entry.key() == ChildKey::Scope(child))
                            .map(|index| (child, index))
                    });
                    match found {
                        Some((child, index)) => {
                            matched[index] = true;
                            if self.tree.is_on_damaged_path(child) || published[index].is_live() {
                                self.assemble_scope(child, published_cursors[index])
                            } else {
                                self.copy(published_cursors[index], published[index].output());
                                self.host.observe(AssemblyEvent::ScopeCopied(child));
                                published[index]
                            }
                        }
                        None => {
                            let child = self.tree.allocate_scope(child_scope, id);
                            self.record_scope_fresh(child)
                        }
                    }
                }
            };
            total.add(assembled.output());
            entries.push(assembled);
        }
        self.detach_unmatched(&published, &matched);
        self.tree.set_children(id, &entries);
        self.host.observe(AssemblyEvent::ScopeEnd(id));
        ChildEntry::scope(id, total)
    }

    fn detach_unmatched(&mut self, published: &[ChildEntry], matched: &[bool]) {
        for (entry, matched) in published.iter().zip(matched) {
            if let (ChildKey::Scope(child), false) = (entry.key(), *matched) {
                self.tree.detach_scope(child);
            }
        }
    }

    fn record_scope_fresh(&mut self, id: ScopeId) -> ChildEntry {
        let scope = self.tree.scope_of(id);
        let plan = self.host.plan_scope(scope);
        if !plan.active {
            self.host.observe(AssemblyEvent::ScopeBegin(id, ScopeAction::Inactive));
            self.tree.set_children(id, &[]);
            self.host.observe(AssemblyEvent::ScopeEnd(id));
            return ChildEntry::scope(id, OutputSize::default());
        }
        self.host.observe(AssemblyEvent::ScopeBegin(id, ScopeAction::Recorded));
        let mut entries: SmallVec<[ChildEntry; 16]> = SmallVec::with_capacity(plan.items.len());
        let mut total = OutputSize::default();
        for item in plan.items {
            let recorded = match item {
                ScopePlanItem::Producer(kind) => self.record_producer(scope.owner, kind),
                ScopePlanItem::Scope(child_scope) => {
                    let child = self.tree.allocate_scope(child_scope, id);
                    self.record_scope_fresh(child)
                }
            };
            total.add(recorded.output());
            entries.push(recorded);
        }
        self.tree.set_children(id, &entries);
        self.host.observe(AssemblyEvent::ScopeEnd(id));
        ChildEntry::scope(id, total)
    }

    fn record_producer(&mut self, owner: NodeSlotId, kind: ProducerKind) -> ChildEntry {
        self.flush_pending_copy();
        let before = self.host.output_position();
        let outcome = self.host.record_producer(owner, kind);
        let after = self.host.output_position();
        let output = OutputSize {
            bytes: after.bytes - before.bytes,
            hits: after.hits - before.hits,
            blocking_wheel_event_regions: after.blocking_wheel_event_regions - before.blocking_wheel_event_regions,
            live: outcome.live,
        };
        self.host.observe(AssemblyEvent::ProducerRecorded(owner, kind, output));
        ChildEntry::producer(kind, output)
    }

    fn copy(&mut self, cursor: SourceCursor, output: OutputSize) {
        debug_assert!(
            self.source_prologue_bytes.is_some(),
            "clean output is copied from a published frame"
        );
        let mut end = cursor;
        end.advance(output);
        match &mut self.pending_copy {
            Some(pending) if pending.end == cursor => {
                pending.end = end;
                pending.blocking_wheel_event_regions += output.blocking_wheel_event_regions;
            }
            _ => {
                self.flush_pending_copy();
                self.pending_copy = Some(PendingCopy {
                    start: cursor,
                    end,
                    blocking_wheel_event_regions: output.blocking_wheel_event_regions,
                });
            }
        }
    }

    fn flush_pending_copy(&mut self) {
        let Some(pending) = self.pending_copy.take() else {
            return;
        };
        self.host.copy_published(
            pending.start.bytes..pending.end.bytes,
            pending.start.hits..pending.end.hits,
            pending.blocking_wheel_event_regions,
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::css_pixels::CssPixelRect;
    use crate::painting::border_radii::BorderRadii;
    use crate::painting::display_list::builder::{CommandRange, RecordedDisplayList};
    use crate::painting::display_list::commands::ContextRef;
    use crate::painting::display_list::recorder::DisplayListRecorder;
    use crate::painting::force_dark::ForceDarkRole;
    use crate::painting::hit_test::{HitTestItem, HitTestItemKind, HitTestList};
    use crate::painting::paint_order_plan::StackingContextPaintPhase;
    use libgfx_rust::{Color, IntRect};
    use std::collections::{HashMap, HashSet};

    fn row(index: u32) -> NodeSlotId {
        NodeSlotId::new(index, 1)
    }

    fn context(owner: NodeSlotId) -> PaintScope {
        PaintScope::stacking_context(owner)
    }

    fn foreground(owner: NodeSlotId) -> PaintScope {
        PaintScope {
            owner,
            kind: PaintScopeKind::Descendants(StackingContextPaintPhase::Foreground),
        }
    }

    #[derive(Clone, Copy)]
    struct Content {
        rects: u32,
        hits: u32,
        version: u8,
        live: bool,
        blocking: u32,
    }

    fn content(rects: u32, hits: u32) -> Content {
        Content {
            rects,
            hits,
            version: 1,
            live: false,
            blocking: 0,
        }
    }

    struct Frame {
        display_list: RecordedDisplayList,
        hit_test_items: Vec<HitTestItem>,
        prologue_bytes: u32,
        root_entry: ChildEntry,
    }

    struct Output {
        recorder: DisplayListRecorder,
        hit_test_list: HitTestList,
        blocking_wheel_event_region_count: u32,
    }

    fn new_output() -> Output {
        Output {
            recorder: DisplayListRecorder::new(None),
            hit_test_list: HitTestList::default(),
            blocking_wheel_event_region_count: 0,
        }
    }

    struct TestHost {
        plans: HashMap<PaintScope, ScopePlan>,
        contents: HashMap<(NodeSlotId, ProducerKind), Content>,
        damage: HashMap<NodeSlotId, PaintDamage>,
        moved_subtrees: HashSet<NodeSlotId>,
        descendant_readers: HashSet<(NodeSlotId, ProducerKind)>,
        recorded: Vec<(NodeSlotId, ProducerKind)>,
        events: Vec<AssemblyEvent>,
        output: Output,
        source: Option<Frame>,
    }

    impl Default for TestHost {
        fn default() -> Self {
            Self {
                plans: HashMap::new(),
                contents: HashMap::new(),
                damage: HashMap::new(),
                moved_subtrees: HashSet::new(),
                descendant_readers: HashSet::new(),
                recorded: Vec::new(),
                events: Vec::new(),
                output: new_output(),
                source: None,
            }
        }
    }

    impl TestHost {
        fn plan(&mut self, scope: PaintScope, active: bool, items: &[ScopePlanItem]) {
            self.plans.insert(
                scope,
                ScopePlan {
                    active,
                    items: items.iter().copied().collect(),
                },
            );
        }

        fn produce(&mut self, owner: NodeSlotId, kind: ProducerKind, content: Content) {
            self.contents.insert((owner, kind), content);
        }

        fn push(&mut self, owner: NodeSlotId, damage: PaintDamage) {
            *self.damage.entry(owner).or_default() |= damage;
        }

        fn clear_damage(&mut self) {
            self.damage.clear();
            self.moved_subtrees.clear();
            self.recorded.clear();
            self.events.clear();
        }

        fn recorded_kinds_of(&self, owner: NodeSlotId) -> Vec<ProducerKind> {
            self.recorded
                .iter()
                .filter(|(recorded_owner, _)| *recorded_owner == owner)
                .map(|(_, kind)| *kind)
                .collect()
        }
    }

    impl AssemblyHost for TestHost {
        fn plan_scope(&mut self, scope: PaintScope) -> ScopePlan {
            self.plans.get(&scope).cloned().unwrap_or_default()
        }

        fn record_producer(&mut self, owner: NodeSlotId, kind: ProducerKind) -> ProducerOutcome {
            self.recorded.push((owner, kind));
            let Some(content) = self.contents.get(&(owner, kind)).copied() else {
                return ProducerOutcome { live: false };
            };
            let output = &mut self.output;
            for index in 0..content.rects {
                output.recorder.fill_rect(
                    IntRect {
                        x: owner.slot_index() as i32,
                        y: kind as i32 * 100 + index as i32,
                        width: 10,
                        height: i32::from(content.version),
                    },
                    Color::from_rgb(content.version, kind as u8, 7),
                    ForceDarkRole::Background,
                );
            }
            for index in 0..content.hits {
                output.hit_test_list.append(HitTestItem {
                    kind: HitTestItemKind::Box,
                    paintable: owner,
                    hit_node: owner,
                    chrome_widget_kind: 0,
                    text_fragment_index: None,
                    caret_node: NodeSlotId::INVALID,
                    caret_offset: 0,
                    rect: CssPixelRect::default(),
                    caret_rect: CssPixelRect::default(),
                    caret_line_index: Some(index as usize + content.version as usize * 100),
                    block_container: NodeSlotId::INVALID,
                    context: ContextRef::default(),
                    border_radii: BorderRadii::default(),
                    path: None,
                    winding_rule: 0,
                    writing_mode: 0,
                    inline_axis_is_reverse: false,
                    block_axis_is_reverse: false,
                    containing_block: NodeSlotId::INVALID,
                    can_produce_caret_position: false,
                });
            }
            output.blocking_wheel_event_region_count += content.blocking;
            ProducerOutcome { live: content.live }
        }

        fn output_position(&self) -> OutputPosition {
            OutputPosition {
                bytes: self.output.recorder.byte_size() as u32,
                hits: self.output.hit_test_list.items.len() as u32,
                blocking_wheel_event_regions: self.output.blocking_wheel_event_region_count,
            }
        }

        fn copy_published(&mut self, bytes: Range<u32>, hits: Range<u32>, blocking_wheel_event_regions: u32) {
            let source = self.source.as_ref().expect("a published frame to copy from");
            if !bytes.is_empty() {
                self.output.recorder.append_cached_command_range_verbatim(
                    &source.display_list,
                    CommandRange {
                        offset: bytes.start,
                        size: bytes.end - bytes.start,
                    },
                );
            }
            if !hits.is_empty() {
                self.output
                    .hit_test_list
                    .append_copies_of(&source.hit_test_items[hits.start as usize..hits.end as usize]);
            }
            self.output.blocking_wheel_event_region_count += blocking_wheel_event_regions;
        }

        fn damaged_rows(&mut self) -> Vec<NodeSlotId> {
            let mut rows: Vec<NodeSlotId> = self.damage.keys().copied().collect();
            rows.extend(self.moved_subtrees.iter().copied());
            rows.sort_unstable_by_key(|row| row.slot_index());
            rows.dedup();
            rows
        }

        fn effective_damage(&mut self, row: NodeSlotId) -> PaintDamage {
            let mut damage = self.damage.get(&row).copied().unwrap_or_default();
            if self.moved_subtrees.contains(&row) {
                damage |= PaintDamage::ALL_PRODUCERS | PaintDamage::MOVED;
            }
            damage
        }

        fn producer_reads_descendants(&mut self, owner: NodeSlotId, kind: ProducerKind) -> bool {
            self.descendant_readers.contains(&(owner, kind))
        }

        fn observe(&mut self, event: AssemblyEvent) {
            self.events.push(event);
        }
    }

    // Records the canvas prologue and assembles the viewport context from the host's source
    // frame.
    fn record_frame(host: &mut TestHost, tree: &mut PaintOrderTree) -> Frame {
        host.output = new_output();
        host.output.recorder.fill_rect(
            IntRect {
                x: 0,
                y: 0,
                width: 800,
                height: 600,
            },
            Color::from_rgb(255, 255, 255),
            ForceDarkRole::Background,
        );
        let prologue_bytes = host.output.recorder.byte_size() as u32;
        let source_prologue_bytes = host.source.as_ref().map(|frame| frame.prologue_bytes);
        let root_entry = Assembler::new(host, tree, source_prologue_bytes).assemble_root(context(row(0)));
        let output = std::mem::replace(&mut host.output, new_output());
        let hit_test_items = std::rc::Rc::try_unwrap(output.hit_test_list.items).expect("items are unshared");
        Frame {
            display_list: output.recorder.into_builder().finish(),
            hit_test_items,
            prologue_bytes,
            root_entry,
        }
    }

    // Assembles the next frame from the source frame and keeps it as the source in turn.
    fn record_next_frame(host: &mut TestHost, tree: &mut PaintOrderTree) -> Frame {
        let frame = record_frame(host, tree);
        let previous = host.source.replace(Frame {
            display_list: frame.display_list.clone(),
            hit_test_items: frame.hit_test_items.clone(),
            prologue_bytes: frame.prologue_bytes,
            root_entry: frame.root_entry,
        });
        assert!(previous.is_some(), "a next frame copies from a first frame");
        frame
    }

    fn assert_matches_from_scratch(host: &mut TestHost, assembled: &Frame) {
        let recorded = std::mem::take(&mut host.recorded);
        let events = std::mem::take(&mut host.events);
        let source = host.source.take();
        let mut fresh_tree = PaintOrderTree::default();
        let fresh = record_frame(host, &mut fresh_tree);
        host.source = source;
        host.recorded = recorded;
        host.events = events;
        assert_eq!(
            assembled.display_list.bytes, fresh.display_list.bytes,
            "command bytes differ"
        );
        assert_eq!(assembled.display_list.command_runs, fresh.display_list.command_runs);
        // Comparing whole items would compare their paths through the host's path equality.
        let item_identities = |items: &[HitTestItem]| -> Vec<(NodeSlotId, Option<usize>)> {
            items
                .iter()
                .map(|item| (item.paintable, item.caret_line_index))
                .collect()
        };
        assert_eq!(
            item_identities(&assembled.hit_test_items),
            item_identities(&fresh.hit_test_items)
        );
        assert_eq!(assembled.root_entry.output(), fresh.root_entry.output());
    }

    // A viewport context (row 0) with a background and a foreground scope listing three
    // card scopes (rows 1..=3) that each draw a background and a foreground with hit items.
    fn cards_host() -> TestHost {
        let mut host = TestHost::default();
        host.plan(
            context(row(0)),
            true,
            &[
                ScopePlanItem::Producer(ProducerKind::DrawBackground),
                ScopePlanItem::Scope(foreground(row(0))),
            ],
        );
        host.produce(row(0), ProducerKind::DrawBackground, content(1, 0));
        host.plan(
            foreground(row(0)),
            true,
            &[
                ScopePlanItem::Scope(foreground(row(1))),
                ScopePlanItem::Scope(foreground(row(2))),
                ScopePlanItem::Scope(foreground(row(3))),
            ],
        );
        for card in 1..=3 {
            host.plan(
                foreground(row(card)),
                true,
                &[
                    ScopePlanItem::Producer(ProducerKind::DrawBackground),
                    ScopePlanItem::Producer(ProducerKind::HitForeground),
                    ScopePlanItem::Producer(ProducerKind::DrawForeground),
                ],
            );
            host.produce(row(card), ProducerKind::DrawBackground, content(1, 0));
            host.produce(row(card), ProducerKind::HitForeground, content(0, 2));
            host.produce(row(card), ProducerKind::DrawForeground, content(card, 0));
        }
        host
    }

    #[test]
    fn a_quiet_frame_is_unchanged_and_a_fresh_frame_records_everything() {
        let mut host = cards_host();
        let mut tree = PaintOrderTree::default();
        assert!(!frame_is_unchanged(&tree, false));

        let frame = record_frame(&mut host, &mut tree);

        assert_eq!(host.recorded.len(), 1 + 3 * 3);
        assert_eq!(frame.root_entry.output().hits, 6);
        assert_eq!(frame.hit_test_items.len(), 6);
        assert_eq!(
            frame.root_entry.output().bytes as usize,
            frame.display_list.bytes.len() - frame.prologue_bytes as usize
        );
        assert!(frame_is_unchanged(&tree, false));
        assert!(!frame_is_unchanged(&tree, true));
    }

    #[test]
    fn one_damaged_producer_records_alone_and_the_rest_is_copied_in_two_runs() {
        let mut host = cards_host();
        let mut tree = PaintOrderTree::default();
        let first = record_frame(&mut host, &mut tree);
        host.source = Some(first);
        host.clear_damage();

        host.produce(row(2), ProducerKind::DrawForeground, content(2, 0));
        host.contents
            .get_mut(&(row(2), ProducerKind::DrawForeground))
            .unwrap()
            .version = 2;
        host.push(row(2), PaintDamage::DRAW_FOREGROUND);
        host.push(row(0), PaintDamage::DESCENDANT_READERS);
        let second = record_next_frame(&mut host, &mut tree);

        assert_eq!(host.recorded, vec![(row(2), ProducerKind::DrawForeground)]);
        let copied_scopes: Vec<AssemblyEvent> = host
            .events
            .iter()
            .copied()
            .filter(|event| matches!(event, AssemblyEvent::ScopeCopied(_)))
            .collect();
        assert_eq!(copied_scopes.len(), 2);
        assert_matches_from_scratch(&mut host, &second);
    }

    #[test]
    fn a_growing_child_shifts_its_later_siblings_without_recording_them() {
        let mut host = cards_host();
        let mut tree = PaintOrderTree::default();
        let first = record_frame(&mut host, &mut tree);
        host.source = Some(first);
        host.clear_damage();

        host.produce(row(1), ProducerKind::DrawForeground, content(5, 0));
        host.produce(row(1), ProducerKind::HitForeground, content(0, 4));
        host.push(row(1), PaintDamage::DRAW_FOREGROUND | PaintDamage::HIT_FOREGROUND);
        host.push(row(0), PaintDamage::DESCENDANT_READERS);
        let grown = record_next_frame(&mut host, &mut tree);
        assert_eq!(
            host.recorded_kinds_of(row(1)),
            vec![ProducerKind::HitForeground, ProducerKind::DrawForeground]
        );
        assert_eq!(host.recorded.len(), 2);
        assert_eq!(grown.hit_test_items.len(), 8);
        assert_matches_from_scratch(&mut host, &grown);
        host.clear_damage();

        host.produce(row(1), ProducerKind::DrawForeground, content(0, 0));
        host.push(row(1), PaintDamage::DRAW_FOREGROUND);
        host.push(row(0), PaintDamage::DESCENDANT_READERS);
        let shrunk = record_next_frame(&mut host, &mut tree);
        assert_eq!(host.recorded, vec![(row(1), ProducerKind::DrawForeground)]);
        assert_matches_from_scratch(&mut host, &shrunk);
    }

    #[test]
    fn a_replanned_parent_copies_kept_children_and_retires_the_removed_one() {
        let mut host = cards_host();
        let mut tree = PaintOrderTree::default();
        let first = record_frame(&mut host, &mut tree);
        host.source = Some(first);
        let removed = tree
            .published_scope(
                foreground(row(2)),
                tree.published_scope(foreground(row(0)), tree.root()).unwrap(),
            )
            .unwrap();
        host.clear_damage();

        host.plan(
            foreground(row(0)),
            true,
            &[
                ScopePlanItem::Scope(foreground(row(3))),
                ScopePlanItem::Scope(foreground(row(1))),
            ],
        );
        host.push(row(0), PaintDamage::ORDER | PaintDamage::DESCENDANT_READERS);
        let second = record_next_frame(&mut host, &mut tree);

        assert!(host.recorded.is_empty());
        assert_matches_from_scratch(&mut host, &second);
        assert_eq!(second.hit_test_items.len(), 4);
        let parent = tree.published_scope(foreground(row(0)), tree.root()).unwrap();
        assert_eq!(tree.children(parent).len(), 2);
        assert_eq!(tree.published_scope(foreground(row(2)), parent), None);
        assert_eq!(tree.scope_count(), 4);
        assert_ne!(tree.published_scope(foreground(row(3)), parent), Some(removed));
    }

    #[test]
    fn a_new_child_is_recorded_under_the_replanned_parent() {
        let mut host = cards_host();
        let mut tree = PaintOrderTree::default();
        let first = record_frame(&mut host, &mut tree);
        host.source = Some(first);
        host.clear_damage();

        host.plan(
            foreground(row(0)),
            true,
            &[
                ScopePlanItem::Scope(foreground(row(1))),
                ScopePlanItem::Scope(foreground(row(4))),
                ScopePlanItem::Scope(foreground(row(2))),
                ScopePlanItem::Scope(foreground(row(3))),
            ],
        );
        host.plan(
            foreground(row(4)),
            true,
            &[
                ScopePlanItem::Producer(ProducerKind::DrawBackground),
                ScopePlanItem::Producer(ProducerKind::HitForeground),
            ],
        );
        host.produce(row(4), ProducerKind::DrawBackground, content(2, 0));
        host.produce(row(4), ProducerKind::HitForeground, content(0, 1));
        host.push(row(4), PaintDamage::ORDER | PaintDamage::ALL_PRODUCERS);
        host.push(row(0), PaintDamage::ORDER | PaintDamage::DESCENDANT_READERS);
        let second = record_next_frame(&mut host, &mut tree);

        assert_eq!(
            host.recorded,
            vec![
                (row(4), ProducerKind::DrawBackground),
                (row(4), ProducerKind::HitForeground)
            ]
        );
        assert_matches_from_scratch(&mut host, &second);
        assert_eq!(tree.scope_count(), 6);
    }

    #[test]
    fn nested_damage_walks_only_the_damaged_path() {
        let mut host = cards_host();
        // Card 3 gets a nested scope of its own.
        host.plan(
            foreground(row(3)),
            true,
            &[
                ScopePlanItem::Producer(ProducerKind::DrawBackground),
                ScopePlanItem::Scope(foreground(row(5))),
                ScopePlanItem::Producer(ProducerKind::DrawForeground),
            ],
        );
        host.plan(
            foreground(row(5)),
            true,
            &[ScopePlanItem::Producer(ProducerKind::DrawForeground)],
        );
        host.produce(row(5), ProducerKind::DrawForeground, content(3, 0));
        let mut tree = PaintOrderTree::default();
        let first = record_frame(&mut host, &mut tree);
        host.source = Some(first);
        host.clear_damage();

        host.contents
            .get_mut(&(row(5), ProducerKind::DrawForeground))
            .unwrap()
            .version = 9;
        host.push(row(5), PaintDamage::DRAW_FOREGROUND);
        for ancestor in [row(3), row(0)] {
            host.push(ancestor, PaintDamage::DESCENDANT_READERS);
        }
        let second = record_next_frame(&mut host, &mut tree);

        assert_eq!(host.recorded, vec![(row(5), ProducerKind::DrawForeground)]);
        let assembled: Vec<ScopeId> = host
            .events
            .iter()
            .filter_map(|event| match event {
                AssemblyEvent::ScopeBegin(id, ScopeAction::Assembled) => Some(*id),
                _ => None,
            })
            .collect();
        assert_eq!(
            assembled.len(),
            4,
            "viewport, its foreground, card 3 and its nested scope"
        );
        assert_matches_from_scratch(&mut host, &second);
    }

    #[test]
    fn a_moved_subtree_records_every_producer_inside_it() {
        let mut host = cards_host();
        host.plan(
            foreground(row(3)),
            true,
            &[
                ScopePlanItem::Producer(ProducerKind::DrawBackground),
                ScopePlanItem::Scope(foreground(row(5))),
            ],
        );
        host.plan(
            foreground(row(5)),
            true,
            &[ScopePlanItem::Producer(ProducerKind::DrawForeground)],
        );
        host.produce(row(5), ProducerKind::DrawForeground, content(2, 0));
        let mut tree = PaintOrderTree::default();
        let first = record_frame(&mut host, &mut tree);
        host.source = Some(first);
        host.clear_damage();

        host.moved_subtrees.extend([row(3), row(5)]);
        host.push(row(0), PaintDamage::DESCENDANT_READERS);
        let second = record_next_frame(&mut host, &mut tree);

        assert_eq!(
            host.recorded,
            vec![
                (row(3), ProducerKind::DrawBackground),
                (row(5), ProducerKind::DrawForeground)
            ]
        );
        assert_matches_from_scratch(&mut host, &second);
    }

    #[test]
    fn descendant_readers_record_when_something_below_them_changed() {
        let mut host = cards_host();
        host.plan(
            foreground(row(3)),
            true,
            &[
                ScopePlanItem::Producer(ProducerKind::Svg),
                ScopePlanItem::Scope(foreground(row(5))),
            ],
        );
        host.produce(row(3), ProducerKind::Svg, content(1, 1));
        host.descendant_readers.insert((row(3), ProducerKind::Svg));
        host.plan(
            foreground(row(5)),
            true,
            &[ScopePlanItem::Producer(ProducerKind::DrawForeground)],
        );
        host.produce(row(5), ProducerKind::DrawForeground, content(1, 0));
        let mut tree = PaintOrderTree::default();
        let first = record_frame(&mut host, &mut tree);
        host.source = Some(first);
        host.clear_damage();

        host.push(row(5), PaintDamage::DRAW_FOREGROUND);
        host.push(row(3), PaintDamage::DESCENDANT_READERS);
        host.push(row(0), PaintDamage::DESCENDANT_READERS);
        let second = record_next_frame(&mut host, &mut tree);

        assert_eq!(
            host.recorded,
            vec![(row(3), ProducerKind::Svg), (row(5), ProducerKind::DrawForeground)]
        );
        assert_matches_from_scratch(&mut host, &second);
    }

    #[test]
    fn live_producers_record_every_frame_and_keep_the_frame_changing() {
        let mut host = cards_host();
        host.produce(
            row(2),
            ProducerKind::DrawBackground,
            Content {
                live: true,
                ..content(1, 0)
            },
        );
        let mut tree = PaintOrderTree::default();
        let first = record_frame(&mut host, &mut tree);
        host.source = Some(first);
        host.clear_damage();
        assert!(!frame_is_unchanged(&tree, false));

        let second = record_next_frame(&mut host, &mut tree);

        assert_eq!(host.recorded, vec![(row(2), ProducerKind::DrawBackground)]);
        assert_matches_from_scratch(&mut host, &second);
    }

    #[test]
    fn an_inactive_context_records_nothing_and_retires_its_children() {
        let mut host = cards_host();
        let mut tree = PaintOrderTree::default();
        let first = record_frame(&mut host, &mut tree);
        host.source = Some(first);
        host.clear_damage();

        let mut hidden = host.plans[&context(row(0))].clone();
        hidden.active = false;
        host.plans.insert(context(row(0)), hidden);
        host.push(row(0), PaintDamage::ELIGIBILITY);
        let second = record_next_frame(&mut host, &mut tree);

        assert!(host.recorded.is_empty());
        assert_eq!(second.root_entry.output(), OutputSize::default());
        assert_eq!(second.display_list.bytes.len(), second.prologue_bytes as usize);
        assert_eq!(tree.scope_count(), 1);
        assert_matches_from_scratch(&mut host, &second);
    }

    #[test]
    fn blocking_wheel_regions_are_counted_through_copies() {
        let mut host = cards_host();
        host.produce(
            row(1),
            ProducerKind::HitForeground,
            Content {
                blocking: 1,
                ..content(0, 2)
            },
        );
        let mut tree = PaintOrderTree::default();
        let first = record_frame(&mut host, &mut tree);
        assert_eq!(first.root_entry.output().blocking_wheel_event_regions, 1);
        assert_eq!(tree.root_entry().output().blocking_wheel_event_regions, 1);
        host.source = Some(first);
        host.clear_damage();

        host.push(row(3), PaintDamage::DRAW_FOREGROUND);
        host.push(row(0), PaintDamage::DESCENDANT_READERS);
        let second = record_next_frame(&mut host, &mut tree);

        assert_eq!(host.recorded, vec![(row(3), ProducerKind::DrawForeground)]);
        assert_eq!(second.root_entry.output().blocking_wheel_event_regions, 1);
        assert_eq!(tree.root_entry().output().blocking_wheel_event_regions, 1);
    }
}
