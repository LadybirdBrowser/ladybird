/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The paint-order tree retained from the published frame. Every scope the planner produced is
//! a node with a stable id. A node's child entries name producers of the node's owner and child
//! scopes, each with the size of the output it contributed, so a child's position in the
//! published tape is the sum of the sizes before it and a change inside one scope never touches
//! the entries of another.

use crate::css::style::fast_hash::FastMap;
use crate::layout::node_data::NodeSlotId;
use crate::painting::paint_order_plan::{PaintScope, PaintScopeKind, StackingContextPaintPhase};
use crate::painting::record::damage::PaintDamage;
use smallvec::SmallVec;

#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct ScopeId(u32);

const SCOPE_INDEX_BITS: u32 = 24;
const SCOPE_INDEX_MASK: u32 = (1 << SCOPE_INDEX_BITS) - 1;
// A child entry tells scopes from producers by the top bit of its key, which an id therefore
// never sets: a slot whose generation reaches this many retirements is never reused.
const SCOPE_GENERATION_LIMIT: u8 = 127;

impl ScopeId {
    pub(crate) const NONE: Self = Self(u32::MAX);

    fn new(index: usize, generation: u8) -> Self {
        assert!(
            index < SCOPE_INDEX_MASK as usize,
            "paint-order tree exhausted its scope ids"
        );
        assert!(
            (1..=SCOPE_GENERATION_LIMIT).contains(&generation),
            "paint scope generation {generation} is out of range"
        );
        Self(index as u32 | (u32::from(generation) << SCOPE_INDEX_BITS))
    }

    fn index(self) -> usize {
        (self.0 & SCOPE_INDEX_MASK) as usize
    }

    fn generation(self) -> u8 {
        (self.0 >> SCOPE_INDEX_BITS) as u8
    }

    pub(crate) fn is_none(self) -> bool {
        self == Self::NONE
    }
}

impl Default for ScopeId {
    fn default() -> Self {
        Self::NONE
    }
}

impl std::fmt::Debug for ScopeId {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.is_none() {
            write!(formatter, "ScopeId(none)")
        } else {
            write!(formatter, "ScopeId({}g{})", self.index(), self.generation())
        }
    }
}

/// One unit of recorded output owned by a row. The order of the draw and hit-test phases
/// mirrors `PaintPhase`, and every kind has the damage bit that makes it record again.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
#[repr(u8)]
pub(crate) enum ProducerKind {
    DrawBackground = 0,
    DrawBorder,
    DrawTableCollapsedBorder,
    DrawForeground,
    DrawOutline,
    DrawOverlay,
    HitBackground,
    HitForeground,
    HitOverlay,
    ScrollMetadata,
    ScopePreamble,
    Svg,
    InlinePiece(u32),
    TextFragment(u32),
}

impl ProducerKind {
    const ALL: [Self; 12] = [
        Self::DrawBackground,
        Self::DrawBorder,
        Self::DrawTableCollapsedBorder,
        Self::DrawForeground,
        Self::DrawOutline,
        Self::DrawOverlay,
        Self::HitBackground,
        Self::HitForeground,
        Self::HitOverlay,
        Self::ScrollMetadata,
        Self::ScopePreamble,
        Self::Svg,
    ];

    pub(crate) fn damage(self) -> PaintDamage {
        match self {
            Self::DrawBackground => PaintDamage::DRAW_BACKGROUND,
            Self::DrawBorder => PaintDamage::DRAW_BORDER,
            Self::DrawTableCollapsedBorder => PaintDamage::DRAW_TABLE_COLLAPSED_BORDER,
            Self::DrawForeground => PaintDamage::DRAW_FOREGROUND,
            Self::DrawOutline => PaintDamage::DRAW_OUTLINE,
            Self::DrawOverlay => PaintDamage::DRAW_OVERLAY,
            Self::HitBackground => PaintDamage::HIT_BACKGROUND,
            Self::HitForeground => PaintDamage::HIT_FOREGROUND,
            Self::HitOverlay => PaintDamage::HIT_OVERLAY,
            Self::ScrollMetadata => PaintDamage::SCROLL_METADATA,
            Self::ScopePreamble => PaintDamage::SCOPE_PREAMBLE,
            Self::Svg => PaintDamage::SVG,
            Self::InlinePiece(_) => {
                PaintDamage::DRAW_BACKGROUND | PaintDamage::DRAW_BORDER | PaintDamage::DRAW_FOREGROUND
            }
            Self::TextFragment(_) => PaintDamage::DRAW_FOREGROUND,
        }
    }

