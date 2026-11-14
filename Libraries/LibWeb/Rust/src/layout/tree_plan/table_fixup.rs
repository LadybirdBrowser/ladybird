/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Table fixup of child lists, steps 1 to 3 of the fixup algorithm, written once for the two trees it goes over: the
//! plan of a walk, which fixes up each list it planned all of as the list closes, and the arena, where the commit fixes
//! up the lists the walk left to it. The steps that read and change the child list of a box read and change only that
//! list and those of the anonymous boxes in it, so fixing up each list on its own makes the tree passes over the whole
//! tree make.
//! https://drafts.csswg.org/css-tables-3/#fixup-algorithm

use super::{OneByOneRowSource, PlanNode, PlanTree, RowCommittedOneByOne, box_fact};
use crate::css::style::tree::StyleNodeID;
use crate::layout::FfiDisplay;
use crate::layout::node_data::{NodeFlag, NodeKind};
use crate::layout::node_facts;
use libcompositing_rust::node_slot_id::NodeSlotId;

/// The kind of anonymous box table fixup generates around a sequence of children.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum AnonymousTableBoxKind {
    TableRow,
    TableCell,
    Table,
    InlineTable,
}

impl AnonymousTableBoxKind {
    /// The style the box takes from the box it is generated in, and the kind of box it is.
    pub(crate) fn style_and_node_kind(self) -> (crate::css::style::layout_style::AnonymousStyleKind, NodeKind) {
        use crate::css::style::layout_style::AnonymousStyleKind;
        match self {
            Self::TableRow => (AnonymousStyleKind::TableRow, NodeKind::Box),
            Self::TableCell => (AnonymousStyleKind::TableCell, NodeKind::BlockContainer),
            Self::Table => (AnonymousStyleKind::Table, NodeKind::Box),
            Self::InlineTable => (AnonymousStyleKind::InlineTable, NodeKind::Box),
        }
    }
}

/// A tree table fixup goes over.
pub(crate) trait TableFixupTree {
    fn parent(&self, node: NodeSlotId) -> NodeSlotId;
    fn first_child(&self, node: NodeSlotId) -> NodeSlotId;
    fn next_sibling(&self, node: NodeSlotId) -> NodeSlotId;
    fn previous_sibling(&self, node: NodeSlotId) -> NodeSlotId;
    fn kind(&self, node: NodeSlotId) -> NodeKind;
    fn has_flag(&self, node: NodeSlotId, flag: NodeFlag) -> bool;
    fn is_out_of_flow(&self, node: NodeSlotId) -> bool;
    fn is_inline_outside(&self, node: NodeSlotId) -> bool;
    /// The display of the box `node`, the one its box uses; that of its parent for a text, whose style it carries.
    fn display(&self, node: NodeSlotId) -> FfiDisplay;
    fn display_before_box_type_transformation(&self, node: NodeSlotId) -> FfiDisplay;
    /// Whether `text`, a text, holds nothing but white space.
    fn text_is_white_space(&mut self, text: NodeSlotId) -> bool;
    /// Takes `nodes` out of the tree, as if they were `display: none`.
    fn remove(&mut self, nodes: &[NodeSlotId]);
    /// Makes an anonymous box of `kind` below `parent`, before `before`, and moves `nodes`, consecutive children of
    /// `parent`, into it. Answers the box.
    fn wrap_in_anonymous_table_box(
        &mut self,
        parent: NodeSlotId,
        nodes: &[NodeSlotId],
        before: NodeSlotId,
        kind: AnonymousTableBoxKind,
    ) -> NodeSlotId;
    fn set_children_are_inline(&mut self, node: NodeSlotId, children_are_inline: bool);
    /// Wraps `table_root` in a new anonymous table wrapper box, which takes the table's outer properties.
    fn wrap_table_root(&mut self, table_root: NodeSlotId);
}

/// Whether a walk over a child list goes into the children of the box it visits.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Visit {
    Children,
    SkipChildren,
}

/// Fixes up the parts of tables in the child lists of `owners`, each with those of the anonymous boxes in it: steps 1
/// to 3 of the fixup algorithm, each over every list before the next. Answers the table roots the steps reached.
pub(crate) fn fix_up_tables_in_child_lists(tree: &mut impl TableFixupTree, owners: &[NodeSlotId]) -> Vec<NodeSlotId> {
    for &owner in owners {
        remove_irrelevant_boxes(tree, owner);
    }
    for &owner in owners {
        generate_missing_child_wrappers(tree, owner);
    }
    let mut table_roots = Vec::new();
    for &owner in owners {
        generate_missing_parents(tree, owner, &mut table_roots);
    }
    table_roots
}

