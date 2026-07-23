/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::abort_on_panic;
use std::ffi::c_void;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiReplacedElementDisplayAdjustment {
    None,
    Inline,
    Block,
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_adjusted_table_display_for_replaced_element(
    is_table_inside: bool,
    is_block_outside: bool,
    is_internal_table: bool,
    is_table_caption: bool,
) -> FfiReplacedElementDisplayAdjustment {
    abort_on_panic(|| {
        // https://drafts.csswg.org/css-display-3/#outer-role
        // Note: Outer display types do affect replaced elements.
        if is_table_inside {
            if is_block_outside {
                return FfiReplacedElementDisplayAdjustment::Block;
            }
            return FfiReplacedElementDisplayAdjustment::Inline;
        }

        // https://drafts.csswg.org/css-display-3/#layout-specific-display
        // When the 'display' property of a replaced element computes to one of the layout-internal values, it is
        // handled as having a used value of 'display: inline'.
        if is_internal_table || is_table_caption {
            return FfiReplacedElementDisplayAdjustment::Inline;
        }
        FfiReplacedElementDisplayAdjustment::None
    })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
// NB: Some variants are only constructed by C++ through the FFI.
#[allow(dead_code)]
pub enum FfiTableDisplay {
    Other,
    TableRoot,
    TableRowGroup,
    TableHeaderGroup,
    TableFooterGroup,
    TableColumnGroup,
    TableColumn,
    TableRow,
    TableCell,
    TableCaption,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLayoutNodeFacts {
    pub current_display: FfiTableDisplay,
    pub display_before_box_type_transformation: FfiTableDisplay,
    pub is_box: bool,
    pub has_style: bool,
    pub is_anonymous: bool,
    pub is_block_container: bool,
    pub children_are_inline: bool,
    pub is_out_of_flow: bool,
    pub is_text: bool,
    pub text_is_ascii_whitespace: bool,
    pub is_inline_outside: bool,
    pub is_table_wrapper: bool,
    pub has_been_wrapped_in_table_wrapper: bool,
    pub has_replaced_element_table_display_adjustment: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiAnonymousTableBoxKind {
    TableRow,
    TableCell,
    Table,
    InlineTable,
}

#[repr(C)]
pub struct FfiTreeBuilderCallbacks {
    pub context: *mut c_void,
    pub layout_node_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiLayoutNodeFacts,
    pub parent: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub first_child: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub next_sibling: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub previous_sibling: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub remove_nodes: unsafe extern "C" fn(*mut c_void, *const *mut c_void, usize),
    pub wrap_in_anonymous:
        unsafe extern "C" fn(*mut c_void, *const *mut c_void, usize, *mut c_void, FfiAnonymousTableBoxKind),
    pub update_existing_table_wrapper: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    pub wrap_table_root: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void),
    pub create_table_grid: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub destroy_table_grid: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub table_grid_column_count: unsafe extern "C" fn(*mut c_void, *mut c_void) -> usize,
    pub table_grid_is_occupied: unsafe extern "C" fn(*mut c_void, *mut c_void, usize, usize) -> bool,
    pub append_missing_table_cell: unsafe extern "C" fn(*mut c_void, *mut c_void),
}

type LayoutNode = *mut c_void;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum TraversalDecision {
    Continue,
    SkipChildrenAndContinue,
    Break,
}

struct TreeBuilderHost<'a> {
    callbacks: &'a FfiTreeBuilderCallbacks,
}

impl TreeBuilderHost<'_> {
    fn facts(&self, node: LayoutNode) -> FfiLayoutNodeFacts {
        assert!(!node.is_null());
        // SAFETY: The entry point's callback contract guarantees that every node passed to a callback is live.
        unsafe { (self.callbacks.layout_node_facts)(self.callbacks.context, node) }
    }

    fn parent(&self, node: LayoutNode) -> LayoutNode {
        // SAFETY: The entry point's callback contract guarantees that `node` is live.
        unsafe { (self.callbacks.parent)(self.callbacks.context, node) }
    }

    fn first_child(&self, node: LayoutNode) -> LayoutNode {
        // SAFETY: The entry point's callback contract guarantees that `node` is live.
        unsafe { (self.callbacks.first_child)(self.callbacks.context, node) }
    }

    fn next_sibling(&self, node: LayoutNode) -> LayoutNode {
        // SAFETY: The entry point's callback contract guarantees that `node` is live.
        unsafe { (self.callbacks.next_sibling)(self.callbacks.context, node) }
    }

    fn previous_sibling(&self, node: LayoutNode) -> LayoutNode {
        // SAFETY: The entry point's callback contract guarantees that `node` is live.
        unsafe { (self.callbacks.previous_sibling)(self.callbacks.context, node) }
    }

    fn for_each_in_inclusive_subtree(
        &self,
        root: LayoutNode,
        mut callback: impl FnMut(LayoutNode) -> TraversalDecision,
    ) {
        let mut current = root;
        while !current.is_null() {
            let decision = callback(current);
            if decision == TraversalDecision::Break {
                return;
            }

            if decision != TraversalDecision::SkipChildrenAndContinue {
                let first_child = self.first_child(current);
                if !first_child.is_null() {
                    current = first_child;
                    continue;
                }
            }
            if current == root {
                break;
            }

            let next_sibling = self.next_sibling(current);
            if !next_sibling.is_null() {
                current = next_sibling;
                continue;
            }

            while current != root && self.next_sibling(current).is_null() {
                current = self.parent(current);
            }
            if current == root {
                break;
            }

            current = self.next_sibling(current);
        }
    }

    fn remove_nodes(&self, nodes: &[LayoutNode]) {
        // SAFETY: All nodes remain tree-owned until the callback first retains the complete slice.
        unsafe { (self.callbacks.remove_nodes)(self.callbacks.context, nodes.as_ptr(), nodes.len()) };
    }

    fn wrap_in_anonymous(&self, nodes: &[LayoutNode], nearest_sibling: LayoutNode, kind: FfiAnonymousTableBoxKind) {
        assert!(!nodes.is_empty());
        // SAFETY: The nodes are live siblings and `nearest_sibling` is either null or their live next sibling.
        unsafe {
            (self.callbacks.wrap_in_anonymous)(
                self.callbacks.context,
                nodes.as_ptr(),
                nodes.len(),
                nearest_sibling,
                kind,
            );
        };
    }
}

fn is_table_track(display: FfiTableDisplay) -> bool {
    matches!(display, FfiTableDisplay::TableRow | FfiTableDisplay::TableColumn)
}

fn is_table_track_group(display: FfiTableDisplay) -> bool {
    // Unless explicitly mentioned otherwise, mentions of table-row-groups in this spec also encompass the specialized
    // table-header-groups and table-footer-groups.
    matches!(
        display,
        FfiTableDisplay::TableRowGroup
            | FfiTableDisplay::TableHeaderGroup
            | FfiTableDisplay::TableFooterGroup
            | FfiTableDisplay::TableColumnGroup
    )
}

fn display_for_table_fixup(facts: FfiLayoutNodeFacts) -> FfiTableDisplay {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // For the purposes of these rules, out-of-flow elements are represented as inline elements of zero width and
    // height. Their containing blocks are chosen accordingly.
    //
    // AD-HOC: Table-internal boxes can be blockified before fixup. Use the pre-transformation display for authored
    // boxes so an out-of-flow table-header-group is still recognized as a proper table child during fixup.
    if facts.has_replaced_element_table_display_adjustment || facts.is_anonymous {
        facts.current_display
    } else {
        facts.display_before_box_type_transformation
    }
}

fn is_proper_table_child(facts: FfiLayoutNodeFacts) -> bool {
    let display = display_for_table_fixup(facts);
    is_table_track_group(display) || is_table_track(display) || display == FfiTableDisplay::TableCaption
}

fn is_table_non_root_box(facts: FfiLayoutNodeFacts) -> bool {
    matches!(
        facts.current_display,
        FfiTableDisplay::TableRow
            | FfiTableDisplay::TableColumn
            | FfiTableDisplay::TableRowGroup
            | FfiTableDisplay::TableHeaderGroup
            | FfiTableDisplay::TableFooterGroup
            | FfiTableDisplay::TableColumnGroup
            | FfiTableDisplay::TableCell
            | FfiTableDisplay::TableCaption
    )
}

fn is_tabular_container(facts: FfiLayoutNodeFacts) -> bool {
    // https://drafts.csswg.org/css-tables-3/#tabular-container
    matches!(
        facts.current_display,
        FfiTableDisplay::TableRoot
            | FfiTableDisplay::TableRow
            | FfiTableDisplay::TableRowGroup
            | FfiTableDisplay::TableHeaderGroup
            | FfiTableDisplay::TableFooterGroup
    )
}

fn is_ignorable_whitespace(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    let facts = host.facts(node);
    if facts.is_text && facts.text_is_ascii_whitespace {
        return true;
    }

    if facts.is_anonymous && facts.is_block_container && facts.children_are_inline {
        let mut contains_only_whitespace = true;
        host.for_each_in_inclusive_subtree(node, |descendant| {
            let descendant_facts = host.facts(descendant);
            if descendant_facts.is_text {
                if !descendant_facts.text_is_ascii_whitespace {
                    contains_only_whitespace = false;
                    return TraversalDecision::Break;
                }
            } else if descendant_facts.is_out_of_flow || !descendant_facts.is_anonymous {
                contains_only_whitespace = false;
                return TraversalDecision::Break;
            }
            TraversalDecision::Continue
        });
        return contains_only_whitespace;
    }

    false
}

fn is_first_or_last_child_with_table_non_root_sibling_if_any(host: &TreeBuilderHost<'_>, node: LayoutNode) -> bool {
    let previous_sibling = host.previous_sibling(node);
    let next_sibling = host.next_sibling(node);
    if !previous_sibling.is_null() && !next_sibling.is_null() {
        return false;
    }
    if !previous_sibling.is_null() && !is_table_non_root_box(host.facts(previous_sibling)) {
        return false;
    }
    if !next_sibling.is_null() && !is_table_non_root_box(host.facts(next_sibling)) {
        return false;
    }
    true
}

fn for_each_sequence_of_consecutive_children_matching(
    host: &TreeBuilderHost<'_>,
    parent: LayoutNode,
    matcher: impl Fn(FfiLayoutNodeFacts) -> bool,
    mut callback: impl FnMut(&[LayoutNode], LayoutNode),
) {
    let mut sequence = Vec::new();
    let mut child = host.first_child(parent);
    while !child.is_null() {
        if matcher(host.facts(child)) || (!sequence.is_empty() && is_ignorable_whitespace(host, child)) {
            sequence.push(child);
        } else if !sequence.is_empty() {
            if !sequence.iter().all(|&node| is_ignorable_whitespace(host, node)) {
                callback(&sequence, child);
            }
            sequence.clear();
        }
        child = host.next_sibling(child);
    }
    if !sequence.is_empty() && !sequence.iter().all(|&node| is_ignorable_whitespace(host, node)) {
        callback(&sequence, std::ptr::null_mut());
    }
}

fn remove_irrelevant_boxes(host: &TreeBuilderHost<'_>, root: LayoutNode) {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // 1. Remove irrelevant boxes:
    // The following boxes are discarded as if they were display:none:
    let mut to_remove = Vec::new();
    host.for_each_in_inclusive_subtree(root, |node| {
        let facts = host.facts(node);

        // 1. Children of a table-column.
        if facts.is_box && facts.current_display == FfiTableDisplay::TableColumn {
            let mut child = host.first_child(node);
            while !child.is_null() {
                to_remove.push(child);
                child = host.next_sibling(child);
            }
        }

        // 2. Children of a table-column-group which are not a table-column.
        if facts.is_box && facts.current_display == FfiTableDisplay::TableColumnGroup {
            let mut child = host.first_child(node);
            while !child.is_null() {
                if host.facts(child).current_display != FfiTableDisplay::TableColumn {
                    to_remove.push(child);
                }
                child = host.next_sibling(child);
            }
        }

        // FIXME: 3. Anonymous inline boxes which contain only white space and are between two immediate siblings each
        //           of which is a table-non-root box.

        // 4. Anonymous inline boxes which meet all of the following criteria:
        //    - they contain only white space
        //    - they are the first and/or last child of a tabular container
        //    - whose immediate sibling, if any, is a table-non-root box
        let parent = host.parent(node);
        if facts.is_box
            && !parent.is_null()
            && is_tabular_container(host.facts(parent))
            && is_first_or_last_child_with_table_non_root_sibling_if_any(host, node)
            && is_ignorable_whitespace(host, node)
        {
            to_remove.push(node);
            return TraversalDecision::SkipChildrenAndContinue;
        }
        TraversalDecision::Continue
    });
    host.remove_nodes(&to_remove);
}

fn generate_missing_child_wrappers(host: &TreeBuilderHost<'_>, root: LayoutNode) {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // 2. Generate missing child wrappers:
    host.for_each_in_inclusive_subtree(root, |parent| {
        let facts = host.facts(parent);
        if !facts.is_box {
            return TraversalDecision::Continue;
        }

        match facts.current_display {
            FfiTableDisplay::TableRoot => {
                // 1. An anonymous table-row box must be generated around each sequence of consecutive children of a
                //    table-root box which are not proper table child boxes.
                for_each_sequence_of_consecutive_children_matching(
                    host,
                    parent,
                    |child| !child.has_style || !is_proper_table_child(child),
                    |sequence, nearest_sibling| {
                        host.wrap_in_anonymous(sequence, nearest_sibling, FfiAnonymousTableBoxKind::TableRow);
                    },
                );
            }
            FfiTableDisplay::TableRowGroup | FfiTableDisplay::TableHeaderGroup | FfiTableDisplay::TableFooterGroup => {
                // 2. An anonymous table-row box must be generated around each sequence of consecutive children of a
                //    table-row-group box which are not table-row boxes.
                for_each_sequence_of_consecutive_children_matching(
                    host,
                    parent,
                    |child| !child.has_style || child.current_display != FfiTableDisplay::TableRow,
                    |sequence, nearest_sibling| {
                        host.wrap_in_anonymous(sequence, nearest_sibling, FfiAnonymousTableBoxKind::TableRow);
                    },
                );
            }
            FfiTableDisplay::TableRow => {
                // 3. An anonymous table-cell box must be generated around each sequence of consecutive children of a
                //    table-row box which are not table-cell boxes.
                for_each_sequence_of_consecutive_children_matching(
                    host,
                    parent,
                    |child| !child.has_style || child.current_display != FfiTableDisplay::TableCell,
                    |sequence, nearest_sibling| {
                        host.wrap_in_anonymous(sequence, nearest_sibling, FfiAnonymousTableBoxKind::TableCell);
                    },
                );
            }
            _ => {}
        }
        TraversalDecision::Continue
    });
}

fn generate_missing_parents(host: &TreeBuilderHost<'_>, root: LayoutNode) -> Vec<LayoutNode> {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // 3. Generate missing parents:
    let mut table_roots_to_wrap = Vec::new();
    host.for_each_in_inclusive_subtree(root, |parent| {
        let facts = host.facts(parent);
        if !facts.has_style {
            return TraversalDecision::Continue;
        }

        // 1. An anonymous table-row box must be generated around each sequence of consecutive table-cell boxes whose
        //    parent is not a table-row.
        if facts.current_display != FfiTableDisplay::TableRow {
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |child| child.has_style && child.current_display == FfiTableDisplay::TableCell,
                |sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, FfiAnonymousTableBoxKind::TableRow);
                },
            );
        }

        // 2. An anonymous table or inline-table box must be generated around each sequence of consecutive proper table
        //    child boxes which are misparented.
        // If the box’s parent is an inline, run-in, or ruby box (or any box that would perform inlinification of its
        // children), then an inline-table box must be generated; otherwise it must be a table box.
        // FIXME: run-in and ruby boxes
        let anonymous_table_kind = if facts.is_inline_outside {
            FfiAnonymousTableBoxKind::InlineTable
        } else {
            FfiAnonymousTableBoxKind::Table
        };

        let is_table_row_group = matches!(
            facts.current_display,
            FfiTableDisplay::TableRowGroup | FfiTableDisplay::TableHeaderGroup | FfiTableDisplay::TableFooterGroup
        );
        // A table-row is misparented if its parent is neither a table-row-group nor a table-root box.
        if !is_table_row_group && facts.current_display != FfiTableDisplay::TableRoot {
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |child| child.has_style && child.current_display == FfiTableDisplay::TableRow,
                |sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, anonymous_table_kind);
                },
            );
        }

        // A table-column box is misparented if its parent is neither a table-column-group box nor a table-root box.
        if facts.current_display != FfiTableDisplay::TableColumnGroup
            && facts.current_display != FfiTableDisplay::TableRoot
        {
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |child| child.has_style && child.current_display == FfiTableDisplay::TableColumn,
                |sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, anonymous_table_kind);
                },
            );
        }

        // A table-row-group, table-column-group, or table-caption box is misparented if its parent is not a table-root
        // box.
        if facts.current_display != FfiTableDisplay::TableRoot {
            for_each_sequence_of_consecutive_children_matching(
                host,
                parent,
                |child| {
                    if !child.has_style {
                        return false;
                    }
                    let display = display_for_table_fixup(child);
                    is_table_track_group(display) || display == FfiTableDisplay::TableCaption
                },
                |sequence, nearest_sibling| {
                    host.wrap_in_anonymous(sequence, nearest_sibling, anonymous_table_kind);
                },
            );
        }

        // 3. An anonymous table-wrapper box must be generated around each table-root.
        if facts.is_box && facts.current_display == FfiTableDisplay::TableRoot {
            if facts.has_been_wrapped_in_table_wrapper {
                let parent = host.parent(parent);
                assert!(!parent.is_null());
                assert!(host.facts(parent).is_table_wrapper);
                return TraversalDecision::Continue;
            }
            table_roots_to_wrap.push(parent);
        }

        TraversalDecision::Continue
    });

    for &table_root in &table_roots_to_wrap {
        let nearest_sibling = host.next_sibling(table_root);
        let parent = host.parent(table_root);
        assert!(!parent.is_null());
        if host.facts(parent).is_table_wrapper {
            // SAFETY: Both nodes are live and `parent` is a TableWrapper.
            unsafe { (host.callbacks.update_existing_table_wrapper)(host.callbacks.context, table_root, parent) };
        } else {
            // SAFETY: `table_root` is attached and `nearest_sibling` is either null or its live next sibling.
            unsafe { (host.callbacks.wrap_table_root)(host.callbacks.context, table_root, nearest_sibling) };
        }
    }
    table_roots_to_wrap
}

