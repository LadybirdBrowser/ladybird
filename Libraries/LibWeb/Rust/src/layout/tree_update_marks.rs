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
use super::node_data::{GENERATED_FOR_BACKDROP, GENERATED_FOR_FIRST_LETTER, NodeFlag, NodeKind, NodeSlotId};
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
    pub(super) const ALL: u8 = CHILD_LIST_INSERTION | PSEUDO_ELEMENT_CHANGE;
}

/// The build has to rebuild what the node produces.
const NEEDS: u8 = 1 << 2;
/// A flat-tree descendant holds a mark: the chain the build climbs down to reach a node it has to
/// rebuild. Only an element, a shadow root and the document are ever on it.
const CHILD_NEEDS: u8 = 1 << 3;

/// One byte of marks per identity, in the element and the text index spaces apart. The low bits
/// are the reuse reasons, so no reason ever needs translating.
#[derive(Default)]
pub(crate) struct LayoutTreeUpdateMarks {
    elements: Vec<u8>,
    text: Vec<u8>,
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

    /// Whether the layout tree build has to rebuild what this node produces.
    pub(crate) fn needs(&self, node: StyleNodeID) -> bool {
        self.get(node) & NEEDS != 0
    }

    /// Which narrower rebuilds the marks collected on this node still permit. See
    /// [`layout_tree_update_reuse_reason`].
    pub(crate) fn reuse_reasons(&self, node: StyleNodeID) -> u8 {
        self.get(node) & layout_tree_update_reuse_reason::ALL
    }

    /// Whether a flat-tree descendant holds a mark. A text node is never on the chain the mark
    /// climbs, so it answers no.
    pub(crate) fn child_needs(&self, node: StyleNodeID) -> bool {
        node.text_index().is_none() && self.get(node) & CHILD_NEEDS != 0
    }

    /// Fold one mark into the node's, answering whether its own bit changed. That answer is what
    /// tells the mark site it has a transition to widen from. Once a reason that forbids reuse
    /// arrives, a later one cannot narrow it back.
    pub(crate) fn merge(&mut self, node: StyleNodeID, value: bool, reuse_reason: u8) -> bool {
        let reuse_reason = reuse_reason & layout_tree_update_reuse_reason::ALL;
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
                return rest | (marks & NEEDS) | merged;
            }
            changed = true;
            rest | if value { NEEDS } else { 0 } | reuse_reason
        });
        changed
    }

    /// Record whether a flat-tree descendant holds a mark, answering what was recorded before. The
    /// mark's ancestor walk stops where the answer is already yes.
    pub(crate) fn set_child_needs(&mut self, node: StyleNodeID, value: bool) -> bool {
        if node.text_index().is_some() {
            return false;
        }
        let before = self.child_needs(node);
        self.update(node, |marks| {
            if value {
                marks | CHILD_NEEDS
            } else {
                marks & !CHILD_NEEDS
            }
        });
        before
    }

    /// Retire the marks the node holds, own and child alike: the build has just answered them, or
    /// the identity has been handed to another node.
    pub(crate) fn clear(&mut self, node: StyleNodeID) {
        self.update(node, |_| 0);
    }
}

/// A write of one node's layout tree update marks.
#[derive(Clone, Copy)]
pub(crate) enum LayoutTreeUpdateMarkWrite {
    /// See [`LayoutTreeUpdateMarks::merge`].
    Merge(StyleNodeID, bool, u8),
    /// See [`LayoutTreeUpdateMarks::set_child_needs`].
    SetChildNeeds(StyleNodeID, bool),
    /// See [`LayoutTreeUpdateMarks::clear`].
    Clear(StyleNodeID),
}

impl LayoutTreeUpdateMarkWrite {
    /// Makes the write to `marks`, answering what the write it makes answers.
    pub(crate) fn apply(self, marks: &mut LayoutTreeUpdateMarks) -> bool {
        match self {
            Self::Merge(node, value, reuse_reason) => marks.merge(node, value, reuse_reason),
            Self::SetChildNeeds(node, value) => marks.set_child_needs(node, value),
            Self::Clear(node) => {
                marks.clear(node);
                false
            }
        }
    }
}

/// Answers `read` of the layout tree update marks of `host`'s document for the node `style_node` names, which the host
/// holds (see [`DocumentHost::read_marks`]). An identity of 0 names no node, and `read` is not run for it.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
unsafe fn read_marks<R: Default>(
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
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
unsafe fn write_marks(host: &DocumentHost, write: LayoutTreeUpdateMarkWrite) -> bool {
    host.write_marks(write)
}

/// Whether the node `style_node` names holds a layout tree update mark of its own.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_needs_layout_tree_update(host: &DocumentHost, style_node: u32) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe { read_marks(host, style_node, LayoutTreeUpdateMarks::needs) }
}

