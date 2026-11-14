/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The layout tree update marks: what the DOM asks the next layout tree build to rebuild. A mark is
//! written where the DOM changes and read by the next build, which retires it, keyed by the style
//! node identity the build walks by. The document's host holds them, and lends them to the layout
//! arena for each job of the render owner, and to the frame in flight.

use super::LayoutNodeArena;
use super::node_data::{GENERATED_FOR_BACKDROP, NodeFlag, NodeKind, NodeSlotId};
use crate::css::computed_value_types::ComputedSize;
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::style::bridge::element_adjustment_fact;
use crate::css::style::tree::StyleNodeID;
use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::record::damage::PaintDamage;
use crate::render_state::DocumentHost;

/// Which narrower rebuild the marks a node has collected so far still permit, as
/// `Node::LayoutTreeUpdateReuseReason` spells them. Nothing set means only a full rebuild will do.
pub(crate) mod layout_tree_update_reuse_reason {
    pub(crate) const CHILD_LIST_INSERTION: u8 = 1;
    pub(crate) const PSEUDO_ELEMENT_CHANGE: u8 = 2;
    /// A node left the child list. Bits 4 and 8 are the marks themselves.
    pub(crate) const CHILD_LIST_REMOVAL: u8 = 16;
    /// The element's style changed in a way its box is built again for, where the box may stay if a fresh build gives
    /// the element a box of the same kind: its record changes, and its child list and pseudo-elements are planned again.
    /// Bits 32 and 64 are notes of the mark.
    pub(crate) const RESTYLE: u8 = 128;
    pub(super) const ALL: u8 = CHILD_LIST_INSERTION | PSEUDO_ELEMENT_CHANGE | CHILD_LIST_REMOVAL | RESTYLE;
}

/// The build has to rebuild what the node produces.
const NEEDS: u8 = 1 << 2;
/// A style change of the element, whose box the build builds again, changed what it does to the counters or the quote
/// depth that the boxes after it read. The box keeps its old style until the build, so only the mark can tell.
const GENERATED_CONTENT_CHANGED: u8 = 1 << 5;
/// A style change of the element made it, or one of its pseudo-elements, a list item, or no longer one, which moves the
/// `list-item` counter value of the list items after it.
const LIST_ITEM_CHANGED: u8 = 1 << 6;

/// What a style change of an element changed of what its box does to the boxes after it.
#[derive(Clone, Copy, PartialEq, Eq)]
enum GeneratedContentChange {
    None,
    ListItems,
    CountersOrQuotes,
}

/// One byte of marks per identity, in the element and the text index spaces apart. The low bits
/// are the reuse reasons, so no reason ever needs translating.
#[derive(Clone, Default)]
pub(crate) struct LayoutTreeUpdateMarks {
    elements: Vec<u8>,
    text: Vec<u8>,
    /// The nodes whose own mark was set since the build took the list last, in the order they were marked: what the
    /// build finds the child lists it plans again from, without walking down to them. A node whose mark was retired
    /// and set again is in it again, and one whose mark was retired since is in it still.
    dirty: Vec<StyleNodeID>,
    /// The boxes removed nodes left in the child lists of boxes that stay, since the build took them last: for each
    /// removal, the row of each entry of such a list that the removed nodes had. Their identities are retired once they
    /// leave the document, so these rows are what names the entries a list lost.
    removed_entries: Vec<NodeSlotId>,
}

impl LayoutTreeUpdateMarks {
    fn get(&self, node: StyleNodeID) -> u8 {
        let (column, index) = match node.text_index() {
            Some(index) => (&self.text, index as usize),
            None => (&self.elements, node.element_slot()),
        };
        column.get(index).copied().unwrap_or(0)
    }

    fn update(&mut self, node: StyleNodeID, update: impl FnOnce(u8) -> u8) {
        let (column, index) = match node.text_index() {
            Some(index) => (&mut self.text, index as usize),
            None => (&mut self.elements, node.element_slot()),
        };
        let current = column.get(index).copied().unwrap_or(0);
        let updated = update(current);
        if updated == current {
            return;
        }
        if index >= column.len() {
            column.resize(index + 1, 0);
        }
        column[index] = updated;
    }

    /// Whether the node holds any mark.
    pub(crate) fn holds_any(&self, node: StyleNodeID) -> bool {
        self.get(node) != 0
    }

    /// Whether the layout tree build has to rebuild what this node produces.
    pub(crate) fn needs(&self, node: StyleNodeID) -> bool {
        self.get(node) & NEEDS != 0
    }

    /// Which narrower rebuilds the marks collected on this node still permit. See
    /// [`layout_tree_update_reuse_reason`].
    pub(crate) fn reuse_reasons(&self, node: StyleNodeID) -> u8 {
        self.get(node) & layout_tree_update_reuse_reason::ALL
    }