fn is_table_track(display: FfiDisplay) -> bool {
    display.is_table_row() || display.is_table_column()
}

fn is_table_track_group(display: FfiDisplay) -> bool {
    // Unless explicitly mentioned otherwise, mentions of table-row-groups in this spec also encompass the specialized
    // table-header-groups and table-footer-groups.
    display.is_table_row_group()
        || display.is_table_header_group()
        || display.is_table_footer_group()
        || display.is_table_column_group()
}

/// The display of `node` the steps read.
pub(crate) fn display_for_table_fixup(tree: &impl TableFixupTree, node: NodeSlotId) -> FfiDisplay {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // For the purposes of these rules, out-of-flow elements are represented as inline elements of zero width and
    // height. Their containing blocks are chosen accordingly.
    //
    // AD-HOC: Table-internal boxes can be blockified before fixup. Use the pre-transformation display for out-of-flow
    // authored boxes so an out-of-flow table-header-group is still recognized as a proper table child during fixup.
    // An in-flow box blockified as a flex or grid item is a block, though, and not a proper table child.
    // Element-specific display adjustments for replaced elements and buttons take precedence over that display.
    if !tree.is_out_of_flow(node)
        || tree.has_flag(node, NodeFlag::IsReplacedElement)
        || tree.has_flag(node, NodeFlag::Anonymous)
        || tree.has_flag(node, NodeFlag::UsesButtonLayout)
    {
        tree.display(node)
    } else {
        tree.display_before_box_type_transformation(node)
    }
}

fn is_proper_table_child(tree: &impl TableFixupTree, node: NodeSlotId) -> bool {
    let display = display_for_table_fixup(tree, node);
    is_table_track_group(display) || is_table_track(display) || display.is_table_caption()
}

fn is_table_non_root_box(tree: &impl TableFixupTree, node: NodeSlotId) -> bool {
    let display = tree.display(node);
    display.is_internal_table() || display.is_table_caption()
}

fn is_table_non_root_box_sibling(tree: &impl TableFixupTree, sibling: NodeSlotId) -> bool {
    // Text nodes carry their parent's style, so only boxes can be table-non-root boxes.
    !sibling.is_invalid() && node_facts::kind_is_box(tree.kind(sibling)) && is_table_non_root_box(tree, sibling)
}

/// https://drafts.csswg.org/css-tables-3/#tabular-container
pub(crate) fn is_tabular_container(tree: &impl TableFixupTree, node: NodeSlotId) -> bool {
    let display = tree.display(node);
    display.is_table_inside()
        || display.is_table_row()
        || display.is_table_row_group()
        || display.is_table_header_group()
        || display.is_table_footer_group()
}

fn is_ignorable_whitespace(tree: &mut impl TableFixupTree, node: NodeSlotId) -> bool {
    if node_facts::kind_is_text(tree.kind(node)) {
        return tree.text_is_white_space(node);
    }
    if !tree.has_flag(node, NodeFlag::Anonymous)
        || !node_facts::kind_is_block_container(tree.kind(node))
        || !tree.has_flag(node, NodeFlag::ChildrenAreInline)
    {
        return false;
    }
    let mut contains_only_whitespace = true;
    for_each_in_subtree(tree, node, &mut |tree, descendant| {
        let holds_only_whitespace = if node_facts::kind_is_text(tree.kind(descendant)) {
            tree.text_is_white_space(descendant)
        } else {
            !tree.is_out_of_flow(descendant) && tree.has_flag(descendant, NodeFlag::Anonymous)
        };
        contains_only_whitespace &= holds_only_whitespace;
        holds_only_whitespace.then_some(Visit::Children)
    });
    contains_only_whitespace
}