    fn from_code(code: u32) -> Self {
        match code & 15 {
            12 => Self::InlinePiece(code >> 4),
            13 => Self::TextFragment(code >> 4),
            kind => Self::ALL[kind as usize],
        }
    }

    pub(super) fn code(self) -> u32 {
        match self {
            Self::InlinePiece(index) | Self::TextFragment(index) => {
                assert!(index < 1 << 27, "inline content exceeds producer key capacity");
                (index << 4) | if matches!(self, Self::InlinePiece(_)) { 12 } else { 13 }
            }
            _ => Self::ALL.iter().position(|kind| *kind == self).unwrap() as u32,
        }
    }
}

/// The output one child contributed to the published frame.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct OutputSize {
    pub bytes: u32,
    pub hits: u32,
    pub blocking_wheel_event_regions: u32,
    // Recorded again every frame: scroll-offset-dependent content and resources without
    // retained output. A scope is live when any child is.
    pub live: bool,
}

impl OutputSize {
    pub(crate) fn add(&mut self, other: OutputSize) {
        self.bytes += other.bytes;
        self.hits += other.hits;
        self.blocking_wheel_event_regions += other.blocking_wheel_event_regions;
        self.live |= other.live;
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum ChildKey {
    Scope(ScopeId),
    Producer(ProducerKind),
}

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ChildEntry {
    key: u32,
    bytes: u32,
    hits: u32,
    meta: u32,
}

const _: () = assert!(std::mem::size_of::<ChildEntry>() == 16);

const SCOPE_KEY_BIT: u32 = 1 << 31;
const META_LIVE: u32 = 1;
const META_BLOCKING_REGIONS_SHIFT: u32 = 8;

impl ChildEntry {
    pub(crate) fn scope(id: ScopeId, output: OutputSize) -> Self {
        debug_assert!(!id.is_none());
        Self::with_key(SCOPE_KEY_BIT | id.0, output)
    }

    pub(crate) fn producer(kind: ProducerKind, output: OutputSize) -> Self {
        Self::with_key(kind.code(), output)
    }

    fn with_key(key: u32, output: OutputSize) -> Self {
        let meta = (output.blocking_wheel_event_regions << META_BLOCKING_REGIONS_SHIFT)
            | if output.live { META_LIVE } else { 0 };
        Self {
            key,
            bytes: output.bytes,
            hits: output.hits,
            meta,
        }
    }

    pub(crate) fn key(&self) -> ChildKey {
        if self.key & SCOPE_KEY_BIT != 0 {
            ChildKey::Scope(ScopeId(self.key & !SCOPE_KEY_BIT))
        } else {
            ChildKey::Producer(ProducerKind::from_code(self.key))
        }
    }

    pub(crate) fn output(&self) -> OutputSize {
        OutputSize {
            bytes: self.bytes,
            hits: self.hits,
            blocking_wheel_event_regions: self.meta >> META_BLOCKING_REGIONS_SHIFT,
            live: self.meta & META_LIVE != 0,
        }
    }

    pub(crate) fn is_live(&self) -> bool {
        self.meta & META_LIVE != 0
    }
}

impl Default for ChildEntry {
    fn default() -> Self {
        Self::scope(ScopeId(SCOPE_INDEX_MASK), OutputSize::default())
    }
}

const NODE_OCCUPIED: u8 = 1;

#[repr(C)]
#[derive(Clone, Copy)]
struct ScopeNode {
    owner: NodeSlotId,
    parent: ScopeId,
    children_start: u32,
    children_len: u32,
    // Equal to the tree's assembly stamp while the node is on a damaged path of the current
    // recording, which is what makes an ancestor recurse into it instead of copying it.
    recursion_stamp: u32,
    kind: u8,
    flags: u8,
    generation: u8,
    _padding: u8,
}

const _: () = assert!(std::mem::size_of::<ScopeNode>() == 24);

fn scope_kind_code(kind: PaintScopeKind) -> u8 {
    match kind {
        PaintScopeKind::PaintedAsStackingContext => 0,
        PaintScopeKind::Descendants(phase) => 1 + phase as u8,
    }
}

fn scope_kind_from_code(code: u8) -> PaintScopeKind {
    match code {
        0 => PaintScopeKind::PaintedAsStackingContext,
        1 => PaintScopeKind::Descendants(StackingContextPaintPhase::BackgroundAndBorders),
        2 => PaintScopeKind::Descendants(StackingContextPaintPhase::Floats),
        3 => PaintScopeKind::Descendants(StackingContextPaintPhase::BackgroundAndBordersForInlineLevelAndReplaced),
        4 => PaintScopeKind::Descendants(StackingContextPaintPhase::Foreground),
        _ => unreachable!("unknown paint scope kind code {code}"),
    }
}

pub(crate) const SCOPE_KIND_COUNT: u8 = 1 + StackingContextPaintPhase::COUNT as u8;

#[derive(Default)]
pub(crate) struct PaintOrderTree {
    nodes: Vec<ScopeNode>,
    free_nodes: Vec<u32>,
    children: Vec<ChildEntry>,
    live_child_entries: u32,
    // The scope of each kind a row owns, indexed by the row's slot. A second scope with the
    // same owner and kind, which the planner produces only for unusual inline-level
    // configurations, lives in the overflow map.
    scopes_by_row: Vec<[ScopeId; SCOPE_KIND_COUNT as usize]>,
    duplicate_scopes: FastMap<(NodeSlotId, u8), SmallVec<[ScopeId; 2]>>,
    root: ScopeId,
    root_entry: ChildEntry,
    assembly_stamp: u32,
}

impl PaintOrderTree {
    pub(crate) fn root(&self) -> ScopeId {
        self.root
    }

    pub(crate) fn root_entry(&self) -> ChildEntry {
        self.root_entry
    }

    #[cfg(test)]
    pub(crate) fn scope_count(&self) -> usize {
        self.nodes.iter().filter(|node| node.flags & NODE_OCCUPIED != 0).count()
    }

    #[cfg(test)]
    pub(crate) fn child_entry_capacity(&self) -> usize {
        self.children.len()
    }

    fn node(&self, id: ScopeId) -> &ScopeNode {
        let node = &self.nodes[id.index()];
        assert_eq!(node.generation, id.generation(), "stale paint scope id {id:?}");
        debug_assert!(node.flags & NODE_OCCUPIED != 0, "freed paint scope id {id:?}");
        node
    }

    fn node_mut(&mut self, id: ScopeId) -> &mut ScopeNode {
        let node = &mut self.nodes[id.index()];
        assert_eq!(node.generation, id.generation(), "stale paint scope id {id:?}");
        debug_assert!(node.flags & NODE_OCCUPIED != 0, "freed paint scope id {id:?}");
        node
    }

    pub(crate) fn scope_of(&self, id: ScopeId) -> PaintScope {
        let node = self.node(id);
        PaintScope {
            owner: node.owner,
            kind: scope_kind_from_code(node.kind),
        }
    }

    #[cfg(test)]
    pub(crate) fn parent(&self, id: ScopeId) -> ScopeId {
        self.node(id).parent
    }

    pub(crate) fn children(&self, id: ScopeId) -> &[ChildEntry] {
        let node = self.node(id);
        let start = node.children_start as usize;
        &self.children[start..start + node.children_len as usize]
    }

    fn scope_in_slot(&self, owner: NodeSlotId, kind_code: u8) -> Option<ScopeId> {
        let id = self.scopes_by_row.get(owner.slot_index() as usize)?[kind_code as usize];
        if id.is_none() {
            return None;
        }
        let node = &self.nodes[id.index()];
        (node.generation == id.generation() && node.flags & NODE_OCCUPIED != 0 && node.owner == owner).then_some(id)
    }

    /// The scope for a plan item under `parent`, if the tree has one. A scope allocated by the
    /// current recording qualifies too; a caller copying from the published frame tells the
    /// two apart by the parent's published child list.
    pub(crate) fn published_scope(&self, scope: PaintScope, parent: ScopeId) -> Option<ScopeId> {
        let kind_code = scope_kind_code(scope.kind);
        if let Some(id) = self.scope_in_slot(scope.owner, kind_code)
            && self.node(id).parent == parent
        {
            return Some(id);
        }
        let ids = self.duplicate_scopes.get(&(scope.owner, kind_code))?;
        ids.iter().copied().find(|id| self.node(*id).parent == parent)
    }

    /// Every scope a row owns, in kind order.
    pub(crate) fn scopes_owned_by(&self, owner: NodeSlotId) -> SmallVec<[ScopeId; 5]> {
        let mut scopes = SmallVec::new();
        for code in 0..SCOPE_KIND_COUNT {
            if let Some(id) = self.scope_in_slot(owner, code) {
                scopes.push(id);
            }
            if !self.duplicate_scopes.is_empty()
                && let Some(ids) = self.duplicate_scopes.get(&(owner, code))
            {
                scopes.extend(ids.iter().copied());
            }
        }
        scopes
    }

    fn register_scope(&mut self, id: ScopeId) {
        let node = self.nodes[id.index()];
        let index = node.owner.slot_index() as usize;
        if self.scopes_by_row.len() <= index {
            self.scopes_by_row
                .resize(index + 1, [ScopeId::NONE; SCOPE_KIND_COUNT as usize]);
        }
        if self.scope_in_slot(node.owner, node.kind).is_some() {
            self.duplicate_scopes
                .entry((node.owner, node.kind))
                .or_default()
                .push(id);
        } else {
            self.scopes_by_row[index][node.kind as usize] = id;
        }
    }

    fn unregister_scope(&mut self, id: ScopeId) {
        let node = self.nodes[id.index()];
        let index = node.owner.slot_index() as usize;
        let key = (node.owner, node.kind);
        if self.scopes_by_row[index][node.kind as usize] == id {
            // A duplicate of the same owner and kind, such as the replacement a scope gets when
            // it moves to another parent, takes over the slot.
            let promoted = self
                .duplicate_scopes
                .get_mut(&key)
                .and_then(|ids| ids.pop())
                .unwrap_or(ScopeId::NONE);
            self.scopes_by_row[index][node.kind as usize] = promoted;
        } else if let Some(ids) = self.duplicate_scopes.get_mut(&key) {
            ids.retain(|candidate| *candidate != id);
        }
        if self.duplicate_scopes.get(&key).is_some_and(|ids| ids.is_empty()) {
            self.duplicate_scopes.remove(&key);
        }
    }

    /// Starts a recording: the assembly stamp advances so no node counts as being on a damaged
    /// path yet.
    pub(crate) fn begin_recording(&mut self) -> u32 {
        self.assembly_stamp = self.assembly_stamp.wrapping_add(1);
        if self.assembly_stamp == 0 {
            for node in &mut self.nodes {
                node.recursion_stamp = 0;
            }
            self.assembly_stamp = 1;
        }
        self.assembly_stamp
    }

    /// Marks the path from a scope to the root as damaged for this recording, stopping at the
    /// first ancestor already marked.
    pub(crate) fn mark_path_to_root_damaged(&mut self, id: ScopeId) {
        let stamp = self.assembly_stamp;
        let mut current = id;
        while !current.is_none() {
            let node = self.node_mut(current);
            if node.recursion_stamp == stamp {
                break;
            }
            node.recursion_stamp = stamp;
            current = node.parent;
        }
    }

    pub(crate) fn is_on_damaged_path(&self, id: ScopeId) -> bool {
        self.node(id).recursion_stamp == self.assembly_stamp
    }

    /// Allocates a scope for this recording and registers it for its owner and kind.
    pub(crate) fn allocate_scope(&mut self, scope: PaintScope, parent: ScopeId) -> ScopeId {
        let node = ScopeNode {
            owner: scope.owner,
            parent,
            children_start: 0,
            children_len: 0,
            recursion_stamp: self.assembly_stamp,
            kind: scope_kind_code(scope.kind),
            flags: NODE_OCCUPIED,
            generation: 1,
            _padding: 0,
        };
        let id = if let Some(index) = self.free_nodes.pop() {
            let slot = &mut self.nodes[index as usize];
            debug_assert_eq!(slot.flags & NODE_OCCUPIED, 0);
            let generation = slot.generation;
            *slot = ScopeNode { generation, ..node };
            ScopeId::new(index as usize, generation)
        } else {
            self.nodes.push(node);
            ScopeId::new(self.nodes.len() - 1, 1)
        };
        self.register_scope(id);
        id
    }

    /// Installs the child list assembled for a scope. A list of the same length overwrites the
    /// previous one in place; any other length takes new entries at the end of the arena.
    pub(crate) fn set_children(&mut self, id: ScopeId, entries: &[ChildEntry]) {
        let node = *self.node(id);
        let len = u32::try_from(entries.len()).expect("paint-order tree exceeds u32 entries");
        if node.children_len == len {
            let start = node.children_start as usize;
            self.children[start..start + entries.len()].copy_from_slice(entries);
        } else {
            let start = u32::try_from(self.children.len()).expect("paint-order tree exceeds u32 entries");
            self.children.extend_from_slice(entries);
            self.live_child_entries = self.live_child_entries - node.children_len + len;
            let node = self.node_mut(id);
            node.children_start = start;
            node.children_len = len;
        }
    }

    /// Retires a scope no plan item matched, with its subtree. Its ids never match again and
    /// its slots are free for the scopes this recording allocates next.
    pub(crate) fn detach_scope(&mut self, id: ScopeId) {
        let mut stack = vec![id];
        while let Some(id) = stack.pop() {
            let node = *self.node(id);
            let start = node.children_start as usize;
            for entry in &self.children[start..start + node.children_len as usize] {
                if let ChildKey::Scope(child) = entry.key() {
                    stack.push(child);
                }
            }
            self.live_child_entries -= node.children_len;
            self.unregister_scope(id);
            let slot = &mut self.nodes[id.index()];
            slot.flags = 0;
            slot.children_len = 0;
            // A retired id never matches again; slots whose generation ran out stay unused.
            if slot.generation < SCOPE_GENERATION_LIMIT {
                slot.generation += 1;
                self.free_nodes.push(id.index() as u32);
            }
        }
    }

    /// Ends the recording with the root's entry, which describes the whole frame, and compacts
    /// the child arena when garbage outgrows the live entries.
    pub(crate) fn finish_recording(&mut self, root: ScopeId, entry: ChildEntry) {
        self.root = root;
        self.root_entry = entry;
        if self.children.len() as u32 - self.live_child_entries > self.live_child_entries {
            self.compact_child_entries();
        }
    }

    // Rebuilds the child arena from the live lists in tree order; nodes keep their ids.
    fn compact_child_entries(&mut self) {
        let mut compacted = Vec::with_capacity(self.live_child_entries as usize);
        let mut stack = vec![self.root];
        while let Some(id) = stack.pop() {
            if id.is_none() {
                continue;
            }
            let node = *self.node(id);
            let start = node.children_start as usize;
            let new_start = compacted.len() as u32;
            compacted.extend_from_slice(&self.children[start..start + node.children_len as usize]);
            self.nodes[id.index()].children_start = new_start;
            for entry in compacted[new_start as usize..].iter().rev() {
                if let ChildKey::Scope(child) = entry.key() {
                    stack.push(child);
                }
            }
        }
        self.children = compacted;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn row(index: u32) -> NodeSlotId {
        NodeSlotId::new(index, 1)
    }

    fn output(bytes: u32, hits: u32) -> OutputSize {
        OutputSize {
            bytes,
            hits,
            ..OutputSize::default()
        }
    }

    fn context_scope(owner: NodeSlotId) -> PaintScope {
        PaintScope::stacking_context(owner)
    }

    fn foreground_scope(owner: NodeSlotId) -> PaintScope {
        PaintScope {
            owner,
            kind: PaintScopeKind::Descendants(StackingContextPaintPhase::Foreground),
        }
    }

    // A root context with one foreground scope holding `leaf_count` leaf scopes of one
    // producer each, recorded as the first frame.
    fn record_first_frame(tree: &mut PaintOrderTree, leaf_count: u32) -> (ScopeId, ScopeId, Vec<ScopeId>) {
        tree.begin_recording();
        let root = tree.allocate_scope(context_scope(row(0)), ScopeId::NONE);
        let foreground = tree.allocate_scope(foreground_scope(row(0)), root);
        let mut leaves = Vec::new();
        let mut foreground_children = Vec::new();
        let mut foreground_output = OutputSize::default();
        for index in 0..leaf_count {
            let leaf = tree.allocate_scope(foreground_scope(row(1 + index)), foreground);
            let leaf_output = output(16, 1);
            tree.set_children(leaf, &[ChildEntry::producer(ProducerKind::DrawForeground, leaf_output)]);
            foreground_children.push(ChildEntry::scope(leaf, leaf_output));
            foreground_output.add(leaf_output);
            leaves.push(leaf);
        }
        tree.set_children(foreground, &foreground_children);
        let mut root_output = output(32, 0);
        root_output.add(foreground_output);
        tree.set_children(
            root,
            &[
                ChildEntry::producer(ProducerKind::DrawBackground, output(32, 0)),
                ChildEntry::scope(foreground, foreground_output),
            ],
        );
        tree.finish_recording(root, ChildEntry::scope(root, root_output));
        (root, foreground, leaves)
    }

    #[test]
    fn scopes_keep_their_ids_and_sizes_add_up() {
        let mut tree = PaintOrderTree::default();
        let (root, foreground, leaves) = record_first_frame(&mut tree, 3);

        assert_eq!(tree.root(), root);
        assert_eq!(tree.root_entry().output(), output(32 + 3 * 16, 3));
        assert_eq!(tree.published_scope(foreground_scope(row(0)), root), Some(foreground));
        assert_eq!(
            tree.published_scope(foreground_scope(row(2)), foreground),
            Some(leaves[1])
        );
        assert_eq!(tree.published_scope(foreground_scope(row(2)), root), None);
        assert_eq!(tree.scopes_owned_by(row(0)).as_slice(), &[root, foreground]);
        assert_eq!(tree.children(foreground).len(), 3);
        assert_eq!(tree.scope_of(leaves[0]), foreground_scope(row(1)));
        assert_eq!(tree.parent(leaves[0]), foreground);
        assert_eq!(tree.scope_count(), 5);
    }

    #[test]
    fn damaged_paths_are_marked_up_to_the_first_marked_ancestor() {
        let mut tree = PaintOrderTree::default();
        let (root, foreground, leaves) = record_first_frame(&mut tree, 2);
        tree.begin_recording();

        tree.mark_path_to_root_damaged(leaves[0]);
        assert!(tree.is_on_damaged_path(leaves[0]));
        assert!(tree.is_on_damaged_path(foreground));
        assert!(tree.is_on_damaged_path(root));
        assert!(!tree.is_on_damaged_path(leaves[1]));

        tree.begin_recording();
        assert!(!tree.is_on_damaged_path(leaves[0]));
        assert!(!tree.is_on_damaged_path(root));
    }

    #[test]
    fn a_recording_replaces_child_lists_and_retires_detached_subtrees_at_once() {
        let mut tree = PaintOrderTree::default();
        let (root, foreground, leaves) = record_first_frame(&mut tree, 2);
        tree.begin_recording();

        let added = tree.allocate_scope(foreground_scope(row(9)), foreground);
        tree.set_children(
            added,
            &[ChildEntry::producer(ProducerKind::DrawForeground, output(8, 0))],
        );
        let children = [
            ChildEntry::scope(leaves[1], output(16, 1)),
            ChildEntry::scope(added, output(8, 0)),
        ];
        tree.set_children(foreground, &children);
        tree.detach_scope(leaves[0]);
        assert_eq!(tree.published_scope(foreground_scope(row(1)), foreground), None);
        assert_eq!(tree.scope_count(), 4);
        tree.finish_recording(root, ChildEntry::scope(root, output(32 + 16 + 8, 1)));

        assert_eq!(tree.children(foreground), &children);
        assert_eq!(tree.root_entry().output(), output(56, 1));
        assert_eq!(tree.published_scope(foreground_scope(row(9)), foreground), Some(added));

        // The retired slot is reused with a new generation, so the old id can never match.
        tree.begin_recording();
        let reused = tree.allocate_scope(foreground_scope(row(7)), foreground);
        assert_eq!(reused.index(), leaves[0].index());
        assert_ne!(reused, leaves[0]);
    }

    #[test]
    fn a_scope_moving_to_another_parent_takes_over_the_slot_its_predecessor_leaves() {
        let mut tree = PaintOrderTree::default();
        let (root, foreground, leaves) = record_first_frame(&mut tree, 2);
        tree.begin_recording();

        // Row 1's foreground scope is listed under the root now; the old one is still there.
        let moved = tree.allocate_scope(foreground_scope(row(1)), root);
        assert_eq!(tree.published_scope(foreground_scope(row(1)), root), Some(moved));
        assert_eq!(
            tree.published_scope(foreground_scope(row(1)), foreground),
            Some(leaves[0])
        );
        assert_eq!(tree.scopes_owned_by(row(1)).len(), 2);

        tree.detach_scope(leaves[0]);
        assert_eq!(tree.published_scope(foreground_scope(row(1)), root), Some(moved));
        assert_eq!(tree.scopes_owned_by(row(1)).as_slice(), &[moved]);
        assert!(tree.duplicate_scopes.is_empty());
    }

    #[test]
    fn a_slot_retired_many_times_keeps_its_ids_distinguishable_from_producers() {
        let mut tree = PaintOrderTree::default();
        let (root, foreground, _) = record_first_frame(&mut tree, 1);
        for round in 0..300u32 {
            tree.begin_recording();
            for child in tree.children(foreground).to_vec() {
                if let ChildKey::Scope(previous) = child.key() {
                    tree.detach_scope(previous);
                }
            }
            let added = tree.allocate_scope(foreground_scope(row(100 + round)), foreground);
            tree.set_children(
                added,
                &[ChildEntry::producer(ProducerKind::DrawForeground, output(8, 0))],
            );
            let entry = ChildEntry::scope(added, output(8, 0));
            assert_eq!(entry.key(), ChildKey::Scope(added));
            tree.set_children(foreground, &[entry]);
            tree.finish_recording(root, ChildEntry::scope(root, output(40, 0)));
            assert_eq!(
                tree.published_scope(foreground_scope(row(100 + round)), foreground),
                Some(added)
            );
        }
        assert_eq!(tree.scope_count(), 3);
    }

    #[test]
    fn same_length_child_lists_are_overwritten_in_place() {
        let mut tree = PaintOrderTree::default();
        let (root, foreground, leaves) = record_first_frame(&mut tree, 2);
        let capacity = tree.child_entry_capacity();
        tree.begin_recording();

        let grown = output(24, 1);
        tree.set_children(leaves[0], &[ChildEntry::producer(ProducerKind::DrawForeground, grown)]);
        let children = [
            ChildEntry::scope(leaves[0], grown),
            ChildEntry::scope(leaves[1], output(16, 1)),
        ];
        tree.set_children(foreground, &children);
        tree.finish_recording(root, ChildEntry::scope(root, output(32 + 24 + 16, 2)));

        assert_eq!(tree.children(foreground), &children);
        assert_eq!(tree.child_entry_capacity(), capacity);
    }

    #[test]
    fn the_child_arena_stays_bounded_under_staggered_updates() {
        let mut tree = PaintOrderTree::default();
        let (root, foreground, leaves) = record_first_frame(&mut tree, 8);
        let mut peak = 0;
        for round in 0..4000u32 {
            tree.begin_recording();
            let leaf = leaves[(round % 8) as usize];
            let leaf_output = output(16 + round % 3, 1);
            // Every other round the leaf gains or loses a producer, so its list changes length.
            let leaf_children: Vec<ChildEntry> = if round % 2 == 0 {
                vec![ChildEntry::producer(ProducerKind::DrawForeground, leaf_output)]
            } else {
                vec![
                    ChildEntry::producer(ProducerKind::DrawBackground, output(0, 0)),
                    ChildEntry::producer(ProducerKind::DrawForeground, leaf_output),
                ]
            };
            tree.set_children(leaf, &leaf_children);
            let children: Vec<ChildEntry> = tree
                .children(foreground)
                .iter()
                .map(|entry| match entry.key() {
                    ChildKey::Scope(id) if id == leaf => ChildEntry::scope(leaf, leaf_output),
                    _ => *entry,
                })
                .collect();
            tree.set_children(foreground, &children);
            let mut total = output(32, 0);
            for entry in &children {
                total.add(entry.output());
            }
            tree.finish_recording(root, ChildEntry::scope(root, total));
            peak = peak.max(tree.child_entry_capacity());
            assert_eq!(tree.root_entry().output().hits, 8);
        }
        let live = 2 + 8 + 2 * 8;
        assert!(
            peak <= live * 2 + 16,
            "child arena grew to {peak} entries for {live} live ones"
        );
        assert_eq!(tree.scope_count(), 10);
    }
}