    /// Whether a style change of the element changed what its box does to the counters or the quote depth after it.
    pub(crate) fn generated_content_changed(&self, node: StyleNodeID) -> bool {
        self.get(node) & GENERATED_CONTENT_CHANGED != 0
    }

    /// Whether a style change of the element made it, or one of its pseudo-elements, a list item, or no longer one.
    pub(crate) fn list_item_changed(&self, node: StyleNodeID) -> bool {
        self.get(node) & LIST_ITEM_CHANGED != 0
    }

    fn note_generated_content_change(&mut self, node: StyleNodeID, change: GeneratedContentChange) {
        let bit = match change {
            GeneratedContentChange::None => return,
            GeneratedContentChange::ListItems => LIST_ITEM_CHANGED,
            GeneratedContentChange::CountersOrQuotes => GENERATED_CONTENT_CHANGED,
        };
        self.update(node, |marks| marks | bit);
    }

    /// Whether a node was marked since the build took the marked nodes last: the build has something to do, but where
    /// every such mark was retired since.
    pub(crate) fn need_build(&self) -> bool {
        !self.dirty.is_empty()
    }

    /// Fold one mark into the node's, answering whether its own bit changed, the reasons it holds changed, as where a
    /// reason it did not hold arrived or the box it marked for a narrower rebuild no longer stays, or the mark is for a
    /// restyle, whose change of the style the box still has is read again. That answer is what tells the mark site it
    /// has a transition to widen from. Once a reason that forbids reuse arrives, a later one cannot narrow it back.
    pub(crate) fn merge(&mut self, node: StyleNodeID, value: bool, reuse_reason: u8) -> bool {
        let reuse_reason = reuse_reason & layout_tree_update_reuse_reason::ALL;
        let was_needed = self.needs(node);
        let mut changed = false;
        self.update(node, |marks| {
            let reasons = marks & layout_tree_update_reuse_reason::ALL;
            let rest = marks & !(NEEDS | layout_tree_update_reuse_reason::ALL);
            if (marks & NEEDS != 0) == value {
                let merged = if reuse_reason == 0 || reasons == 0 {
                    0
                } else {
                    reasons | reuse_reason
                };
                changed = value && (merged != reasons || reuse_reason & layout_tree_update_reuse_reason::RESTYLE != 0);
                return rest | (marks & NEEDS) | merged;
            }
            changed = true;
            rest | if value { NEEDS } else { 0 } | reuse_reason
        });
        if value && !was_needed {
            // NB: A node whose mark was retired and set again is in the list again, and identities are reused, so a
            //     document that is never laid out, such as a hidden one, would grow it for as long as script inserts
            //     nodes. Before the list grows past one entry per identity, the repeated entries go.
            if self.dirty.len() == self.dirty.capacity() && self.dirty.len() > self.elements.len() + self.text.len() {
                self.dirty.sort_unstable();
                self.dirty.dedup();
            }
            self.dirty.push(node);
        }
        changed
    }

    /// Takes the nodes whose own mark was set since the list was taken last, see [`Self::dirty`].
    pub(crate) fn take_dirty(&mut self) -> Vec<StyleNodeID> {
        std::mem::take(&mut self.dirty)
    }

    /// Gives back `dirty`, nodes taken with [`Self::take_dirty`] whose marks stay for the next build.
    pub(crate) fn keep_dirty(&mut self, dirty: Vec<StyleNodeID>) {
        if self.dirty.is_empty() {
            self.dirty = dirty;
        } else {
            self.dirty.extend(dirty);
        }
    }

    /// Notes `row`, the box of an entry of a list whose box stays, as the box of a node that left the document.
    pub(crate) fn note_removed_entry(&mut self, row: NodeSlotId) {
        self.removed_entries.push(row);
    }

    /// Takes the rows noted with [`Self::note_removed_entry`] since they were taken last.
    pub(crate) fn take_removed_entries(&mut self) -> Vec<NodeSlotId> {
        std::mem::take(&mut self.removed_entries)
    }

    /// Retire the marks the node holds: the build has just answered them, or
    /// the identity has been handed to another node.
    pub(crate) fn clear(&mut self, node: StyleNodeID) {
        self.update(node, |_| 0);
    }

    /// Fold in `other`, marks made from none by merges alone, as if each of them
    /// had been made to these: a node's reuse reasons combine the same in any order.
    pub(crate) fn fold(&mut self, other: Self) {
        fold_column(&mut self.elements, other.elements);
        fold_column(&mut self.text, other.text);
        self.dirty.extend(other.dirty);
        self.removed_entries.extend(other.removed_entries);
    }
}

