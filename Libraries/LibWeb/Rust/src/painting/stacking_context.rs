/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
use crate::layout::node_facts;
use crate::painting::paintable_rows::PaintableRowsWrite;
use crate::painting::style_queries::{self, effective_z_index};

pub use crate::painting::paintable_data::NO_STACKING_CONTEXT;

pub struct StackingContextNode {
    pub paintable: NodeSlotId,
    pub parent: u32,
    pub children: Vec<u32>,
    pub index_in_tree_order: usize,
    pub effective_z_index: Option<i32>,
    pub positioned_descendants_and_stacking_contexts_with_stack_level_0: Vec<NodeSlotId>,
    pub non_positioned_floating_descendants: Vec<NodeSlotId>,
    pub contains_inline_or_replaced_descendants: bool,
}

#[derive(Default)]
pub struct StackingContextTree {
    pub nodes: Vec<StackingContextNode>,
}

pub(crate) fn build_stacking_context_tree(
    layout_arena: &mut impl PaintableRowsWrite,
    root: NodeSlotId,
) -> StackingContextTree {
    let mut tree = StackingContextTree::default();
    tree.nodes.push(StackingContextNode {
        paintable: root,
        parent: NO_STACKING_CONTEXT,
        children: Vec::new(),
        index_in_tree_order: 0,
        effective_z_index: effective_z_index(layout_arena, root),
        positioned_descendants_and_stacking_contexts_with_stack_level_0: Vec::new(),
        non_positioned_floating_descendants: Vec::new(),
        contains_inline_or_replaced_descendants: false,
    });
    layout_arena.paintable_data_mut(root).stacking_context = 0;

    let mut index_in_tree_order = 1;
    let mut stack: Vec<NodeSlotId> = Vec::new();
    let mut child = crate::painting::paint_order::first_paint_child(layout_arena, root);
    while let Some(first) = child {
        stack.push(first);
        child = None;
        while let Some(current) = stack.pop() {
            visit(layout_arena, &mut tree, current, &mut index_in_tree_order);
            if let Some(next) = crate::painting::paint_order::next_paint_sibling(layout_arena, current) {
                stack.push(next);
            }
            if let Some(first_child) = crate::painting::paint_order::first_paint_child(layout_arena, current) {
                stack.push(first_child);
            }
        }
    }

    sort(&mut tree, 0);
    tree
}

fn visit(
    layout_arena: &mut impl PaintableRowsWrite,
    tree: &mut StackingContextTree,
    paintable: NodeSlotId,
    index_in_tree_order: &mut usize,
) {
    let mut ancestor = crate::painting::paint_order::paint_parent(layout_arena, paintable);
    let mut parent_context = NO_STACKING_CONTEXT;
    while let Some(candidate) = ancestor {
        let context = layout_arena.paintable_data(candidate).stacking_context;
        if context != NO_STACKING_CONTEXT {
            parent_context = context;
            break;
        }
        ancestor = crate::painting::paint_order::paint_parent(layout_arena, candidate);
    }
    // We should always reach the viewport's stacking context.
    assert_ne!(
        parent_context, NO_STACKING_CONTEXT,
        "paintable outside the viewport's stacking context"
    );

    let establishes_stacking_context =
        crate::painting::style_queries::establishes_stacking_context(layout_arena, paintable);
    let z_index = effective_z_index(layout_arena, paintable);
    let positioned = style_queries::is_positioned(layout_arena, paintable);
    let floating = style_queries::is_floating(layout_arena, paintable);
    let inline = style_queries::is_inline(layout_arena, paintable);
    {
        let parent = &mut tree.nodes[parent_context as usize];
        if (positioned || establishes_stacking_context) && z_index.unwrap_or(0) == 0 {
            parent
                .positioned_descendants_and_stacking_contexts_with_stack_level_0
                .push(paintable);
        }
        if !positioned && floating {
            parent.non_positioned_floating_descendants.push(paintable);
        }
        let layout_kind = layout_arena.node_kind_if_live(paintable);
        if !establishes_stacking_context && (inline || layout_kind.is_some_and(node_facts::kind_is_replaced_box)) {
            parent.contains_inline_or_replaced_descendants = true;
        }
    }
    if !establishes_stacking_context {
        layout_arena.paintable_data_mut(paintable).stacking_context = NO_STACKING_CONTEXT;
        return;
    }
    let index = tree.nodes.len() as u32;
    tree.nodes.push(StackingContextNode {
        paintable,
        parent: parent_context,
        children: Vec::new(),
        index_in_tree_order: *index_in_tree_order,
        effective_z_index: z_index,
        positioned_descendants_and_stacking_contexts_with_stack_level_0: Vec::new(),
        non_positioned_floating_descendants: Vec::new(),
        contains_inline_or_replaced_descendants: false,
    });
    *index_in_tree_order += 1;
    tree.nodes[parent_context as usize].children.push(index);
    layout_arena.paintable_data_mut(paintable).stacking_context = index;
}

fn sort(tree: &mut StackingContextTree, index: u32) {
    let mut children = std::mem::take(&mut tree.nodes[index as usize].children);
    children.sort_by_key(|child| {
        let node = &tree.nodes[*child as usize];
        (node.effective_z_index.unwrap_or(0), node.index_in_tree_order)
    });
    for child in &children {
        sort(tree, *child);
    }
    tree.nodes[index as usize].children = children;
}