/// Visits `root` and the boxes of its subtree in pre-order, reading where to go next once each visit returns; a visit
/// answering none ends the walk. A visit may change the tree below the box it visits.
fn for_each_in_subtree<T: TableFixupTree>(
    tree: &mut T,
    root: NodeSlotId,
    visit: &mut dyn FnMut(&mut T, NodeSlotId) -> Option<Visit>,
) {
    let mut current = root;
    loop {
        let Some(decision) = visit(tree, current) else {
            return;
        };
        if decision == Visit::Children {
            let first_child = tree.first_child(current);
            if !first_child.is_invalid() {
                current = first_child;
                continue;
            }
        }
        while current != root && tree.next_sibling(current).is_invalid() {
            current = tree.parent(current);
        }
        if current == root {
            return;
        }
        current = tree.next_sibling(current);
    }
}

/// Visits `owner` and the boxes of its child list in pre-order, telling `visit` whether the child list of the box it
/// visits is part of what table fixup goes over: the child list of `owner`, and those of the anonymous boxes in it that
/// are no pseudo-element's. The steps that act on the children of a box act only on these lists.
fn for_each_in_table_fixup_child_list<T: TableFixupTree>(
    tree: &mut T,
    owner: NodeSlotId,
    visit: &mut dyn FnMut(&mut T, NodeSlotId, bool) -> Visit,
) {
    for_each_in_subtree(tree, owner, &mut |tree, node| {
        let child_list_is_in_scope = node == owner
            || (!node_facts::kind_is_text(tree.kind(node))
                && tree.has_flag(node, NodeFlag::Anonymous)
                && !tree.has_flag(node, NodeFlag::IsPseudoElementPrincipalBox));
        match visit(tree, node, child_list_is_in_scope) {
            Visit::Children if child_list_is_in_scope => Some(Visit::Children),
            _ => Some(Visit::SkipChildren),
        }
    });
}

/// Wraps each sequence of consecutive children of `parent` that `matcher` matches, with the white space between them,
/// in an anonymous box of `kind`.
fn for_each_sequence_of_consecutive_children_matching<T: TableFixupTree>(
    tree: &mut T,
    parent: NodeSlotId,
    matcher: &dyn Fn(&T, NodeSlotId) -> bool,
    kind: AnonymousTableBoxKind,
) {
    let mut sequences = Vec::new();
    let mut sequence: Vec<NodeSlotId> = Vec::new();
    let mut end_sequence = |tree: &mut T, sequence: &mut Vec<NodeSlotId>, mut nearest_sibling: NodeSlotId| {
        // Whitespace that follows the last matching child is not part of the sequence. The fixup algorithm only
        // discards whitespace-only boxes that lie between two table-non-root boxes (step 1), so whitespace after the
        // last box of the sequence stays outside the anonymous wrapper: in "a <cell>b</cell><cell>c</cell> d" the
        // space before "d" is ordinary inline content next to the generated inline-table.
        while sequence.last().is_some_and(|&last| !matcher(tree, last)) {
            nearest_sibling = sequence.pop().expect("a trailing whitespace node");
        }
        if !sequence.iter().all(|&node| is_ignorable_whitespace(tree, node)) {
            sequences.push((std::mem::take(sequence), nearest_sibling));
        }
        sequence.clear();
    };
    let mut child = tree.first_child(parent);
    while !child.is_invalid() {
        if matcher(tree, child) || (!sequence.is_empty() && is_ignorable_whitespace(tree, child)) {
            sequence.push(child);
        } else if !sequence.is_empty() {
            end_sequence(tree, &mut sequence, child);
        }
        child = tree.next_sibling(child);
    }
    if !sequence.is_empty() {
        end_sequence(tree, &mut sequence, NodeSlotId::INVALID);
    }
    for (sequence, nearest_sibling) in sequences {
        wrap_in_anonymous_table_box(tree, parent, &sequence, nearest_sibling, kind);
    }
}