/// Which narrower rebuilds the marks the node `style_node` names has collected still permit.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_layout_tree_update_reuse_reasons(host: &DocumentHost, style_node: u32) -> u8 {
    // SAFETY: Guaranteed by the caller.
    unsafe { read_marks(host, style_node, LayoutTreeUpdateMarks::reuse_reasons) }
}

/// Whether a flat-tree descendant of the node `style_node` names holds a layout tree update mark.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_child_needs_layout_tree_update(host: &DocumentHost, style_node: u32) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe { read_marks(host, style_node, LayoutTreeUpdateMarks::child_needs) }
}

/// Folds a layout tree update mark into the one the node `style_node` names holds, answering
/// whether its own bit changed: the transition the mark site widens the rebuild from.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_merge_layout_tree_update_mark(
    host: &DocumentHost,
    style_node: u32,
    value: bool,
    reuse_reason: u8,
) -> bool {
    let Some(style_node) = StyleNodeID::from_raw(style_node) else {
        return false;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { write_marks(host, LayoutTreeUpdateMarkWrite::Merge(style_node, value, reuse_reason)) }
}

/// Records whether a flat-tree descendant of the node `style_node` names holds a layout tree update
/// mark, answering what was recorded before. The mark's ancestor walk stops where it was already.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_set_child_needs_layout_tree_update(
    host: &DocumentHost,
    style_node: u32,
    value: bool,
) -> bool {
    let Some(style_node) = StyleNodeID::from_raw(style_node) else {
        return false;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { write_marks(host, LayoutTreeUpdateMarkWrite::SetChildNeeds(style_node, value)) }
}

/// Retires the layout tree update marks the node `style_node` names holds. An identity handed to a node holds none,
/// whatever the node that held it before left behind.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_clear_layout_tree_update_marks(host: &DocumentHost, style_node: u32) {
    let Some(style_node) = StyleNodeID::from_raw(style_node) else {
        return;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { write_marks(host, LayoutTreeUpdateMarkWrite::Clear(style_node)) };
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
    /// What a node removal marks its parent with.
    pub(crate) const NODE_REMOVE: Self = Self {
        reuse_reason: 0,
        is_child_list_insertion: false,
        is_structural_boundary_self_rebuild: true,
    };

    /// What a node insertion marks the node and its parent with.
    pub(crate) const NODE_INSERT: Self = Self {
        reuse_reason: layout_tree_update_reuse_reason::CHILD_LIST_INSERTION,
        is_child_list_insertion: true,
        is_structural_boundary_self_rebuild: true,
    };
}

impl LayoutNodeArena {
    /// Marks the node `node` names, or the document for `None`, for the next layout tree build to rebuild, as the DOM
    /// side does where a node changes, for a node that has a box: the marks of its flat-tree ancestors lead the build
    /// to it, and its box is invalidated as [`Self::apply_layout_tree_update_mark`] does. What only the DOM side knows
    /// of, the resources an SVG element lends its references and a display: contents element's parent, is not for a
    /// node with a box.
    pub(crate) fn mark_layout_tree_update(&self, node: Option<StyleNodeID>, mark: FfiLayoutTreeUpdateMark) {
        let document = self.document_style_node();
        let Some(style_node) = node.or(document) else {
            return;
        };
        if mark.is_child_list_insertion
            && let Some(element) = node
            && let Some(owner) = self.first_letter_owner_covering(element)
            && owner != element
        {
            self.mark_layout_tree_update(Some(owner), mark);
        }
        if !self
            .layout_tree_update_marks()
            .borrow_mut()
            .merge(style_node, true, mark.reuse_reason)
        {
            return;
        }

        // The build reaches a top layer member from the document rather than through its ancestors.
        let is_in_top_layer = |node: StyleNodeID| {
            self.with_style_store(|engine| {
                engine.element_adjustment_facts(node) & element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0
            })
        };
        let mut is_inside_top_layer_member = node.is_some_and(&is_in_top_layer);
        let mut current = node;
        while let Some(child) = current {
            current = self.flat_tree_parent(child);
            // The document element's flat-tree parent is the document.
            let Some(parent) = current.or(document) else {
                break;
            };
            is_inside_top_layer_member |= current.is_some_and(&is_in_top_layer);
            if self
                .layout_tree_update_marks()
                .borrow_mut()
                .set_child_needs(parent, true)
            {
                break;
            }
        }
        if is_inside_top_layer_member && let Some(document) = document {
            self.layout_tree_update_marks()
                .borrow_mut()
                .set_child_needs(document, true);
        }

        self.apply_layout_tree_update_mark(node, mark);
    }

    /// The writes that mark `node`, which is in no top layer, for the next layout tree build in marks the arena does
    /// not hold, as [`Self::mark_layout_tree_update`] marks it in its own: the host's, which the render clock leaves
    /// the host to make in its own once it has them back.
    pub(crate) fn layout_tree_update_mark_writes(
        &self,
        node: StyleNodeID,
        reuse_reason: u8,
    ) -> impl Iterator<Item = LayoutTreeUpdateMarkWrite> + '_ {
        let ancestors = std::iter::successors(self.flat_tree_parent(node), |&ancestor| self.flat_tree_parent(ancestor))
            .chain(self.document_style_node());
        std::iter::once(LayoutTreeUpdateMarkWrite::Merge(node, true, reuse_reason))
            .chain(ancestors.map(|ancestor| LayoutTreeUpdateMarkWrite::SetChildNeeds(ancestor, true)))
    }

    /// Invalidates the box of the node `node` names, or the document's for `None`, as a layout tree update mark on it
    /// asks, and marks the node a rebuild of it escalates to. A node without a box has nothing to invalidate.
    pub(crate) fn apply_layout_tree_update_mark(&self, node: Option<StyleNodeID>, mark: FfiLayoutTreeUpdateMark) {
        let row = match node {
            Some(node) => self.bound_row(node),
            None => self.bound_viewport_row(),
        };
        if row.is_invalid() {
            return;
        }
        let classification = self.classify_layout_tree_update(row, mark.is_structural_boundary_self_rebuild);
        if classification.marks_partial_relayout_boundary_self_only {
            self.set_needs_layout_update(row, false);
        } else if mark.is_child_list_insertion {
            // What an insertion invalidates depends on the boxes it attaches, which only the layout tree build knows.
            self.defer_child_list_insertion_layout_update(row);
        } else {
            self.set_needs_layout_update(row, true);
        }
        // FIXME: Escalating a rebuild past anonymous parents is not optimal, and we should figure out how to rebuild a
        //        smaller part of the tree.
        if let Some(target) = classification.escalation_target {
            self.mark_layout_tree_update(StyleNodeID::from_raw(target), mark);
        }
    }

    /// The `::first-letter` owner on or above the element `element` whose first letter its box holds, or that has no
    /// first-letter box, whose rebuild an insertion under `element` reaches. See
    /// `DOM::Node::first_letter_owner_for_layout_subtree_from`.
    fn first_letter_owner_covering(&self, element: StyleNodeID) -> Option<StyleNodeID> {
        let layout_node = self.bound_row(element);
        self.with_style_store(|engine| {
            let mut ancestor = Some(element);
            while let Some(current) = ancestor {
                if engine.has_published_first_letter_style(current) {
                    if layout_node.is_invalid() {
                        return None;
                    }
                    let first_letter = self.bound_pseudo_element_row(current, GENERATED_FOR_FIRST_LETTER);
                    let mut box_ancestor = first_letter;
                    while !box_ancestor.is_invalid() && box_ancestor != layout_node {
                        box_ancestor = self.data(box_ancestor).parent.get();
                    }
                    if first_letter.is_invalid() || !box_ancestor.is_invalid() {
                        return Some(current);
                    }
                }
                ancestor = engine
                    .tree()
                    .parent(current)
                    .map(|parent| engine.tree().host_of(parent).unwrap_or(parent));
            }
            None
        })
    }
}