struct TableGrid<'a> {
    host: &'a TreeBuilderHost<'a>,
    grid: *mut c_void,
}

impl TableGrid<'_> {
    fn column_count(&self) -> usize {
        // SAFETY: `grid` is live until this wrapper is dropped.
        unsafe { (self.host.callbacks.table_grid_column_count)(self.host.callbacks.context, self.grid) }
    }

    fn is_occupied(&self, column_index: usize, row_index: usize) -> bool {
        // SAFETY: `grid` is live until this wrapper is dropped.
        unsafe {
            (self.host.callbacks.table_grid_is_occupied)(
                self.host.callbacks.context,
                self.grid,
                column_index,
                row_index,
            )
        }
    }
}

impl Drop for TableGrid<'_> {
    fn drop(&mut self) {
        // SAFETY: This wrapper owns the grid returned by `create_table_grid` and drops it exactly once.
        unsafe { (self.host.callbacks.destroy_table_grid)(self.host.callbacks.context, self.grid) };
    }
}

fn fixup_row(host: &TreeBuilderHost<'_>, row: LayoutNode, table_grid: &TableGrid<'_>, row_index: usize) {
    for column_index in 0..table_grid.column_count() {
        if table_grid.is_occupied(column_index, row_index) {
            continue;
        }
        // SAFETY: `row` is a live table-row box.
        unsafe { (host.callbacks.append_missing_table_cell)(host.callbacks.context, row) };
    }
}