fn wrap_in_anonymous_table_box(
    tree: &mut impl TableFixupTree,
    parent: NodeSlotId,
    nodes: &[NodeSlotId],
    before: NodeSlotId,
    kind: AnonymousTableBoxKind,
) {
    let wrapper = tree.wrap_in_anonymous_table_box(parent, nodes, before, kind);
    // An anonymous table-cell takes over the run of content it is generated around, so it has inline children
    // exactly when that run is inline-level. Anonymous table-row, table and inline-table boxes only ever own
    // table-internal boxes (their wrapped children are, or become through the remaining fixup steps, table-internal
    // boxes), so they never have inline children, not even when they are generated inside an inline box. An
    // inline-table with inline children would also not derive its baseline from its first row.
    let children_are_inline =
        kind == AnonymousTableBoxKind::TableCell && nodes.iter().any(|&node| tree.is_inline_outside(node));
    tree.set_children_are_inline(wrapper, children_are_inline);
    // The wrapper takes the place of the run in its parent. A table-row, table-cell or table box is block-level,
    // so a parent whose inline content it replaced entirely (a table-row-group or table around text, a table-row
    // around text or inline boxes) no longer has inline children. A stale flag would make baseline derivation look
    // for line boxes the parent does not have, leaving it and every box that derives its baseline from it (a cell
    // around a nested table, an inline-table in a line) without a baseline. A parent that keeps inline-level
    // children next to the wrapper (text around an anonymous table generated for out-of-flow row groups, which
    // the fixup treats as inline-level boxes of zero size) still lays its children out in a line, and inline
    // boxes keep their inline children: the table-row generated around their cells is wrapped in an inline-level
    // inline-table next.
    if kind != AnonymousTableBoxKind::InlineTable && node_facts::kind_is_box(tree.kind(parent)) {
        let mut child = tree.first_child(parent);
        while !child.is_invalid() {
            if child != wrapper && tree.is_inline_outside(child) {
                return;
            }
            child = tree.next_sibling(child);
        }
        tree.set_children_are_inline(parent, false);
    }
}

fn remove_irrelevant_boxes(tree: &mut impl TableFixupTree, owner: NodeSlotId) {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // 1. Remove irrelevant boxes:
    // The following boxes are discarded as if they were display:none:
    // NB: Steps 1 and 2, the children of a table-column and those of a table-column-group which are not a
    //     table-column, are never planned.
    let mut to_remove = Vec::new();
    for_each_in_table_fixup_child_list(tree, owner, &mut |tree, node, _| {
        if node == owner {
            return Visit::Children;
        }
        let (parent, previous, next) = (tree.parent(node), tree.previous_sibling(node), tree.next_sibling(node));

        // 3. Anonymous inline boxes which contain only white space and are between two immediate siblings each of
        //    which is a table-non-root box.
        // This is what keeps "<cell>b</cell> <cell>c</cell>" a single table with adjacent cells, regardless of whether
        // the siblings live in a table, in a block (where the whitespace sits in an anonymous block wrapper) or in an
        // inline box. The whitespace before the first and after the last table-non-root box of such a run is not
        // discarded and remains ordinary inline content.
        if is_table_non_root_box_sibling(tree, previous)
            && is_table_non_root_box_sibling(tree, next)
            && is_ignorable_whitespace(tree, node)
        {
            to_remove.push(node);
            return Visit::SkipChildren;
        }

        // 4. Anonymous inline boxes which meet all of the following criteria:
        //    - they contain only white space
        //    - they are the first and/or last child of a tabular container
        //    - whose immediate sibling, if any, is a table-non-root box
        if node_facts::kind_is_box(tree.kind(node))
            && is_tabular_container(tree, parent)
            && !tree.has_flag(parent, NodeFlag::Anonymous)
            && (previous.is_invalid() || next.is_invalid())
            && (previous.is_invalid() || is_table_non_root_box(tree, previous))
            && (next.is_invalid() || is_table_non_root_box(tree, next))
            && is_ignorable_whitespace(tree, node)
        {
            to_remove.push(node);
            return Visit::SkipChildren;
        }
        Visit::Children
    });
    tree.remove(&to_remove);
}