fn fold_column(column: &mut Vec<u8>, other: Vec<u8>) {
    if column.len() < other.len() {
        column.resize(other.len(), 0);
    }
    for (marks, other) in column.iter_mut().zip(other) {
        *marks = fold_marks(*marks, other);
    }
}

/// The marks of a node with `marks` once the merges that made `other` are made to them.
fn fold_marks(marks: u8, other: u8) -> u8 {
    use layout_tree_update_reuse_reason::ALL;
    let mut folded = marks | (other & (GENERATED_CONTENT_CHANGED | LIST_ITEM_CHANGED));
    if other & NEEDS != 0 {
        let (own, reasons) = (marks & ALL, other & ALL);
        let merged = match marks & NEEDS != 0 {
            true if own == 0 || reasons == 0 => 0,
            true => own | reasons,
            false => reasons,
        };
        folded = (folded & !ALL) | NEEDS | merged;
    }
    folded
}

/// A write of one node's layout tree update marks.
#[derive(Clone, Copy)]
pub(crate) enum LayoutTreeUpdateMarkWrite {
    /// See [`LayoutTreeUpdateMarks::merge`].
    Merge(StyleNodeID, bool, u8),
    /// See [`LayoutTreeUpdateMarks::clear`].
    Clear(StyleNodeID),
}

impl LayoutTreeUpdateMarkWrite {
    /// Makes the write to `marks`, answering what the write it makes answers.
    pub(crate) fn apply(self, marks: &mut LayoutTreeUpdateMarks) -> bool {
        match self {
            Self::Merge(node, value, reuse_reason) => marks.merge(node, value, reuse_reason),
            Self::Clear(node) => {
                marks.clear(node);
                false
            }
        }
    }
}

/// Answers `read` of the layout tree update marks of `host`'s document for the node `style_node` names, which the host
/// holds (see [`DocumentHost::read_marks`]). An identity of 0 names no node, and `read` is not run for it.
fn read_marks<R: Default>(
    host: &DocumentHost,
    style_node: u32,
    read: fn(&LayoutTreeUpdateMarks, StyleNodeID) -> R,
) -> R {
    let Some(style_node) = StyleNodeID::from_raw(style_node) else {
        return R::default();
    };
    host.read_marks(|marks| read(marks, style_node))
}

/// Makes `write` to the layout tree update marks of `host`'s document, which the host holds, answering what it answers.
fn write_marks(host: &DocumentHost, write: LayoutTreeUpdateMarkWrite) -> bool {
    host.write_marks(write)
}

/// Whether a node of `host`'s document was marked for a layout tree update since the build took the marks last, see
/// [`LayoutTreeUpdateMarks::need_build`].
#[unsafe(no_mangle)]
pub extern "C" fn render_state_layout_tree_update_marks_need_build(host: &DocumentHost) -> bool {
    host.read_marks(LayoutTreeUpdateMarks::need_build)
}

/// Whether the node `style_node` names holds a layout tree update mark of its own.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_needs_layout_tree_update(host: &DocumentHost, style_node: u32) -> bool {
    read_marks(host, style_node, LayoutTreeUpdateMarks::needs)
}

/// Which narrower rebuilds the marks the node `style_node` names has collected still permit.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_layout_tree_update_reuse_reasons(host: &DocumentHost, style_node: u32) -> u8 {
    read_marks(host, style_node, LayoutTreeUpdateMarks::reuse_reasons)
}

/// Folds a layout tree update mark into the one the node `style_node` names holds, answering
/// whether it is a transition the mark site widens the rebuild from (see
/// [`LayoutTreeUpdateMarks::merge`]).
#[unsafe(no_mangle)]
pub extern "C" fn render_state_merge_layout_tree_update_mark(
    host: &DocumentHost,
    style_node: u32,
    value: bool,
    reuse_reason: u8,
) -> bool {
    let Some(style_node) = StyleNodeID::from_raw(style_node) else {
        return false;
    };
    write_marks(host, LayoutTreeUpdateMarkWrite::Merge(style_node, value, reuse_reason))
}

/// Retires the layout tree update marks the node `style_node` names holds. An identity handed to a node holds none,
/// whatever the node that held it before left behind.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_clear_layout_tree_update_marks(host: &DocumentHost, style_node: u32) {
    let Some(style_node) = StyleNodeID::from_raw(style_node) else {
        return;
    };
    write_marks(host, LayoutTreeUpdateMarkWrite::Clear(style_node));
}

/// A layout tree update mark as the render owner applies it, which says what the reason the DOM made it for asks of the
/// box it marks.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiLayoutTreeUpdateMark {
    /// See [`layout_tree_update_reuse_reason`].
    pub reuse_reason: u8,
    /// The mark is for children inserted under the node, whose layout invalidation waits for the build.
    pub is_child_list_insertion: bool,
    /// A partial relayout boundary marked for this reason rebuilds as itself alone.
    pub is_structural_boundary_self_rebuild: bool,
}

