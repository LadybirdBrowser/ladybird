/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::StackingContextFacts;
use crate::cow_column::{CowColumn, RowMut};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::paint_order;
use crate::painting::paint_read::GeometryRead;
use crate::painting::paintable_rows::PAINTABLE_SLOTS_PER_CHUNK;
use crate::painting::visual_context::dirty::VisualContextBoxDirtyKind;
use std::cell::Ref;
use std::ops::{Deref, DerefMut};
use std::sync::Arc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct NonzeroZIndexChildContext {
    pub z_index: i32,
    pub slot: NodeSlotId,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct StackingContextEntries {
    pub child_contexts_with_nonzero_z_index: Vec<NonzeroZIndexChildContext>,
    pub stack_level_zero_boxes: Vec<NodeSlotId>,
    pub non_positioned_float_count: u32,
    pub inline_or_replaced_count: u32,
    pub needs_resort: bool,
}

impl StackingContextEntries {
    pub(crate) fn negative_z_index_child_contexts(&self) -> &[NonzeroZIndexChildContext] {
        let first_positive = self
            .child_contexts_with_nonzero_z_index
            .partition_point(|entry| entry.z_index < 0);
        &self.child_contexts_with_nonzero_z_index[..first_positive]
    }

    pub(crate) fn positive_z_index_child_contexts(&self) -> &[NonzeroZIndexChildContext] {
        let first_positive = self
            .child_contexts_with_nonzero_z_index
            .partition_point(|entry| entry.z_index < 0);
        &self.child_contexts_with_nonzero_z_index[first_positive..]
    }
}

/// Each stacking context root's entry table, by row. It is published with the rows, so a write
/// copies a table, and its chunk, only while a published frame shares them.
pub(crate) type StackingContextEntryColumn = CowColumn<Option<Arc<StackingContextEntries>>, PAINTABLE_SLOTS_PER_CHUNK>;

/// A root's table, for writing. It copies the table, and its row's chunk, only if a published
/// frame shares them and the table changes.
struct TableMut<'a>(
    RowMut<&'a mut StackingContextEntryColumn, Option<Arc<StackingContextEntries>>, PAINTABLE_SLOTS_PER_CHUNK>,
);

impl Deref for TableMut<'_> {
    type Target = StackingContextEntries;

    fn deref(&self) -> &StackingContextEntries {
        self.0.as_deref().expect("a table row holds a table")
    }
}

impl DerefMut for TableMut<'_> {
    fn deref_mut(&mut self) -> &mut StackingContextEntries {
        Arc::make_mut(self.0.as_mut().expect("a table row holds a table"))
    }
}

fn table_mut(tables: &mut StackingContextEntryColumn, root: NodeSlotId) -> Option<TableMut<'_>> {
    let index = root.slot_index() as usize;
    tables.get(index)?.as_ref()?;
    tables.row_mut(index).map(TableMut)
}

/// Drops the table of the root in a row, if it has one.
pub(crate) fn drop_table(tables: &mut StackingContextEntryColumn, index: usize) {
    tables.set(index, None);
}