/// Has the render state invalidate the box of the DOM node `style_node` names, or the document's for 0, as the layout
/// tree update `mark` on the node asks, as it applies the host's writes: the classification reads the arena, which the
/// host does not wait for.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_apply_layout_tree_update_mark(
    host: &DocumentHost,
    style_node: u32,
    mark: FfiLayoutTreeUpdateMark,
) {
    use super::layout_changes::{LayoutChange, queue};
    let node = StyleNodeID::from_raw(style_node);
    // SAFETY: Guaranteed by the caller.
    unsafe { queue(host, LayoutChange::ApplyLayoutTreeUpdateMark { node, mark }) };
}

/// What the host marks on a box. A second mark on the box the last queued change marks merges into it, so a burst of
/// marks on one box costs one change.
#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct FfiBoxMarks {
    /// The box lays out again, and its ancestors with it where `layout_update_through_ancestors`.
    pub layout_update: bool,
    pub layout_update_through_ancestors: bool,
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
    /// that renders a range of the text, as the first letter splits it, is built again instead, as the host marked.
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
#[derive(Clone, Copy, PartialEq, Eq)]
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
        self.layout_update_through_ancestors |= later.layout_update_through_ancestors;
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

    pub(crate) fn apply(self, arena: &mut LayoutNodeArena, target: MarkedBox) {
        let row = target.row(arena);
        let Some(kind) = arena.node_kind_if_live(row) else {
            return;
        };
        let is_text = super::node_facts::kind_is_text(kind);
        if self.text_data_changed && is_text && !arena.text_has_source_range(row) {
            arena.invalidate_text_content(row);
            arena.set_needs_layout_update(row, true);
        }
        if self.image_data_changed {
            if kind == NodeKind::ImageBox && arena.node_style_if_live(row).is_some_and(size_is_independent_of_image) {
                arena.bump_fragment_cache_epoch_of_self_and_ancestors(row);
                arena.reset_cached_intrinsic_sizes_of_self_and_ancestors(row);
            } else {
                arena.set_needs_layout_update(row, true);
            }
        }
        if self.language_changed && super::rendered_text::enroll_text_after_language_change(arena, row) {
            arena.set_needs_layout_update(row, true);
        }
        if self.has_editing_facts {
            let stamp = |flag: NodeFlag, value: bool| {
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
            arena.set_needs_layout_update(row, self.layout_update_through_ancestors);
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
fn repaint(arena: &mut LayoutNodeArena, row: NodeSlotId, includes_hit_testing: bool) {
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

/// Marks the box bound to the node with `style_node`, or the document's for 0.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_mark_node_box(host: &DocumentHost, style_node: u32, marks: FfiBoxMarks) {
    let target = MarkedBox::Node(StyleNodeID::from_raw(style_node));
    // SAFETY: Guaranteed by the caller.
    unsafe { super::layout_changes::queue(host, super::layout_changes::LayoutChange::MarkBox { target, marks }) };
}

/// Marks the box bound to the pseudo-element of kind `generated_for` on the element with `generator`.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_mark_pseudo_element_box(
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
    // SAFETY: Guaranteed by the caller.
    unsafe { super::layout_changes::queue(host, super::layout_changes::LayoutChange::MarkBox { target, marks }) };
}

/// Marks the box of the row `row`.
///
/// # Safety
///
/// `host` must be a live document host, on the document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_mark_row_box(host: &DocumentHost, row: NodeSlotId, marks: FfiBoxMarks) {
    let target = MarkedBox::Row(row);
    // SAFETY: Guaranteed by the caller.
    unsafe { super::layout_changes::queue(host, super::layout_changes::LayoutChange::MarkBox { target, marks }) };
}

#[cfg(test)]
mod tests {
    use super::layout_tree_update_reuse_reason::{CHILD_LIST_INSERTION, PSEUDO_ELEMENT_CHANGE};
    use super::*;

    #[test]
    fn a_reason_that_forbids_reuse_cannot_be_narrowed_again() {
        let mut marks = LayoutTreeUpdateMarks::default();
        let element = StyleNodeID::element(5);
        assert!(marks.merge(element, true, CHILD_LIST_INSERTION));
        assert!(!marks.merge(element, true, PSEUDO_ELEMENT_CHANGE));
        assert_eq!(
            marks.reuse_reasons(element),
            CHILD_LIST_INSERTION | PSEUDO_ELEMENT_CHANGE
        );
        assert!(!marks.merge(element, true, 0));
        assert_eq!(marks.reuse_reasons(element), 0);
        assert!(!marks.merge(element, true, CHILD_LIST_INSERTION));
        assert_eq!(marks.reuse_reasons(element), 0);
        assert!(marks.needs(element));
    }

    #[test]
    fn the_child_mark_answers_what_it_was_and_only_elements_hold_one() {
        let mut marks = LayoutTreeUpdateMarks::default();
        let element = StyleNodeID::element(3);
        let text = StyleNodeID::text(3);
        assert!(!marks.set_child_needs(element, true));
        assert!(marks.set_child_needs(element, true));
        assert!(marks.child_needs(element));
        assert!(!marks.set_child_needs(text, true));
        assert!(!marks.child_needs(text));
        assert!(marks.merge(text, true, 0));
        assert!(marks.needs(text) && !marks.needs(element));
        marks.clear(element);
        assert!(!marks.child_needs(element));
        assert!(marks.needs(text));
    }
}