impl FfiLayoutTreeUpdateMark {
    /// What a node insertion marks the node and its parent with.
    pub(crate) const NODE_INSERT: Self = Self {
        reuse_reason: layout_tree_update_reuse_reason::CHILD_LIST_INSERTION,
        is_child_list_insertion: true,
        is_structural_boundary_self_rebuild: true,
    };

    pub(crate) const CHARACTER_DATA_REPLACE_DATA: Self = Self {
        reuse_reason: 0,
        is_child_list_insertion: false,
        is_structural_boundary_self_rebuild: true,
    };

    /// The mark a style change makes for the next layout tree build, which may reuse a box as `reuse_reason` says.
    pub(crate) const fn style_change(reuse_reason: u8) -> Self {
        Self {
            reuse_reason,
            is_child_list_insertion: false,
            is_structural_boundary_self_rebuild: false,
        }
    }
}

impl LayoutNodeArena {
    /// Marks the node `node` names, or the document for `None`, for the next layout tree build to rebuild, as the DOM
    /// side does where a node changes, for a node that has a box: its box is invalidated as
    /// [`Self::apply_layout_tree_update_mark`] does. What only the DOM side knows
    /// of, the resources an SVG element lends its references and a display: contents element's parent, is not for a
    /// node with a box.
    pub(crate) fn mark_layout_tree_update(&mut self, node: Option<StyleNodeID>, mark: FfiLayoutTreeUpdateMark) {
        let document = self.document_style_node();
        let Some(style_node) = node.or(document) else {
            return;
        };
        if !self
            .layout_tree_update_marks_mut()
            .merge(style_node, true, mark.reuse_reason)
        {
            return;
        }
        self.apply_layout_tree_update_mark(node, mark);
    }

    /// Invalidates the box of the node `node` names, or the document's for `None`, as a layout tree update mark on it
    /// asks, and marks the node whose child list its box is built again in. A node without a box has nothing to
    /// invalidate.
    pub(crate) fn apply_layout_tree_update_mark(&mut self, node: Option<StyleNodeID>, mark: FfiLayoutTreeUpdateMark) {
        // NB: A node whose box is built again is an entry of the child list of the box it is laid out in, which the
        //     build plans again around it.
        let builds_box_again =
            mark.reuse_reason == 0 || mark.reuse_reason & layout_tree_update_reuse_reason::RESTYLE != 0;
        let layout_parent = match node {
            Some(node) if builds_box_again => self.layout_parent_with_box(node),
            _ => None,
        };
        // NB: Only the child list of the box a restyled element is laid out in decides whether its box stays, so one that
        //     has none is built again.
        if mark.reuse_reason & layout_tree_update_reuse_reason::RESTYLE != 0 && layout_parent.is_none() {
            self.mark_layout_tree_update(node, FfiLayoutTreeUpdateMark::style_change(0));
            return;
        }
        if let Some(layout_parent) = layout_parent {
            self.mark_layout_tree_update(Some(layout_parent), FfiLayoutTreeUpdateMark::NODE_INSERT);
        }
        let row = match node {
            Some(node) => self.bound_row(node),
            None => self.bound_viewport_row(),
        };
        if row.is_invalid() {
            return;
        }
        // NB: A box whose style the arena derived from its element's takes no other record while the build keeps it, so
        //     a change of its pseudo-elements, which may come with one of its own style, builds it again.
        if mark.reuse_reason
            & (layout_tree_update_reuse_reason::PSEUDO_ELEMENT_CHANGE | layout_tree_update_reuse_reason::RESTYLE)
            != 0
            && node.is_some()
            && self.node_style_record_is_derived(row)
        {
            self.mark_layout_tree_update(node, FfiLayoutTreeUpdateMark::style_change(0));
            return;
        }
        if builds_box_again
            && let Some(node) = node
            && node.element_index().is_some()
        {
            let change = self.generated_content_change_of_style_change(node, row);
            self.layout_tree_update_marks_mut()
                .note_generated_content_change(node, change);
        }
        let classification = self.classify_layout_tree_update(row, mark.is_structural_boundary_self_rebuild);
        if classification.marks_partial_relayout_boundary_self_only {
            self.set_needs_layout_update(row, false);
        } else if mark.is_child_list_insertion {
            // NB: What a child entering or leaving the list invalidates depends on the boxes the build attaches or
            //     drops, which the build's change of the list invalidates.
            self.child_list_changes_await_build = true;
        } else {
            self.set_needs_layout_update(row, true);
        }
    }