impl LayoutNodeArena {
    pub(crate) fn stacking_context_entries(&self, root: NodeSlotId) -> Option<Ref<'_, StackingContextEntries>> {
        if !self.paintable_row_is_populated(root) {
            return None;
        }
        Ref::filter_map(self.paintable_rows.stacking_context_entries.borrow(), |tables| {
            tables
                .get(root.slot_index() as usize)
                .and_then(|table| table.as_deref())
        })
        .ok()
    }

    pub(crate) fn ensure_stacking_context_entries_for_root(&self, root: NodeSlotId) {
        debug_assert!(self.paintable_row_is_populated(root));
        let mut tables = self.paintable_rows.stacking_context_entries.borrow_mut();
        let index = root.slot_index() as usize;
        if tables.get(index).is_some_and(Option::is_none) {
            tables
                .set(index, Some(Arc::default()))
                .expect("the row is in the column");
        }
    }

    pub(crate) fn drop_stacking_context_entries_of_root(&self, root: NodeSlotId) {
        drop_table(
            &mut self.paintable_rows.stacking_context_entries.borrow_mut(),
            root.slot_index() as usize,
        );
    }

    pub(crate) fn register_stacking_context_contribution(&self, slot: NodeSlotId, facts: &StackingContextFacts) {
        let enclosing = facts.enclosing_stacking_context;
        if !self.paintable_row_is_populated(enclosing) {
            return;
        }
        let mut tables = self.paintable_rows.stacking_context_entries.borrow_mut();
        let Some(mut table) = table_mut(&mut tables, enclosing) else {
            return;
        };
        let mut composition_changed = false;
        if let Some(z_index) = facts.nonzero_z_index_child_contribution() {
            let label = self.node_pre_order_label(slot);
            let position = table
                .child_contexts_with_nonzero_z_index
                .partition_point(|entry| (entry.z_index, self.node_pre_order_label(entry.slot)) < (z_index, label));
            table
                .child_contexts_with_nonzero_z_index
                .insert(position, NonzeroZIndexChildContext { z_index, slot });
            composition_changed = true;
        } else if facts.contributes_to_stack_level_zero() {
            let label = self.node_pre_order_label(slot);
            let position = table
                .stack_level_zero_boxes
                .partition_point(|entry| self.node_pre_order_label(*entry) < label);
            table.stack_level_zero_boxes.insert(position, slot);
            composition_changed = true;
        }
        if facts.contributes_non_positioned_float() {
            composition_changed |= table.non_positioned_float_count == 0;
            table.non_positioned_float_count += 1;
        }
        if facts.contributes_inline_or_replaced() {
            composition_changed |= table.inline_or_replaced_count == 0;
            table.inline_or_replaced_count += 1;
        }
        drop(table);
        drop(tables);
        if composition_changed {
            self.note_stacking_context_composition_changed(enclosing);
        }
    }

    pub(crate) fn withdraw_stacking_context_contribution(&self, slot: NodeSlotId, facts: &StackingContextFacts) {
        let enclosing = facts.enclosing_stacking_context;
        if !self.paintable_row_is_populated(enclosing) {
            return;
        }
        let mut tables = self.paintable_rows.stacking_context_entries.borrow_mut();
        let Some(mut table) = table_mut(&mut tables, enclosing) else {
            return;
        };
        let mut composition_changed = false;
        if facts.nonzero_z_index_child_contribution().is_some() {
            if let Some(position) = table
                .child_contexts_with_nonzero_z_index
                .iter()
                .position(|entry| entry.slot == slot)
            {
                table.child_contexts_with_nonzero_z_index.remove(position);
                composition_changed = true;
            }
        } else if facts.contributes_to_stack_level_zero()
            && let Some(position) = table.stack_level_zero_boxes.iter().position(|entry| *entry == slot)
        {
            table.stack_level_zero_boxes.remove(position);
            composition_changed = true;
        }
        if facts.contributes_non_positioned_float() {
            composition_changed |= table.non_positioned_float_count == 1;
            table.non_positioned_float_count = table.non_positioned_float_count.saturating_sub(1);
        }
        if facts.contributes_inline_or_replaced() {
            composition_changed |= table.inline_or_replaced_count == 1;
            table.inline_or_replaced_count = table.inline_or_replaced_count.saturating_sub(1);
        }
        drop(table);
        drop(tables);
        if composition_changed {
            self.note_stacking_context_composition_changed(enclosing);
        }
    }

    fn flag_stacking_context_entries_for_resort(&self, root: NodeSlotId) {
        if !self.paintable_row_is_populated(root) {
            return;
        }
        self.note_stacking_context_composition_changed(root);
        let mut tables = self.paintable_rows.stacking_context_entries.borrow_mut();
        let Some(mut table) = table_mut(&mut tables, root) else {
            return;
        };
        if table.needs_resort {
            return;
        }
        table.needs_resort = true;
        self.paintable_rows
            .stacking_context_roots_flagged_for_resort
            .borrow_mut()
            .push(root);
    }

    pub(crate) fn apply_stacking_context_facts_delta(
        &self,
        slot: NodeSlotId,
        previous: Option<&StackingContextFacts>,
        next: &mut StackingContextFacts,
        was_reattached: bool,
    ) {
        if let Some(previous) = previous {
            if previous.contribution_is_registered && previous.same_contributions_as(next) && !was_reattached {
                next.contribution_is_registered = true;
                return;
            }
            if previous.paints_inline_content_itself() != next.paints_inline_content_itself()
                && let Some(line_root) = self.inline_pieces_root(slot)
            {
                self.note_line_root_needs_fragment_ownership(line_root);
            }
            if previous.contribution_is_registered {
                self.withdraw_stacking_context_contribution(slot, previous);
            }
            if previous.establishes_stacking_context && !next.establishes_stacking_context {
                self.drop_stacking_context_entries_of_root(slot);
            }
        }
        if next.establishes_stacking_context {
            self.ensure_stacking_context_entries_for_root(slot);
        }
        self.register_stacking_context_contribution(slot, next);
        next.contribution_is_registered = true;
        if was_reattached {
            if let Some(previous) = previous {
                self.flag_stacking_context_entries_for_resort(previous.enclosing_stacking_context);
            }
            self.flag_stacking_context_entries_for_resort(next.enclosing_stacking_context);
        }
    }

    pub(crate) fn resort_stacking_context_entries_flagged_for_resort(&self) {
        let flagged_roots = std::mem::take(
            &mut *self
                .paintable_rows
                .stacking_context_roots_flagged_for_resort
                .borrow_mut(),
        );
        for root in flagged_roots {
            if !self.paintable_row_is_populated(root) {
                continue;
            }
            let mut tables = self.paintable_rows.stacking_context_entries.borrow_mut();
            let Some(mut table) = table_mut(&mut tables, root) else {
                continue;
            };
            table.needs_resort = false;
            table
                .child_contexts_with_nonzero_z_index
                .sort_by_key(|entry| (entry.z_index, self.node_pre_order_label(entry.slot)));
            table
                .stack_level_zero_boxes
                .sort_by_key(|entry| self.node_pre_order_label(*entry));
        }
    }

    pub(crate) fn rebuild_all_stacking_context_entries_from_records(&self, viewport: NodeSlotId) {
        {
            let mut tables = self.paintable_rows.stacking_context_entries.borrow_mut();
            for index in 0..self.paintable_row_count() {
                drop_table(&mut tables, index);
            }
        }
        self.paintable_rows
            .stacking_context_roots_flagged_for_resort
            .borrow_mut()
            .clear();
        let paintable_rows = self.paintable_rows();
        let mut roots_in_visit_order: Vec<NodeSlotId> = Vec::new();
        paint_order::for_each_in_paint_subtree(&paintable_rows, viewport, |slot| {
            let Some(record) = self.paintable_visual_context_record(slot) else {
                return;
            };
            let facts = record.stacking_context;
            drop(record);
            if facts.establishes_stacking_context {
                self.ensure_stacking_context_entries_for_root(slot);
                roots_in_visit_order.push(slot);
            }
            let mut tables = self.paintable_rows.stacking_context_entries.borrow_mut();
            if let Some(mut table) = self
                .paintable_row_is_populated(facts.enclosing_stacking_context)
                .then(|| table_mut(&mut tables, facts.enclosing_stacking_context))
                .flatten()
            {
                if let Some(z_index) = facts.nonzero_z_index_child_contribution() {
                    table
                        .child_contexts_with_nonzero_z_index
                        .push(NonzeroZIndexChildContext { z_index, slot });
                } else if facts.contributes_to_stack_level_zero() {
                    table.stack_level_zero_boxes.push(slot);
                }
                if facts.contributes_non_positioned_float() {
                    table.non_positioned_float_count += 1;
                }
                if facts.contributes_inline_or_replaced() {
                    table.inline_or_replaced_count += 1;
                }
            }
            drop(tables);
            self.set_paintable_record_stacking_context_contribution_registered(slot);
        });
        let mut tables = self.paintable_rows.stacking_context_entries.borrow_mut();
        for root in roots_in_visit_order {
            if let Some(mut table) = table_mut(&mut tables, root) {
                table
                    .child_contexts_with_nonzero_z_index
                    .sort_by_key(|entry| entry.z_index);
            }
        }
    }

    pub(crate) fn withdraw_stacking_context_state_of_reset_row(
        &self,
        slot: NodeSlotId,
        facts: Option<StackingContextFacts>,
    ) {
        if let Some(facts) = facts
            && facts.contribution_is_registered
        {
            self.withdraw_stacking_context_contribution(slot, &facts);
        }
        self.drop_stacking_context_entries_of_root(slot);
    }

    pub(crate) fn note_layout_subtree_attached(&self, subtree_root: NodeSlotId) {
        if self.paintable_row_count() == 0 {
            return;
        }
        // The new parent lists the subtree from now on, and every row inside plans its own
        // descendants under the new ancestry.
        self.push_enclosing_paint_order_damage(subtree_root);
        self.for_each_node_in_layout_subtree_in_pre_order(subtree_root, |node| {
            if self.paintable_row_is_populated(node) {
                self.note_overflow_contained_box_added(node);
                self.note_visual_context_box_dirty(node, VisualContextBoxDirtyKind::ReattachedInLayoutTree);
                self.push_paint_damage(node, crate::painting::record::damage::PaintDamage::ORDER);
            }
        });
    }
}