fn generate_missing_child_wrappers<T: TableFixupTree>(tree: &mut T, owner: NodeSlotId) {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // 2. Generate missing child wrappers:
    for_each_in_table_fixup_child_list(tree, owner, &mut |tree, parent, child_list_is_in_scope| {
        let kind = tree.kind(parent);
        // AD-HOC: SVG layout derives box types from the element, so display values must not introduce anonymous boxes
        //         inside SVG content.
        if !child_list_is_in_scope || !node_facts::kind_is_box(kind) || node_facts::kind_is_svg_content(kind) {
            return Visit::Children;
        }
        let has_style = |tree: &T, child: NodeSlotId| tree.has_flag(child, NodeFlag::HasStyle);
        let display = tree.display(parent);
        if display.is_table_inside() {
            // 1. An anonymous table-row box must be generated around each sequence of consecutive children of a
            //    table-root box which are not proper table child boxes.
            for_each_sequence_of_consecutive_children_matching(
                tree,
                parent,
                &|tree, child| !has_style(tree, child) || !is_proper_table_child(tree, child),
                AnonymousTableBoxKind::TableRow,
            );
        } else if display.is_table_row_group() || display.is_table_header_group() || display.is_table_footer_group() {
            // 2. An anonymous table-row box must be generated around each sequence of consecutive children of a
            //    table-row-group box which are not table-row boxes.
            for_each_sequence_of_consecutive_children_matching(
                tree,
                parent,
                &|tree, child| !has_style(tree, child) || !tree.display(child).is_table_row(),
                AnonymousTableBoxKind::TableRow,
            );
        } else if display.is_table_row() {
            // 3. An anonymous table-cell box must be generated around each sequence of consecutive children of a
            //    table-row box which are not table-cell boxes.
            for_each_sequence_of_consecutive_children_matching(
                tree,
                parent,
                &|tree, child| !has_style(tree, child) || !tree.display(child).is_table_cell(),
                AnonymousTableBoxKind::TableCell,
            );
        }
        Visit::Children
    });
}

fn generate_missing_parents<T: TableFixupTree>(tree: &mut T, owner: NodeSlotId, table_roots: &mut Vec<NodeSlotId>) {
    // https://drafts.csswg.org/css-tables-3/#fixup-algorithm
    // 3. Generate missing parents:
    let mut table_roots_to_wrap = Vec::new();
    for_each_in_table_fixup_child_list(tree, owner, &mut |tree, parent, child_list_is_in_scope| {
        if !tree.has_flag(parent, NodeFlag::HasStyle) {
            return Visit::Children;
        }
        let current_display = tree.display(parent);
        // NB: The steps below wrap children of the box, but for the last, which wraps the box itself.
        let wraps_children = child_list_is_in_scope && !node_facts::kind_is_svg_content(tree.kind(parent));

        // 1. An anonymous table-row box must be generated around each sequence of consecutive table-cell boxes whose
        //    parent is not a table-row.
        if wraps_children && !current_display.is_table_row() {
            for_each_sequence_of_consecutive_children_matching(
                tree,
                parent,
                &|tree, child| tree.has_flag(child, NodeFlag::HasStyle) && tree.display(child).is_table_cell(),
                AnonymousTableBoxKind::TableRow,
            );
        }

        // 2. An anonymous table or inline-table box must be generated around each sequence of consecutive proper table
        //    child boxes which are misparented.
        // If the box’s parent is an inline, run-in, or ruby box (or any box that would perform inlinification of its
        // children), then an inline-table box must be generated; otherwise it must be a table box.
        // FIXME: run-in and ruby boxes
        let anonymous_table_kind = if tree.is_inline_outside(parent) {
            AnonymousTableBoxKind::InlineTable
        } else {
            AnonymousTableBoxKind::Table
        };

        // A table-row is misparented if its parent is neither a table-row-group nor a table-root box.
        // A table-column box is misparented if its parent is neither a table-column-group box nor a table-root box.
        // A table-row-group, table-column-group, or table-caption box is misparented if its parent is not a table-root
        // box.
        // NB: The sequence spans misparented proper table children of every kind: the anonymous table-row generated
        //     around loose cells by step 1 and the table-row-group next to it belong to the same anonymous table.
        if wraps_children && !current_display.is_table_inside() {
            let is_table_row_group = current_display.is_table_row_group_kind();
            let is_table_column_group = current_display.is_table_column_group();
            for_each_sequence_of_consecutive_children_matching(
                tree,
                parent,
                &|tree, child| {
                    if !tree.has_flag(child, NodeFlag::HasStyle) {
                        return false;
                    }
                    let display = tree.display(child);
                    if display.is_table_row() {
                        return !is_table_row_group;
                    }
                    if display.is_table_column() {
                        return !is_table_column_group;
                    }
                    let display = display_for_table_fixup(tree, child);
                    is_table_track_group(display) || display.is_table_caption()
                },
                anonymous_table_kind,
            );
        }

        // 3. An anonymous table-wrapper box must be generated around each table-root.
        if node_facts::kind_is_box(tree.kind(parent)) && current_display.is_table_inside() {
            let wrap_parent = tree.parent(parent);
            if !wrap_parent.is_invalid() && !node_facts::kind_is_svg_content(tree.kind(wrap_parent)) {
                table_roots.push(parent);
                if tree.kind(wrap_parent) != NodeKind::TableWrapper {
                    table_roots_to_wrap.push(parent);
                }
            }
        }
        Visit::Children
    });
    for table_root in table_roots_to_wrap {
        tree.wrap_table_root(table_root);
    }
}