    /// What the style the element `node` or one of its `::before`, `::after` and `::marker` pseudo-elements has now does
    /// to the counters, the list item count or the quote depth that the style its box `row`, or the pseudo-element's
    /// box, still has does not.
    fn generated_content_change_of_style_change(&self, node: StyleNodeID, row: NodeSlotId) -> GeneratedContentChange {
        use super::node_data::{GENERATED_FOR_AFTER, GENERATED_FOR_BEFORE, GENERATED_FOR_MARKER, pseudo_kind_of};
        fn change_of(
            old: Option<ComputedValuesView<'_>>,
            new: Option<ComputedValuesView<'_>>,
        ) -> GeneratedContentChange {
            let (counters_or_quotes, list_item) = match (old, new) {
                (None, None) => (false, false),
                (Some(old), Some(new)) => (
                    old.counter_reset() != new.counter_reset()
                        || old.counter_increment() != new.counter_increment()
                        || old.counter_set() != new.counter_set()
                        || old.content_quote_transfer() != new.content_quote_transfer(),
                    old.display().is_list_item() != new.display().is_list_item(),
                ),
                (Some(style), None) | (None, Some(style)) => {
                    (style.affects_generated_content_state(), style.display().is_list_item())
                }
            };
            match (counters_or_quotes, list_item) {
                (true, _) => GeneratedContentChange::CountersOrQuotes,
                (false, true) => GeneratedContentChange::ListItems,
                (false, false) => GeneratedContentChange::None,
            }
        }
        let style_of_row = |row: NodeSlotId| {
            (!row.is_invalid())
                .then(|| crate::layout::node_facts::node_style_view(self.data(row)))
                .flatten()
        };
        self.with_style_store(|engine| {
            // NB: An element or pseudo-element without a box moves no counter and no quote.
            let new_element = engine
                .published_style_view(node, None)
                .filter(|style| !style.display().is_none());
            let mut change = change_of(style_of_row(row), new_element);
            for generated_for in [GENERATED_FOR_BEFORE, GENERATED_FOR_AFTER, GENERATED_FOR_MARKER] {
                if change == GeneratedContentChange::CountersOrQuotes {
                    break;
                }
                let new = new_element
                    .and_then(|_| engine.published_style_view(node, Some(pseudo_kind_of(generated_for))))
                    .filter(|style| {
                        !style.display().is_none()
                            && (generated_for == GENERATED_FOR_MARKER || !style.content_is_keyword())
                    });
                let pseudo_change = change_of(style_of_row(self.bound_pseudo_element_row(node, generated_for)), new);
                if pseudo_change != GeneratedContentChange::None {
                    change = pseudo_change;
                }
            }
            change
        })
    }

    /// The element whose box lays out the box of the node `node` names among its children: the node's flat-tree parent,
    /// or the nearest flat-tree ancestor above `display: contents` elements, if its box is attached. A top layer member
    /// and the document element, whose boxes the viewport lays out, have none, as has a node in a subtree without
    /// boxes.
    pub(crate) fn layout_parent_with_box(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.with_style_store(|engine| {
            if node.element_index().is_some()
                && engine.element_adjustment_facts(node) & element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0
            {
                return None;
            }
            let flat_tree_parent = |node: StyleNodeID| {
                engine.assigned_slot_of(node).or_else(|| match node.text_index() {
                    Some(_) => engine.text_parent(node),
                    None => engine.parent_element_or_host(node),
                })
            };
            let mut parent = flat_tree_parent(node)?;
            loop {
                let row = self.bound_row(parent);
                if !row.is_invalid() {
                    return (!self.data(row).parent.get().is_invalid()).then_some(parent);
                }
                if !engine
                    .element_published_box_facts(parent)
                    .is_some_and(|facts| facts.display.is_contents())
                {
                    return None;
                }
                parent = flat_tree_parent(parent)?;
            }
        })
    }
}

/// Has the render state invalidate the box of the DOM node `style_node` names, or the document's for 0, as the layout
/// tree update `mark` on the node asks, as it applies the host's writes: the classification reads the arena, which the
/// host does not wait for.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_apply_layout_tree_update_mark(
    host: &DocumentHost,
    style_node: u32,
    mark: FfiLayoutTreeUpdateMark,
) {
    use super::layout_changes::{LayoutChange, queue};
    let node = StyleNodeID::from_raw(style_node);
    queue(host, LayoutChange::ApplyLayoutTreeUpdateMark { node, mark });
}

