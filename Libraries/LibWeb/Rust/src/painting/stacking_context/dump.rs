/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::host::FfiStackingContextDumpEntry;

pub(crate) fn for_each_stacking_context_in_dump_order(
    arena: &LayoutNodeArena,
    viewport: NodeSlotId,
    emit: &mut impl FnMut(FfiStackingContextDumpEntry),
) {
    if arena.stacking_context_entries(viewport).is_none() {
        return;
    }
    visit(arena, viewport, 0, emit);
}

fn visit(arena: &LayoutNodeArena, root: NodeSlotId, depth: usize, emit: &mut impl FnMut(FfiStackingContextDumpEntry)) {
    let effective_z_index = arena
        .paintable_visual_context_record(root)
        .and_then(|record| record.stacking_context.effective_z_index);
    emit(FfiStackingContextDumpEntry {
        layout_node_shell: arena.shell_if_live(root),
        depth,
        has_effective_z_index: effective_z_index.is_some(),
        effective_z_index: effective_z_index.unwrap_or(0),
    });
    let Some(entries) = arena.stacking_context_entries(root) else {
        return;
    };
    for entry in entries.negative_z_index_child_contexts() {
        visit(arena, entry.slot, depth + 1, emit);
    }
    for &descendant in &entries.stack_level_zero_boxes {
        if arena.paintable_row_is_populated(descendant)
            && arena
                .paintable_rows()
                .paintable_data(descendant)
                .establishes_stacking_context
        {
            visit(arena, descendant, depth + 1, emit);
        }
    }
    for entry in entries.positive_z_index_child_contexts() {
        visit(arena, entry.slot, depth + 1, emit);
    }
}
