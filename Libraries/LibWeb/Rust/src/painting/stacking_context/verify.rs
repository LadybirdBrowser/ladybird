/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::StackingContextFacts;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::node_painting;
use crate::painting::paint_order;
use std::collections::HashMap;
use std::fmt::Write;

const MAXIMUM_REPORT_LINE_COUNT: usize = 32;

struct Report {
    text: String,
    line_count: usize,
}

impl Report {
    fn note(&mut self, line: std::fmt::Arguments<'_>) {
        if self.line_count == MAXIMUM_REPORT_LINE_COUNT {
            self.text.push_str("(report capped)\n");
        }
        self.line_count += 1;
        if self.line_count > MAXIMUM_REPORT_LINE_COUNT {
            return;
        }
        let _ = writeln!(self.text, "{line}");
    }
}

#[derive(Default)]
struct ExpectedEntries {
    child_contexts_with_nonzero_z_index: Vec<(i32, NodeSlotId)>,
    stack_level_zero_boxes: Vec<NodeSlotId>,
    non_positioned_float_count: u32,
    inline_or_replaced_count: u32,
}

struct Verifier<'a> {
    arena: &'a LayoutNodeArena,
    expected_entries_by_root: HashMap<NodeSlotId, ExpectedEntries>,
    expected_roots: Vec<NodeSlotId>,
    previous_label: Option<u64>,
    report: Report,
}

impl Verifier<'_> {
    fn visit(&mut self, slot: NodeSlotId, enclosing: NodeSlotId) {
        let fresh = StackingContextFacts::gather(self.arena, slot, enclosing);
        let label = self.arena.node_pre_order_label(slot);
        if self.previous_label.is_some_and(|previous| label <= previous) {
            self.report.note(format_args!(
                "pre-order label of {slot:?} is not greater than the previous label"
            ));
        }
        self.previous_label = Some(label);

        match self.arena.paintable_visual_context_record(slot) {
            Some(record) => {
                let stored = record.stacking_context;
                drop(record);
                let mut fresh_with_registered_contribution = fresh;
                fresh_with_registered_contribution.contribution_is_registered = true;
                if stored != fresh_with_registered_contribution {
                    self.report.note(format_args!(
                        "facts of {slot:?} diverge: stored {stored:?}, fresh {fresh_with_registered_contribution:?}"
                    ));
                }
            }
            None => self.report.note(format_args!("{slot:?} has no visual context record")),
        }
        let paintable_rows = self.arena.paintable_rows();
        if paintable_rows.paintable_data(slot).establishes_stacking_context != fresh.establishes_stacking_context {
            self.report.note(format_args!(
                "PaintableData.establishes_stacking_context of {slot:?} diverges from a fresh query"
            ));
        }

        if let Some(z_index) = fresh.nonzero_z_index_child_contribution() {
            self.expected_entries_by_root
                .entry(enclosing)
                .or_default()
                .child_contexts_with_nonzero_z_index
                .push((z_index, slot));
        } else if fresh.contributes_to_stack_level_zero() {
            self.expected_entries_by_root
                .entry(enclosing)
                .or_default()
                .stack_level_zero_boxes
                .push(slot);
        }
        if fresh.contributes_non_positioned_float() {
            self.expected_entries_by_root
                .entry(enclosing)
                .or_default()
                .non_positioned_float_count += 1;
        }
        if fresh.contributes_inline_or_replaced() {
            self.expected_entries_by_root
                .entry(enclosing)
                .or_default()
                .inline_or_replaced_count += 1;
        }
        if fresh.establishes_stacking_context {
            self.expected_roots.push(slot);
            self.expected_entries_by_root.entry(slot).or_default();
        }

        let enclosing_for_children = if fresh.establishes_stacking_context {
            slot
        } else {
            enclosing
        };
        let mut children = Vec::new();
        paint_order::for_each_paint_child(&self.arena.paintable_rows(), slot, |child| children.push(child));
        for child in children {
            self.visit(child, enclosing_for_children);
        }

        let paintable_rows = self.arena.paintable_rows();
        if node_painting::has_lines(&paintable_rows, slot)
            && !self.arena.paintable_side_data(slot).inline_box_pieces().is_empty()
        {
            for (owner, recomputed_filter) in
                crate::painting::fragment_ownership::compute_fragment_ownership_for_block(&paintable_rows, slot)
            {
                let stored_filter = self.arena.paintable_side_data(owner).fragment_ownership.clone();
                if stored_filter.as_ref() != Some(&recomputed_filter) {
                    self.report.note(format_args!(
                        "fragment ownership of {owner:?} under line root {slot:?} diverges from a fresh computation"
                    ));
                }
            }
        }
    }

    fn compare_tables(&mut self) {
        for root in std::mem::take(&mut self.expected_roots) {
            let expected = self
                .expected_entries_by_root
                .get_mut(&root)
                .expect("every expected root has an expected entry set");
            expected
                .child_contexts_with_nonzero_z_index
                .sort_by_key(|(z_index, _)| *z_index);
            let Some(table) = self.arena.stacking_context_entries(root) else {
                self.report
                    .note(format_args!("establishing box {root:?} has no entries table"));
                continue;
            };
            let stored_nonzero: Vec<(i32, NodeSlotId)> = table
                .child_contexts_with_nonzero_z_index
                .iter()
                .map(|entry| (entry.z_index, entry.slot))
                .collect();
            if stored_nonzero != expected.child_contexts_with_nonzero_z_index {
                self.report.note(format_args!(
                    "nonzero z-index child contexts of {root:?} diverge: stored {stored_nonzero:?}, expected {:?}",
                    expected.child_contexts_with_nonzero_z_index
                ));
            }
            if table.stack_level_zero_boxes != expected.stack_level_zero_boxes {
                self.report.note(format_args!(
                    "stack level zero boxes of {root:?} diverge: stored {:?}, expected {:?}",
                    table.stack_level_zero_boxes, expected.stack_level_zero_boxes
                ));
            }
            if table.non_positioned_float_count != expected.non_positioned_float_count {
                self.report.note(format_args!(
                    "float count of {root:?} diverges: stored {}, expected {}",
                    table.non_positioned_float_count, expected.non_positioned_float_count
                ));
            }
            if table.inline_or_replaced_count != expected.inline_or_replaced_count {
                self.report.note(format_args!(
                    "inline or replaced count of {root:?} diverges: stored {}, expected {}",
                    table.inline_or_replaced_count, expected.inline_or_replaced_count
                ));
            }
            if table.needs_resort {
                self.report
                    .note(format_args!("entries table of {root:?} is still flagged for a resort"));
            }
        }
        let stored_table_count = self
            .arena
            .paintable_rows
            .stacking_context_entries
            .borrow()
            .iter()
            .filter(|table| table.is_some())
            .count();
        let expected_table_count = self
            .expected_entries_by_root
            .keys()
            .filter(|root| !root.is_invalid())
            .count();
        if stored_table_count != expected_table_count {
            self.report.note(format_args!(
                "{stored_table_count} entries tables are stored but {expected_table_count} establishing boxes exist"
            ));
        }
    }
}

pub(crate) fn verification_report(arena: &LayoutNodeArena, viewport: NodeSlotId) -> String {
    if !arena.paintable_row_is_populated(viewport) {
        return String::new();
    }
    let mut verifier = Verifier {
        arena,
        expected_entries_by_root: HashMap::new(),
        expected_roots: Vec::new(),
        previous_label: None,
        report: Report {
            text: String::new(),
            line_count: 0,
        },
    };
    verifier.visit(viewport, NodeSlotId::INVALID);
    verifier.compare_tables();
    verifier.report.text
}