/// What the host marks on a box. A second mark on the box the last queued change marks merges into it, so a burst of
/// marks on one box costs one change.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct FfiBoxMarks {
    /// The box lays out again, and its ancestors with it.
    pub layout_update: bool,
    /// The box paints again, and is hit-tested again where `repaint_hit_testing`. A text box repaints its containing
    /// block.
    pub repaint: bool,
    pub repaint_hit_testing: bool,
    /// The box's paint subtree paints and is hit-tested again.
    pub repaint_subtree: bool,
    /// The paint subtree of the element's `::backdrop` paints and is hit-tested again.
    pub repaint_backdrop: bool,
    /// What the box propagates as text decorations to the text below it is stale.
    pub propagated_text_decorations: bool,
    /// The box takes `dom_paint_facts`, and paints and is hit-tested again.
    pub has_dom_paint_facts: bool,
    pub dom_paint_facts: u8,
    /// The data of the text node changed: its text box renders it again, and lays out again with its ancestors. A box
    /// that renders a range of the text, as the first letter splits it, is built again instead by its `::first-letter`
    /// owner.
    pub text_data_changed: bool,
    /// The image data of the image element changed: its image box drops the intrinsic sizes of itself and its ancestors
    /// where the image's natural size cannot change the box's own, and any other box lays out again with its
    /// ancestors.
    pub image_data_changed: bool,
    /// The text of the box renders with the language it now resolves: where any of it is cased by its language, the box
    /// lays out again with its ancestors.
    pub language_changed: bool,
    /// The box takes `is_editing_host` and, a text box, `produces_line_box_fragment_when_empty`, which the build stamps
    /// it with, and lays out again with its ancestors where either flips: an editing host gains a minimum block size, and
    /// an empty editable text a zero-width fragment.
    pub has_editing_facts: bool,
    pub is_editing_host: bool,
    pub produces_line_box_fragment_when_empty: bool,
}

/// The box a mark names.
#[derive(Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum MarkedBox {
    /// The box bound to the DOM node the identity names, or the document's for `None`, found as the mark is applied: a
    /// mark made beside a frame in flight, which holds the boxes, names the node alone.
    Node(Option<StyleNodeID>),
    /// The box bound to the pseudo-element of kind `generated_for` on the element `generator`.
    PseudoElement { generator: StyleNodeID, generated_for: u8 },
    /// The box of the row, named so by a host that holds the box, or one no node is bound to: an anonymous one, or one of
    /// several built for one node.
    Row(NodeSlotId),
}

impl MarkedBox {
    /// The row of the box, as the arena binds it now.
    pub(crate) fn row(self, arena: &LayoutNodeArena) -> NodeSlotId {
        match self {
            Self::Node(None) => arena.bound_viewport_row(),
            Self::Node(Some(node)) => arena.bound_row(node),
            Self::PseudoElement {
                generator,
                generated_for,
            } => arena.bound_pseudo_element_row(generator, generated_for),
            Self::Row(row) => row,
        }
    }
}

impl FfiBoxMarks {
    /// Folds `later`, marked on the same box, into these marks: each mark covers the narrower one before it, and the
    /// later DOM paint facts replace the earlier ones.
    pub(crate) fn merge(&mut self, later: Self) {
        self.layout_update |= later.layout_update;
        self.repaint |= later.repaint;
        self.repaint_hit_testing |= later.repaint_hit_testing;
        self.repaint_subtree |= later.repaint_subtree;
        self.repaint_backdrop |= later.repaint_backdrop;
        self.propagated_text_decorations |= later.propagated_text_decorations;
        self.text_data_changed |= later.text_data_changed;
        self.image_data_changed |= later.image_data_changed;
        self.language_changed |= later.language_changed;
        if later.has_editing_facts {
            self.has_editing_facts = true;
            self.is_editing_host = later.is_editing_host;
            self.produces_line_box_fragment_when_empty = later.produces_line_box_fragment_when_empty;
        }
        if later.has_dom_paint_facts {
            self.has_dom_paint_facts = true;
            self.dom_paint_facts = later.dom_paint_facts;
        }
    }

    /// Whether the marks may lay the box out again.
    pub(crate) fn may_lay_out(&self) -> bool {
        self.layout_update
            || self.text_data_changed
            || self.image_data_changed
            || self.language_changed
            || self.has_editing_facts
    }

    pub(crate) fn is_empty(&self) -> bool {
        let Self {
            layout_update,
            repaint,
            repaint_hit_testing,
            repaint_subtree,
            repaint_backdrop,
            propagated_text_decorations,
            has_dom_paint_facts,
            dom_paint_facts: _,
            text_data_changed,
            image_data_changed,
            language_changed,
            has_editing_facts,
            is_editing_host: _,
            produces_line_box_fragment_when_empty: _,
        } = *self;
        !(layout_update
            || repaint
            || repaint_hit_testing
            || repaint_subtree
            || repaint_backdrop
            || propagated_text_decorations
            || has_dom_paint_facts
            || text_data_changed
            || image_data_changed
            || language_changed
            || has_editing_facts)
    }