fn missing_cells_fixup(host: &TreeBuilderHost<'_>, table_roots: &[LayoutNode]) {
    // https://drafts.csswg.org/css-tables-3/#missing-cells-fixup
    // Once the amount of columns in a table is known, any table-row box must be modified such that it owns enough
    // cells to fill all the columns of the table, when taking spans into account. New table-cell anonymous boxes must
    // be appended to its rows content until this condition is met.
    for &table_root in table_roots {
        // SAFETY: `table_root` is a live table-root box and the callback returns a newly owned grid.
        let grid = unsafe { (host.callbacks.create_table_grid)(host.callbacks.context, table_root) };
        assert!(!grid.is_null());
        let grid = TableGrid { host, grid };
        let mut row_index = 0;

        let mut child = host.first_child(table_root);
        while !child.is_null() {
            let child_facts = host.facts(child);
            if child_facts.is_box
                && matches!(
                    child_facts.current_display,
                    FfiTableDisplay::TableRowGroup
                        | FfiTableDisplay::TableHeaderGroup
                        | FfiTableDisplay::TableFooterGroup
                )
            {
                let mut row = host.first_child(child);
                while !row.is_null() {
                    let row_facts = host.facts(row);
                    if row_facts.is_box && row_facts.current_display == FfiTableDisplay::TableRow {
                        fixup_row(host, row, &grid, row_index);
                        row_index += 1;
                    }
                    row = host.next_sibling(row);
                }
            }
            child = host.next_sibling(child);
        }

        let mut child = host.first_child(table_root);
        while !child.is_null() {
            let child_facts = host.facts(child);
            if child_facts.is_box && child_facts.current_display == FfiTableDisplay::TableRow {
                fixup_row(host, child, &grid, row_index);
                row_index += 1;
            }
            child = host.next_sibling(child);
        }
    }
}