/// The plan of a walk as table fixup goes over it, where `text_is_whitespace` tells whether a text holds only white
/// space.
struct PlanTableFixup<'t> {
    tree: &'t mut PlanTree,
    text_is_whitespace: &'t dyn Fn(StyleNodeID) -> bool,
}

impl TableFixupTree for PlanTableFixup<'_> {
    fn parent(&self, node: NodeSlotId) -> NodeSlotId {
        self.tree.node(node).parent()
    }

    fn first_child(&self, node: NodeSlotId) -> NodeSlotId {
        self.tree.node(node).first_child()
    }

    fn next_sibling(&self, node: NodeSlotId) -> NodeSlotId {
        self.tree.node(node).next_sibling()
    }

    fn previous_sibling(&self, node: NodeSlotId) -> NodeSlotId {
        self.tree.node(node).previous_sibling()
    }

    fn kind(&self, node: NodeSlotId) -> NodeKind {
        self.tree.node(node).kind()
    }

    fn has_flag(&self, node: NodeSlotId, flag: NodeFlag) -> bool {
        self.tree.node(node).has_flag(flag)
    }

    fn is_out_of_flow(&self, node: NodeSlotId) -> bool {
        self.tree.node(node).is_out_of_flow()
    }

    fn is_inline_outside(&self, node: NodeSlotId) -> bool {
        self.tree.node(node).has_fact(box_fact::INLINE_OUTSIDE)
    }

    fn display(&self, node: NodeSlotId) -> FfiDisplay {
        let planned = self.tree.node(node);
        if node_facts::kind_is_text(planned.kind()) {
            return self.display(planned.parent());
        }
        if let Some(&display) = self.tree.table_part_displays.get(&node) {
            return display;
        }
        match self.tree.style_of(node) {
            Some(style) => {
                crate::layout::tree_builder::used_display_of_box(planned.kind(), planned.flags(), style.display())
            }
            // NB: Every other box without a style of its own the walk made is an anonymous box that is no part of a
            //     table.
            None if planned.has_fact(box_fact::INLINE_OUTSIDE) => FfiDisplay::inline(),
            None => FfiDisplay::block(),
        }
    }

    fn display_before_box_type_transformation(&self, node: NodeSlotId) -> FfiDisplay {
        match self.tree.style_of(node) {
            Some(style) => style.display_before_box_type_transformation(),
            None => self.display(node),
        }
    }

    fn text_is_white_space(&mut self, text: NodeSlotId) -> bool {
        if let Some(text) = self.tree.style_node(text) {
            return (self.text_is_whitespace)(text);
        }
        // NB: The commit spells a generated text, which holds only white space where the content of the pseudo-element
        //     it is generated for does.
        let mut ancestor = self.tree.node(text).parent();
        while !ancestor.is_invalid() && !self.tree.node(ancestor).has_flag(NodeFlag::IsPseudoElementPrincipalBox) {
            ancestor = self.tree.node(ancestor).parent();
        }
        self.tree.white_space_pseudo_element_boxes.contains(&ancestor)
    }

    fn remove(&mut self, nodes: &[NodeSlotId]) {
        // NB: The commit frees the rows of the boxes removed, once it committed every row of the plan.
        for &node in nodes {
            self.tree.detach(node);
            self.tree.dropped_rows.push(node);
        }
    }

    fn wrap_in_anonymous_table_box(
        &mut self,
        parent: NodeSlotId,
        nodes: &[NodeSlotId],
        _: NodeSlotId,
        kind: AnonymousTableBoxKind,
    ) -> NodeSlotId {
        let tree = &mut *self.tree;
        let display = match kind {
            AnonymousTableBoxKind::TableRow => FfiDisplay::internal(crate::css::css_enums::display_internal::TABLE_ROW),
            AnonymousTableBoxKind::TableCell => {
                FfiDisplay::internal(crate::css::css_enums::display_internal::TABLE_CELL)
            }
            AnonymousTableBoxKind::Table => FfiDisplay::table(),
            AnonymousTableBoxKind::InlineTable => FfiDisplay::inline_table(),
        };
        let node_kind = kind.style_and_node_kind().1;
        let wrapper = tree.push(PlanNode::new(
            node_kind,
            node_facts::construction_flags(node_kind, true, 0),
            super::box_facts_of_display(display) | box_fact::HAS_STYLE,
        ));
        tree.table_part_displays.insert(wrapper, display);
        // NB: The wrapper goes before the children it takes, so the pre-order positions of all of them stay in order.
        tree.attach(parent, wrapper, nodes[0]);
        for &node in nodes {
            tree.move_child(node, wrapper);
        }
        let facts = tree.node(wrapper).facts();
        tree.rows_committed_one_by_one.push(RowCommittedOneByOne {
            slot: wrapper,
            facts,
            source: OneByOneRowSource::AnonymousTableBox {
                style_parent: parent,
                kind,
            },
        });
        wrapper
    }

    fn set_children_are_inline(&mut self, node: NodeSlotId, children_are_inline: bool) {
        self.tree
            .set_flag(node, NodeFlag::ChildrenAreInline, children_are_inline);
    }

    fn wrap_table_root(&mut self, table_root: NodeSlotId) {
        let parent = self.tree.node(table_root).parent();
        let wrapper = self.tree.push_table_wrapper(table_root);
        self.tree.attach(parent, wrapper, table_root);
        self.tree.move_child(table_root, wrapper);
        self.tree.note_table_wrapper(wrapper, table_root);
    }
}

