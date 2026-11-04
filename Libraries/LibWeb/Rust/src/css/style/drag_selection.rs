/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a clock lane's drag selection reads of the DOM: the places of nodes in tree order, and what their `user-select`
//! allows, from the nodes the style engine mirrors, which is all the Render thread has of the document. Comments and
//! processing instructions have no place among them, and never have a box to highlight.
//!
//! A style update keeps the `::selection` styles of the elements the host's selection covers alone, so a row a drag
//! reaches first has none. A lane styles those in its fork, as the host does once it covers them.

use super::StyleEngine;
use super::tree::StyleNodeID;
use crate::css::computed_value_views::ComputedValuesView;
use crate::layout::node_data::DomPaintFact;
use crate::painting::hit_test::caret::NodeScopes;
use crate::painting::host::FfiNodeIdentity;
use smallvec::SmallVec;
use std::collections::HashSet;

/// The used value of `user-select`, as `DOM::Node::user_select_used_value` decides it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum UsedUserSelect {
    Text,
    None,
    All,
    Contain,
}

impl StyleEngine {
    /// The parent of `node`, as `DOM::Node::parent` answers it: a shadow root has none.
    pub(crate) fn drag_parent(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        match node.text_index() {
            Some(_) => self.retained.text_parent(node),
            None => self.retained.dom_parent(node),
        }
    }

    /// `node` and its ancestors, up to the root of its tree.
    fn drag_ancestry(&self, node: StyleNodeID) -> impl Iterator<Item = StyleNodeID> + '_ {
        std::iter::successors(Some(node), |&node| self.drag_parent(node))
    }

    /// The root of the tree `node` is in.
    pub(crate) fn drag_root(&self, node: StyleNodeID) -> StyleNodeID {
        self.drag_ancestry(node).last().unwrap_or(node)
    }

    /// Whether `ancestor` is `node` or an ancestor of it.
    pub(crate) fn drag_is_inclusive_ancestor(&self, ancestor: StyleNodeID, node: StyleNodeID) -> bool {
        self.drag_ancestry(node).any(|node| node == ancestor)
    }

    /// Whether `a` comes before `b` in the tree order of the tree they share.
    pub(crate) fn drag_precedes(&self, a: StyleNodeID, b: StyleNodeID) -> bool {
        let chain = |node| self.drag_ancestry(node).collect::<SmallVec<[StyleNodeID; 32]>>();
        let (a_chain, b_chain) = (chain(a), chain(b));
        // Walk both from the root end to where they part. An ancestor comes before what it holds.
        let shared = a_chain
            .iter()
            .rev()
            .zip(b_chain.iter().rev())
            .take_while(|(a, b)| a == b)
            .count();
        if shared == 0 || shared == b_chain.len() {
            return false;
        }
        if shared == a_chain.len() {
            return true;
        }
        let (left, right) = (a_chain[a_chain.len() - shared - 1], b_chain[b_chain.len() - shared - 1]);
        std::iter::successors(self.retained.next_sibling_in_dom_order(left), |&sibling| {
            self.retained.next_sibling_in_dom_order(sibling)
        })
        .any(|sibling| sibling == right)
    }

    /// The first child of `node`, in the tree it is in: a shadow tree is another tree.
    fn drag_first_child(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.retained.dom_children(node).next()
    }

    /// Whether `node` has children.
    pub(crate) fn drag_has_children(&self, node: StyleNodeID) -> bool {
        self.drag_first_child(node).is_some()
    }

    /// The node right after `node`, which then is the last node of nothing it holds.
    pub(crate) fn drag_next_sibling(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        self.retained.next_sibling_in_dom_order(node)
    }

    /// The node after `node` and all it holds, in tree order, within `stay_within`.
    pub(crate) fn drag_next_after_subtree(&self, node: StyleNodeID, stay_within: StyleNodeID) -> Option<StyleNodeID> {
        self.drag_ancestry(node)
            .take_while(|&node| node != stay_within)
            .find_map(|node| self.retained.next_sibling_in_dom_order(node))
    }

    /// The node after `node` in tree order, within `stay_within`, as `TreeNode::next_in_pre_order` answers it.
    pub(crate) fn drag_next_in_pre_order_within(
        &self,
        node: StyleNodeID,
        stay_within: StyleNodeID,
    ) -> Option<StyleNodeID> {
        self.drag_first_child(node)
            .or_else(|| self.drag_next_after_subtree(node, stay_within))
    }

    /// Whether `node` is inert.
    pub(crate) fn drag_is_inert(&self, node: StyleNodeID) -> bool {
        self.tree().dom_paint_facts(node) & DomPaintFact::Inert as u8 != 0
    }

    /// The element whose `user-select` decides for `node`: `node` itself, or the one a text node takes its style from.
    pub(crate) fn drag_style_element(&self, node: StyleNodeID) -> Option<StyleNodeID> {
        match node.text_index() {
            Some(_) => self.retained.inheritance_parent(node),
            None => Some(node),
        }
    }

    /// Whether `node` is an element with `display: contents`, which generates no box.
    pub(crate) fn drag_has_no_box_of_its_own(&self, node: StyleNodeID) -> bool {
        node.element_index().is_some()
            && self
                .computed_group_sets
                .assigned_style_record(node)
                .and_then(|record| self.computed_group_sets.style_record_view(record.raw()))
                .is_some_and(|view| ComputedValuesView::of(view.payloads).display().is_contents())
    }

    /// The used value of `user-select` on `element`, as `DOM::Node::user_select_used_value` decides it. An editing host
    /// or a mutable form control contains the selection, whatever its style says.
    ///
    /// NB: The mirror tells an editing host from the elements in it by nothing, and a form control not at all, so every
    ///     editable element answers `Contain`, and a text control that is not editable answers as its style says. A
    ///     drag that reaches either stays in a text control's user agent shadow tree, or is the host's by the answer.
    pub(crate) fn drag_used_user_select(&self, element: StyleNodeID) -> UsedUserSelect {
        use crate::css::css_enums::user_select;
        let mut node = element;
        loop {
            if self.tree().dom_paint_facts(node) & DomPaintFact::EditableOrEditingHost as u8 != 0 {
                return match node == element {
                    true => UsedUserSelect::Contain,
                    false => UsedUserSelect::Text,
                };
            }
            let view = self
                .computed_group_sets
                .assigned_style_record(node)
                .and_then(|record| self.computed_group_sets.style_record_view(record.raw()));
            let Some(view) = view else {
                return UsedUserSelect::None;
            };
            let computed = ComputedValuesView::of(view.payloads).misc_interaction().user_select;
            match computed {
                user_select::NONE => return UsedUserSelect::None,
                user_select::ALL => return UsedUserSelect::All,
                user_select::CONTAIN if node == element => return UsedUserSelect::Contain,
                user_select::AUTO => {}
                _ => return UsedUserSelect::Text,
            }
            match self.retained.inheritance_parent(node) {
                Some(parent) if parent.element_index().is_some() && !self.retained.is_relation_only(parent) => {
                    node = parent;
                }
                _ => return UsedUserSelect::Text,
            }
        }
    }
}

