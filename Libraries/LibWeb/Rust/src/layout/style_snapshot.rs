/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Layout outputs retained for the next style stage: what a container query asks of a container's
//! box. A layout commit gathers its rows in the arena and hands them to the style engine together
//! once it completes, so no style evaluation reads a partly committed set.

use crate::css::style::tree::StyleNodeID;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::layout::used_values::FfiCssPixelSize;

/// What the last layout commit and scroll state say of one element's box, for the container
/// queries that ask about it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct LayoutStyleSnapshotRow {
    pub(crate) content_width_raw: i32,
    pub(crate) content_height_raw: i32,
    /// Whether the element has a box at all; a size query on one without is unknown.
    pub(crate) has_committed_box: bool,
    pub(crate) stuck: u8,
    pub(crate) snapped: u8,
    pub(crate) scrollable: u8,
    pub(crate) scrolled: u8,
}

/// The box geometry one commit gathers for one element.
#[derive(Clone, Copy)]
pub(crate) struct CommittedGeometry {
    pub(crate) node: StyleNodeID,
    pub(crate) content_width_raw: i32,
    pub(crate) content_height_raw: i32,
    pub(crate) has_committed_box: bool,
}

impl LayoutNodeArena {
    /// The style node a node's row is for, and whether the node's style makes it a size container;
    /// `None` for a node whose box the style engine takes no row of.
    fn snapshot_target(&self, node: NodeSlotId) -> Option<(StyleNodeID, bool)> {
        let style_node = self
            .node_style_node(node)
            .filter(|style_node| style_node.element_index().is_some())?;
        if !self.has_style_engine() || self.bound_row(style_node) != node {
            return None;
        }
        let style = crate::layout::node_facts::node_style_view(self.data(node));
        let is_size_container = style.is_some_and(|style| {
            let box_values = style.box_values();
            box_values.is_size_container || box_values.is_inline_size_container
        });
        Some((style_node, is_size_container))
    }

    fn committed_geometry(&self, node: NodeSlotId, style_node: StyleNodeID) -> CommittedGeometry {
        let rows = self.paintable_rows();
        let has_committed_box = rows.paintable_row_is_populated(node);
        let size = if has_committed_box {
            crate::painting::paintable_geometry::committed_content_size(&rows, node)
        } else {
            FfiCssPixelSize::default()
        };
        CommittedGeometry {
            node: style_node,
            content_width_raw: size.width.raw_value(),
            content_height_raw: size.height.raw_value(),
            has_committed_box,
        }
    }

    /// Gathers the node's row for the style engine. Style asks layout only about size containers,
    /// and about scroll-state containers, whose rows the scroll state makes; an element that
    /// stops being one keeps its row up to date until it goes.
    pub(crate) fn gather_layout_style_snapshot_geometry(&self, node: NodeSlotId) {
        let Some((style_node, is_size_container)) = self.snapshot_target(node) else {
            return;
        };
        if !is_size_container && !self.with_style_store(|engine| engine.layout_style_snapshot(style_node).is_some()) {
            return;
        }
        let row = self.committed_geometry(node, style_node);
        self.layout_style_snapshot_commit.borrow_mut().push(row);
    }

    /// Hands the style engine the committed box of a node whose new style makes it a size
    /// container, which the last commit gathered no row for. Until the next commit, a size query
    /// on it reads that box, as the host's evaluation reads the box it holds.
    pub(crate) fn publish_new_size_container_geometry(&self, node: NodeSlotId) {
        let Some((style_node, true)) = self.snapshot_target(node) else {
            return;
        };
        if self.with_style_store(|engine| engine.layout_style_snapshot(style_node).is_some()) {
            return;
        }
        let row = self.committed_geometry(node, style_node);
        if row.has_committed_box {
            self.with_style_engine(|engine| engine.apply_layout_style_snapshot_commit(&[row]));
        }
    }

    /// Gathers a row without a box for a node that holds a row and lost its bound box: no commit
    /// reaches a node without one, and a size query on it is unknown.
    pub(crate) fn gather_layout_style_snapshot_box_loss(&self, style_node: StyleNodeID) {
        if !self.has_style_engine()
            || !self.bound_row(style_node).is_invalid()
            || !self.with_style_store(|engine| engine.layout_style_snapshot(style_node).is_some())
        {
            return;
        }
        self.layout_style_snapshot_commit.borrow_mut().push(CommittedGeometry {
            node: style_node,
            content_width_raw: 0,
            content_height_raw: 0,
            has_committed_box: false,
        });
    }

    /// Hands the rows a completed commit gathered to the style engine.
    pub(crate) fn publish_layout_style_snapshot_commit(&self) {
        let rows = std::mem::take(&mut *self.layout_style_snapshot_commit.borrow_mut());
        if rows.is_empty() {
            return;
        }
        self.with_style_engine(|engine| engine.apply_layout_style_snapshot_commit(&rows));
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::style::StyleEngine;

    #[test]
    fn a_commit_keeps_the_scroll_state_and_the_scroll_state_keeps_the_geometry() {
        let mut engine = StyleEngine::new();
        let node = StyleNodeID::element(1);
        engine.set_element_scroll_state(node, 1, 2, 4, 8);
        engine.apply_layout_style_snapshot_commit(&[CommittedGeometry {
            node,
            content_width_raw: 11,
            content_height_raw: 13,
            has_committed_box: true,
        }]);
        assert_eq!(
            engine.layout_style_snapshot(node),
            Some(LayoutStyleSnapshotRow {
                content_width_raw: 11,
                content_height_raw: 13,
                has_committed_box: true,
                stuck: 1,
                snapped: 2,
                scrollable: 4,
                scrolled: 8,
            })
        );
        engine.set_element_scroll_state(node, 0, 0, 0, 0);
        let row = engine.layout_style_snapshot(node).unwrap();
        assert!(row.has_committed_box);
        assert_eq!((row.content_width_raw, row.stuck), (11, 0));
    }
}