impl PlanTree {
    /// Fixes up the parts of tables in the child list of `owner`, a box whose children the walk planned all of, see
    /// [`fix_up_tables_in_child_lists`]. `text_is_whitespace` tells whether a text holds only white space.
    pub(crate) fn fix_up_tables_in_child_list(
        &mut self,
        owner: NodeSlotId,
        text_is_whitespace: &dyn Fn(StyleNodeID) -> bool,
    ) {
        if !self
            .node(owner)
            .has_fact(box_fact::HAS_TABLE_PART_CHILD | box_fact::TABULAR_CONTAINER)
        {
            return;
        }
        fix_up_tables_in_child_lists(
            &mut PlanTableFixup {
                tree: self,
                text_is_whitespace,
            },
            &[owner],
        );
    }

    /// Makes the anonymous table wrapper box of `table`, a table root the walk planned below `parent`, which is to go
    /// where the table is, or is placed by `place` where the walk places the table: the box that takes the table's
    /// outer properties, see [`OneByOneRowSource::TableWrapper`]. Answers the wrapper.
    pub(crate) fn push_table_wrapper(&mut self, table: NodeSlotId) -> NodeSlotId {
        let table_box = self.node(table);
        // NB: The facts of the wrapper, as those of a kept table's wrapper, are those of the table but for its inside
        //     display: its own is flow-root.
        let facts = table_box.facts()
            & !(box_fact::TABLE_INSIDE
                | box_fact::TABULAR_CONTAINER
                | box_fact::STAMPED_BY_BUILDER
                | box_fact::OWNS_CONTENT_REPLACEMENT_IMAGE
                | box_fact::HOLDS_HIGHLIGHT_STYLE
                | box_fact::OF_CHILDREN
                | box_fact::OPEN_PARENT
                | box_fact::HAS_TABLE_PART_CHILD
                | box_fact::DERIVED_ON_COMMIT
                | box_fact::CHILDREN_DERIVED_ON_COMMIT
                | box_fact::SUBTREE_DERIVED_ON_COMMIT
                | box_fact::OWN_CHILDREN_DERIVED_ON_COMMIT
                | box_fact::ESTABLISHES_FLEX_ITEMS
                | box_fact::ESTABLISHES_GRID_ITEMS);
        self.push(PlanNode::new(
            NodeKind::TableWrapper,
            node_facts::construction_flags(NodeKind::TableWrapper, true, 0),
            facts | box_fact::HAS_STYLE,
        ))
    }

    /// Lists what the commit does for `wrapper`, the table wrapper of `table`, once the walk linked both.
    pub(crate) fn note_table_wrapper(&mut self, wrapper: NodeSlotId, table: NodeSlotId) {
        let facts = self.node(wrapper).facts();
        self.rows_committed_one_by_one.push(RowCommittedOneByOne {
            slot: wrapper,
            facts,
            source: OneByOneRowSource::TableWrapper { table },
        });
        self.table_roots.push(table);
    }
}
