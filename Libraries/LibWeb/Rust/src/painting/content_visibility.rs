/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::content_visibility;
use crate::layout::node_data::NodeSlotId;
use crate::painting::node_painting;
use crate::painting::paintable_rows::PaintableRowsRead;

pub(crate) fn for_each_box_with_auto_content_visibility(
    arena: &impl PaintableRowsRead,
    root: NodeSlotId,
    mut push_box: impl FnMut(NodeSlotId),
) {
    if !arena.slot_is_live(root) {
        return;
    }
    arena.for_each_node_in_layout_subtree_in_pre_order_with_pruning(root, |node| {
        if node_painting::forms_unconnected_subtree(arena.data(node).kind.get()) {
            return false;
        }
        if !arena.node_dom_node_is_element(node) || !arena.paintable_row_is_populated(node) {
            return true;
        }
        let content_visibility_is_auto = arena
            .node_style_if_live(node)
            .is_some_and(|style| style.content_visibility() == content_visibility::AUTO);
        if content_visibility_is_auto {
            push_box(node);
        }
        true
    });
}