/// Runs the CSS table-model fixup over an already constructed layout subtree.
///
/// # Safety
///
/// `callbacks` must point to a valid callback table for the duration of the call. `root` must be a live
/// NodeWithStyle, and every callback-returned node must remain live for as long as it stays attached to that root.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_fixup_tables(callbacks: *const FfiTreeBuilderCallbacks, root: *mut c_void) {
    abort_on_panic(|| {
        assert!(!callbacks.is_null());
        assert!(!root.is_null());
        // SAFETY: Guaranteed by the entry point's contract and checked for null above.
        let host = TreeBuilderHost {
            callbacks: unsafe { &*callbacks },
        };
        remove_irrelevant_boxes(&host, root);
        generate_missing_child_wrappers(&host, root);
        let table_roots = generate_missing_parents(&host, root);
        missing_cells_fixup(&host, &table_roots);
    });
}

#[cfg(test)]
mod tests {
    use super::{FfiReplacedElementDisplayAdjustment, rust_adjusted_table_display_for_replaced_element};

    #[test]
    fn replaced_table_display_adjustments() {
        assert_eq!(
            rust_adjusted_table_display_for_replaced_element(true, true, false, false),
            FfiReplacedElementDisplayAdjustment::Block
        );
        assert_eq!(
            rust_adjusted_table_display_for_replaced_element(true, false, false, false),
            FfiReplacedElementDisplayAdjustment::Inline
        );
        assert_eq!(
            rust_adjusted_table_display_for_replaced_element(false, false, true, false),
            FfiReplacedElementDisplayAdjustment::Inline
        );
        assert_eq!(
            rust_adjusted_table_display_for_replaced_element(false, false, false, true),
            FfiReplacedElementDisplayAdjustment::Inline
        );
        assert_eq!(
            rust_adjusted_table_display_for_replaced_element(false, false, false, false),
            FfiReplacedElementDisplayAdjustment::None
        );
    }
}
