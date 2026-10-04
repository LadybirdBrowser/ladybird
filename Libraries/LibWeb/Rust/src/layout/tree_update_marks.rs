/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The layout tree update marks: what the DOM asks the next layout tree build to rebuild. A mark is
//! written where the DOM changes and read by the next build, which retires it. The layout arena
//! holds them, keyed by the style node identity the build walks by.

use super::layout_changes::LayoutChange;
use crate::css::style::engine_calls::document_host;
use crate::css::style::tree::StyleNodeID;
use crate::render_state::{ArenaChange, ArenaRead, DocumentHost, ask};

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
}

impl LayoutTreeUpdateMarkWrite {
    /// Makes the write to `marks`, answering what the write it makes answers.
    pub(crate) fn apply(self, marks: &mut LayoutTreeUpdateMarks) -> bool {
        match self {
            Self::Merge(node, value, reuse_reason) => marks.merge(node, value, reuse_reason),
            Self::SetChildNeeds(node, value) => marks.set_child_needs(node, value),
        }
    }
}

/// Answers `read` of the layout tree update marks of `host`'s document for the node `style_node` names. An identity of
/// 0 names no node, and `read` is not run for it. Beside a frame in flight, which holds the document's marks, the host
/// answers from the marks it made beside the frame: a mark it made before the frame flew reads as unset there, which
/// at most makes a mark reach further than it had to. With no frame in flight, the host reads the document's marks
/// where they are, between the render state's jobs.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
unsafe fn read_marks<R: Default>(
    host: *const DocumentHost,
    style_node: u32,
    read: fn(&LayoutTreeUpdateMarks, StyleNodeID) -> R,
) -> R {
    let Some(style_node) = StyleNodeID::from_raw(style_node) else {
        return R::default();
    };
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    match host.marks_beside_flight() {
        Ok(marks) => read(&marks, style_node),
        Err(here) => ask(
            here,
            host,
            ArenaRead::new((style_node, read), |arena, (style_node, read)| {
                read(&arena.layout_tree_update_marks().borrow(), style_node)
            }),
        ),
    }
}

/// Makes `write` to the layout tree update marks of `host`'s document, answering what it answers. Beside a frame in
/// flight, the host answers from the marks it made beside the frame, as [`read_marks`] does, and queues the write for
/// the document's marks behind its other writes.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
unsafe fn write_marks(host: *const DocumentHost, write: LayoutTreeUpdateMarkWrite) -> bool {
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    match host.marks_beside_flight() {
        Ok(mut marks) => {
            let answer = write.apply(&mut marks);
            drop(marks);
            host.queue_change(ArenaChange::Layout(LayoutChange::LayoutTreeUpdateMark(write)));
            answer
        }
        Err(here) => ask(
            here,
            host,
            ArenaRead::new(write, |arena, write| {
                write.apply(&mut arena.layout_tree_update_marks().borrow_mut())
            }),
        ),
    }
}

/// Whether the node `style_node` names holds a layout tree update mark of its own.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_needs_layout_tree_update(host: *const DocumentHost, style_node: u32) -> bool {
    // SAFETY: Guaranteed by the caller.
    unsafe { read_marks(host, style_node, LayoutTreeUpdateMarks::needs) }
}

/// Which narrower rebuilds the marks the node `style_node` names has collected still permit.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_layout_tree_update_reuse_reasons(
    host: *const DocumentHost,
    style_node: u32,
) -> u8 {
    // SAFETY: Guaranteed by the caller.
    unsafe { read_marks(host, style_node, LayoutTreeUpdateMarks::reuse_reasons) }
}

/// Whether a flat-tree descendant of the node `style_node` names holds a layout tree update mark.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_child_needs_layout_tree_update(
    host: *const DocumentHost,
    style_node: u32,
) -> bool {
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
    host: *const DocumentHost,
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
    host: *const DocumentHost,
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
/// whatever the node that held it before left behind. The write is queued, as nothing the host asks waits on it, and
/// made to the marks the host made beside a frame in flight as well.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_clear_layout_tree_update_marks(host: *const DocumentHost, style_node: u32) {
    let Some(style_node) = StyleNodeID::from_raw(style_node) else {
        return;
    };
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { document_host(host) };
    if let Ok(mut marks) = host.marks_beside_flight() {
        marks.clear(style_node);
    }
    host.queue_change(ArenaChange::Layout(LayoutChange::ClearLayoutTreeUpdateMarks(
        style_node,
    )));
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