    pub(crate) fn apply(self, arena: &mut LayoutNodeArena, target: MarkedBox) {
        let row = target.row(arena);
        let Some(kind) = arena.node_kind_if_live(row) else {
            return;
        };
        let is_text = super::node_facts::kind_is_text(kind);
        if self.text_data_changed && is_text {
            if !arena.text_has_source_range(row) {
                arena.invalidate_text_content(row);
                arena.set_needs_layout_update(row, true);
            } else if let Some(owner) = arena.first_letter_owner_of_split_text(row) {
                arena.mark_layout_tree_update(Some(owner), FfiLayoutTreeUpdateMark::CHARACTER_DATA_REPLACE_DATA);
            }
        }
        if self.image_data_changed {
            if kind == NodeKind::ImageBox && arena.node_style_if_live(row).is_some_and(size_is_independent_of_image) {
                arena.invalidate_fragments_and_intrinsic_sizes_of_self_and_ancestors(row);
            } else {
                arena.set_needs_layout_update(row, true);
            }
        }
        if self.language_changed && super::rendered_text::enroll_text_after_language_change(arena, row) {
            arena.set_needs_layout_update(row, true);
        }
        if self.has_editing_facts {
            let mut stamp = |flag: NodeFlag, value: bool| {
                let flips = (arena.node_flags(row) & flag as u32 != 0) != value;
                if flips {
                    arena.set_node_flag(row, flag, value);
                }
                flips
            };
            let mut flipped = stamp(NodeFlag::IsEditingHost, self.is_editing_host);
            if is_text {
                flipped |= stamp(
                    NodeFlag::ProducesLineBoxFragmentWhenEmpty,
                    self.produces_line_box_fragment_when_empty,
                );
            }
            if flipped {
                arena.set_needs_layout_update(row, true);
            }
        }
        if self.layout_update {
            arena.set_needs_layout_update(row, true);
        }
        if self.has_dom_paint_facts {
            arena.set_node_dom_paint_facts(row, self.dom_paint_facts);
        }
        if self.propagated_text_decorations && arena.paintable_row_is_populated(row) {
            arena.push_propagated_text_decoration_damage(row);
        }
        if self.repaint_subtree {
            repaint_subtree(arena, row);
        }
        if self.repaint_backdrop
            && let MarkedBox::Node(Some(node)) = target
        {
            let backdrop = arena.bound_pseudo_element_row(node, GENERATED_FOR_BACKDROP);
            repaint_subtree(arena, backdrop);
            repaint(arena, backdrop, true);
        }
        let hit_testing = self.repaint_hit_testing || self.repaint_subtree || self.has_dom_paint_facts;
        if !(self.repaint || hit_testing) {
            return;
        }
        if !is_text {
            repaint(arena, row, hit_testing);
            return;
        }
        if let Some(containing_block) =
            crate::painting::paint_read::PaintRead::node_containing_block_if_live(arena, row)
        {
            repaint(arena, containing_block, hit_testing);
        }
        // The nearest inline box above the text that paints itself caches what it paints of the text.
        if let Some(inline_box) =
            crate::painting::fragment_ownership::nearest_self_painting_inline_box(&arena.paintable_rows(), row)
        {
            arena.push_paint_damage(inline_box, PaintDamage::ALL_DRAW | PaintDamage::ALL_HIT);
        }
    }
}

/// Whether the box's sizes are definite whatever the natural size of its image is.
fn size_is_independent_of_image(style: ComputedValuesView<'_>) -> bool {
    let definite = |size: &ComputedSize| size.is_length_percentage() && !size.contains_percentage();
    let definite_or_none = |size: &ComputedSize| size.is_none() || definite(size);
    definite(style.width())
        && definite(style.height())
        && definite(style.min_width())
        && definite(style.min_height())
        && definite_or_none(style.max_width())
        && definite_or_none(style.max_height())
}

fn repaint_subtree(arena: &mut LayoutNodeArena, row: NodeSlotId) {
    if arena.paintable_row_is_populated(row) {
        arena.push_paint_damage_to_paint_subtree(row, PaintDamage::ALL_PRODUCERS);
    }
}

/// The row paints again, and the root's with it where the root paints the body's propagated background.
pub(crate) fn repaint(arena: &mut LayoutNodeArena, row: NodeSlotId, includes_hit_testing: bool) {
    if !arena.paintable_row_is_populated(row) {
        return;
    }
    let damage = if includes_hit_testing {
        PaintDamage::ALL_PRODUCERS
    } else {
        PaintDamage::ALL_DRAW
    };
    arena.push_paint_damage_for_repaint(row, damage);
    if arena.node_flags(row) & NodeFlag::IsBody as u32 != 0 {
        let source = super::viewport_propagation::root_background_source(arena);
        if source.use_body_background_properties && arena.paintable_row_is_populated(source.root_layout_node) {
            arena.push_paint_damage(source.root_layout_node, PaintDamage::ALL_DRAW | PaintDamage::ALL_HIT);
        }
    }
}