/// Which elements a drag's highlight still owes their `::selection` styles.
pub(crate) struct SelectionStyles {
    /// Whether a rule of the document styles `::selection`, without which the rows show the default colors as they are.
    document_styles_selection: bool,
    /// The elements whose rows hold their `::selection` styles.
    styled: HashSet<StyleNodeID>,
}

impl SelectionStyles {
    /// What the frame of the document `engine` holds owes.
    pub(crate) fn of(engine: &StyleEngine) -> Self {
        Self {
            document_styles_selection: engine
                .retained
                .live_style
                .as_deref()
                .is_none_or(|live| live.styles_selection()),
            styled: HashSet::new(),
        }
    }

    /// Whether the rows of `element` have yet to get its `::selection` styles.
    pub(crate) fn owes(&self, element: StyleNodeID) -> bool {
        self.document_styles_selection && !self.styled.contains(&element)
    }

    /// Notes that the rows of `element` hold its `::selection` styles.
    pub(crate) fn note_styled(&mut self, element: StyleNodeID) {
        self.styled.insert(element);
    }
}

/// What `HitTestList`'s caret queries read of the DOM on the Render thread: the mirror, where the host asks the DOM.
pub(crate) struct MirrorNodeScopes<'a>(pub(crate) &'a StyleEngine);

impl NodeScopes for MirrorNodeScopes<'_> {
    fn contains(&self, ancestor: FfiNodeIdentity, node: FfiNodeIdentity) -> bool {
        let identity = |identity: FfiNodeIdentity| StyleNodeID::from_raw(identity.style_node);
        match (ancestor.is_document, node.is_document) {
            (_, true) => ancestor.is_document,
            // A shadow tree's nodes are not the document's descendants.
            (true, false) => identity(node).is_none_or(|node| {
                let root = self.0.drag_root(node);
                self.0.retained.host_of(root).is_none()
            }),
            (false, false) => match (identity(ancestor), identity(node)) {
                (Some(ancestor), Some(node)) => self.0.drag_is_inclusive_ancestor(ancestor, node),
                _ => false,
            },
        }
    }
}