fn queue_marks(host: &DocumentHost, target: MarkedBox, marks: FfiBoxMarks) {
    if marks.is_empty() {
        return;
    }
    super::layout_changes::queue(host, super::layout_changes::LayoutChange::MarkBox { target, marks });
}

/// Marks the box bound to the node with `style_node`, or the document's for 0.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_mark_node_box(host: &DocumentHost, style_node: u32, marks: FfiBoxMarks) {
    let target = MarkedBox::Node(StyleNodeID::from_raw(style_node));
    queue_marks(host, target, marks);
}

/// Marks the box bound to the pseudo-element of kind `generated_for` on the element with `generator`.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_mark_pseudo_element_box(
    host: &DocumentHost,
    generator: u32,
    generated_for: u8,
    marks: FfiBoxMarks,
) {
    let Some(generator) = StyleNodeID::from_raw(generator) else {
        return;
    };
    let target = MarkedBox::PseudoElement {
        generator,
        generated_for,
    };
    queue_marks(host, target, marks);
}

/// Marks the box of the row `row`.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_mark_row_box(host: &DocumentHost, row: NodeSlotId, marks: FfiBoxMarks) {
    let target = MarkedBox::Row(row);
    queue_marks(host, target, marks);
}

#[cfg(test)]
mod tests {
    use super::layout_tree_update_reuse_reason::{CHILD_LIST_INSERTION, PSEUDO_ELEMENT_CHANGE, RESTYLE};
    use super::*;

    #[test]
    fn a_reason_that_forbids_reuse_cannot_be_narrowed_again() {
        let mut marks = LayoutTreeUpdateMarks::default();
        let element = StyleNodeID::element(5);
        assert!(marks.merge(element, true, CHILD_LIST_INSERTION));
        // NB: A reason the node did not hold is a transition the mark site widens from, as every restyle is.
        assert!(marks.merge(element, true, PSEUDO_ELEMENT_CHANGE));
        assert!(!marks.merge(element, true, PSEUDO_ELEMENT_CHANGE));
        assert!(marks.merge(element, true, RESTYLE));
        assert!(marks.merge(element, true, RESTYLE));
        assert_eq!(
            marks.reuse_reasons(element),
            CHILD_LIST_INSERTION | PSEUDO_ELEMENT_CHANGE | RESTYLE
        );
        // NB: The box the marks kept goes now, which what the mark site widens from has to hear of.
        assert!(marks.merge(element, true, 0));
        assert_eq!(marks.reuse_reasons(element), 0);
        assert!(!marks.merge(element, true, 0));
        assert!(!marks.merge(element, true, CHILD_LIST_INSERTION));
        assert_eq!(marks.reuse_reasons(element), 0);
        assert!(marks.needs(element));
    }

    #[test]
    fn marks_made_apart_and_folded_in_are_the_marks_made_in_place() {
        let reasons = [0, CHILD_LIST_INSERTION, PSEUDO_ELEMENT_CHANGE];
        let make_held = |marks: &mut LayoutTreeUpdateMarks, held: usize| {
            if held > 0 {
                marks.merge(StyleNodeID::element(1), true, reasons[held - 1]);
            }
        };
        let make_apart = |marks: &mut LayoutTreeUpdateMarks, first: u8, second: u8| {
            marks.merge(StyleNodeID::element(1), true, first);
            marks.merge(StyleNodeID::element(1), true, second);
            marks.merge(StyleNodeID::text(7), true, 0);
        };
        for held in 0..=reasons.len() {
            for first in reasons {
                for second in reasons {
                    let mut in_place = LayoutTreeUpdateMarks::default();
                    make_held(&mut in_place, held);
                    make_apart(&mut in_place, first, second);

                    let mut apart = LayoutTreeUpdateMarks::default();
                    make_apart(&mut apart, first, second);
                    let mut folded = LayoutTreeUpdateMarks::default();
                    make_held(&mut folded, held);
                    folded.fold(apart);

                    for node in [1, 2, 4, 7]
                        .map(StyleNodeID::element)
                        .into_iter()
                        .chain([StyleNodeID::text(7)])
                    {
                        assert_eq!(
                            folded.get(node),
                            in_place.get(node),
                            "held {held}, apart {first} then {second}"
                        );
                    }
                }
            }
        }
    }
}
