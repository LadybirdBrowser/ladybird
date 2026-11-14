/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The child list of a box that keeps its box, built again: the planner plans the list as a fresh build plans it, but
//! keeps the boxes of the children that stay as they are, without walking below them. The plan is then reconciled with
//! the boxes the list had: the anonymous boxes the list makes again take over the ones it had where they hold the same
//! kept boxes, every box the list no longer has goes, before any new box is bound to its node, and the new boxes are
//! committed as a fresh build commits them, labeled in pre-order between the boxes the list kept.

use super::planned_build::{UnitCommit, commit_plan, run_post_commit_events};
use super::*;
use crate::layout::layout_node_arena::PlannedLinks;
use crate::layout::tree_plan::box_fact;
use crate::layout::tree_plan::planner::{ListWindow, PlanContext, Planner, SubtreePlan, UnitPlanning};

/// Where in the tree the box whose child list a build plans again is, as the walk that plans the list needs to know.
pub(super) struct UnitPlace {
    /// Whether the box is the viewport, whose list is the box of the document element and those of the top layer.
    pub(super) is_viewport: bool,
    /// Whether the box is inside an `<svg>`, and not inside a `<foreignObject>` in it.
    pub(super) has_svg_root: bool,
    /// The boxes above the box, outermost first.
    pub(super) ancestor_rows: Vec<NodeSlotId>,
    /// The marked nodes whose boxes go in the list, each once: see `append_window` and `bounded_window`.
    pub(super) marked_children: Vec<StyleNodeID>,
    /// The entries of the list whose nodes left the document, in the order of the list: see
    /// [`super::note_removed_list_entries`].
    pub(super) removed_entries: Vec<NodeSlotId>,
}

/// Plans the child list of `root`, the box of `element`, again, and reconciles the list with the boxes it had.
pub(super) fn update_child_list(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    root: LayoutNode,
    element: StyleNodeID,
    place: &UnitPlace,
) {
    // NB: The first letter of a box at or above the list may move with the list, so it is split out again once the
    //     build is over, where the list changed at or before it.
    let is_element = !place.is_viewport;
    let mut first_letter_owners = Vec::new();
    host.arena().with_style_store(|engine| {
        if !is_element {
            return;
        }
        let mut ancestor = Some(element);
        while let Some(current) = ancestor {
            if engine.has_published_first_letter_style(current)
                && !state.first_letter_owners_to_split_again.contains(&current)
            {
                first_letter_owners.push(current);
            }
            ancestor = engine.parent_element_or_host(current);
        }
    });
    // NB: The list is planned from the box's style, which a style change of the element left to the build.
    if is_element {
        let host_calls = host.host_calls();
        host.arena_mut()
            .install_published_style_in_kept_box(host_calls, element, root);
    }
    let reasons = host.arena().layout_tree_update_reuse_reasons(element);
    if loses_entries_in_place(host, root, element, reasons, place, &first_letter_owners) {
        remove_entries_in_place(host, state, root, element, place);
        return;
    }
    let mut unit = UnitPlanning {
        keeps_pseudo_element_boxes: reasons
            & (layout_tree_update_reuse_reason::PSEUDO_ELEMENT_CHANGE | layout_tree_update_reuse_reason::RESTYLE)
            == 0,
        fresh_from_entry: u32::MAX,
        verifies_kept_boxes: cfg!(debug_assertions) || kept_box_verification_is_enabled(),
        window: append_window(host, root, element, reasons, &place.marked_children)
            .or_else(|| bounded_window(host, root, element, reasons, &place.marked_children)),
    };
    let mut plan = plan_child_list(host, root, element, unit, place);
    // NB: A block that goes among the inline boxes of the list wraps the boxes before the window too, or those after it
    //     where it goes before the box the window ends at.
    if unit.window.is_some_and(|window| {
        plan.tree.moved_stand_in
            || window
                .end
                .is_some_and(|end| plan.tree.node(root).last_child() != host.arena().bound_row(end))
    }) {
        let free_slots = plan.discard();
        host.arena_mut().give_back_free_slots(free_slots);
        unit.window = None;
        plan = plan_child_list(host, root, element, unit, place);
    }
    // NB: Counters and quotes are resolved for the new boxes alone, so a new box that changes them before a kept one,
    //     or one the list no longer has, has every entry after it built again, which resolves them again. A window
    //     that ends before kept boxes changes them for those too, and the whole list, planned again, tells from where.
    loop {
        let fresh_from_entry = [
            first_entry_after_changed_generated_content(host, element, &plan),
            first_entry_after_dropped_generated_content(host, root, element, &plan, unit.window),
        ]
        .into_iter()
        .flatten()
        .min();
        let Some(fresh_from_entry) = fresh_from_entry else {
            break;
        };
        let free_slots = plan.discard();
        host.arena_mut().give_back_free_slots(free_slots);
        let window_ends_before_kept_boxes = unit.window.is_some_and(|window| window.end.is_some());
        unit.window = None;
        if !window_ends_before_kept_boxes {
            unit.fresh_from_entry = fresh_from_entry;
        }
        plan = plan_child_list(host, root, element, unit, place);
        if !window_ends_before_kept_boxes {
            break;
        }
    }
    // NB: The quotes of the new boxes read the depth before the list from the boxes the builds before made.
    let entry = match unit.window.and_then(|window| window.start) {
        Some(start) => Some(start),
        None => (!place.is_viewport).then_some(element),
    };
    let arena = host.arena();
    let mut quotes = UnitQuotes {
        arena,
        cursor: &mut state.quote_depth_cursor,
        entry,
        lost_its_place: false,
    };
    arena.with_style_store(|engine| plan.fill_generated_content(engine, arena, &mut quotes));
    if quotes.lost_its_place {
        state.asks_for_every_box_again = true;
    }
    let first_letters: Vec<NodeSlotId> = first_letter_owners
        .iter()
        .map(|&owner| host.arena().bound_pseudo_element_row(owner, GENERATED_FOR_FIRST_LETTER))
        .collect();
    let changed_at_or_before = match inserted_entries_between_kept_boxes(host, root, &plan, unit.window, &first_letters)
    {
        Some(planned) => {
            insert_planned_entries_in_place(host, state, root, plan, &planned);
            if kept_box_verification_is_enabled() || cfg!(debug_assertions) {
                verify_list_edited_in_place(host, root, element, place);
            }
            Vec::new()
        }
        None => reconcile(host, state, root, plan, unit.window, &first_letters),
    };
    // NB: A style change of the list's box, or of its pseudo-elements, may change the style the first letter derives.
    let restyled = reasons
        & (layout_tree_update_reuse_reason::PSEUDO_ELEMENT_CHANGE | layout_tree_update_reuse_reason::RESTYLE)
        != 0;
    for (owner, changed) in first_letter_owners.into_iter().zip(changed_at_or_before) {
        if restyled || changed {
            state.first_letter_owners_to_split_again.push(owner);
        }
    }
}

/// Whether the only change of the child list of `root`, the box of `element`, is that it lost the entries
/// `place.removed_entries`, whose boxes leave a list that a fresh walk plans with the same box for every entry that
/// stays: the list is then edited in place (see `remove_entries_in_place`), without a walk. A fresh walk decides a box
/// from its node, the list's box, and the box before it among the in-flow boxes of the list (see
/// `Planner::whitespace_text_needs_box`), so the boxes that stay keep their decisions where each case below holds.
// OPTIMIZATION: A list that loses some of its entries then costs what it loses, and reads none of the entries that stay.
fn loses_entries_in_place(
    host: &TreeBuilderHost<'_>,
    root: LayoutNode,
    element: StyleNodeID,
    reasons: u8,
    place: &UnitPlace,
    first_letter_owners: &[StyleNodeID],
) -> bool {
    use crate::css::css_enums::positioning;
    let arena = host.arena();
    // NB: The list lost entries, and nothing else changed: no node of it was inserted or marked, and the list's box and
    //     its pseudo-elements are as they were. The viewport's list holds the top layer, which moves on its own.
    if reasons != layout_tree_update_reuse_reason::CHILD_LIST_REMOVAL
        || !place.marked_children.is_empty()
        || place.removed_entries.is_empty()
        || place.is_viewport
    {
        return false;
    }
    // NB: The first letter of a box at or above the list may be in what the list holds first.
    if !first_letter_owners.is_empty() {
        return false;
    }
    if !list_may_change_in_part(host, root, element) {
        return false;
    }
    let root_data = arena.data(root);
    // NB: A list item's inside marker keeps the white space that starts its list, which a box that leaves first gives
    //     to the white space after it.
    if root_data.kind.get() == NodeKind::ListItemBox {
        return false;
    }
    let children_are_inline = node_facts::has_flag(root_data, NodeFlag::ChildrenAreInline);
    // NB: A box in flow like the boxes of the list: a block in a list of blocks, or an inline-level box in a list of
    //     inline content. A list of inline content may hold a block, as an inline box that holds one does, which a list
    //     without inline content around it would hold as a block.
    let is_in_flow_like_list = |row: NodeSlotId| {
        arena.style_view(row).is_some_and(|style| {
            !style.is_floating()
                && style.position() != positioning::ABSOLUTE
                && style.position() != positioning::FIXED
                && if children_are_inline {
                    style.display().is_inline_outside()
                } else {
                    style.display().is_block_outside()
                }
        })
    };
    // NB: A box beside a run of entries that leave, which a fresh walk decides as it did, as it does what follows it:
    //     the box of an element or a pseudo-element, in flow like the boxes of the list, so that a list that keeps a
    //     box keeps its kind of children. White space text has a box after an inline-level box, which makes that text
    //     the neighbor, and none after a block, as after what precedes the run of blocks. Two anonymous boxes that wrap
    //     runs of inline content among blocks merge where they become neighbors, and an out of flow box passes what
    //     precedes it on to what follows it.
    let keeps_its_decision_beside_a_removal = |row: NodeSlotId| {
        let data = arena.data(row);
        !(node_facts::has_flag(data, NodeFlag::Anonymous) && data.generated_for.get() == 0)
            && !node_facts::kind_is_text(data.kind.get())
            && is_in_flow_like_list(row)
    };
    arena.with_style_store(|engine| {
        let mut previous_entry = NodeSlotId::INVALID;
        for (index, &entry) in place.removed_entries.iter().enumerate() {
            if !arena.slot_is_live(entry) || arena.data(entry).parent.get() != root {
                return false;
            }
            let data = arena.data(entry);
            // NB: The box of an element: a text's box ends the white space after it in some cases and not others, and
            //     a line break starts a line, which the white space after it collapses into.
            if node_facts::has_flag(data, NodeFlag::Anonymous)
                || data.generated_for.get() != 0
                || node_facts::kind_is_text(data.kind.get())
                || data.kind.get() == NodeKind::BreakNode
            {
                return false;
            }
            // NB: An entry of a node that is still a child of the list's element stays.
            if arena
                .dom_node_style_node(entry)
                .is_some_and(|node| arena.bound_row(node) == entry && engine.dom_parent(node).is_some())
            {
                return false;
            }
            // NB: An inline box that leaves holds inline content alone.
            if !is_in_flow_like_list(entry)
                || (data.kind.get() == NodeKind::InlineNode
                    && !node_facts::has_flag(data, NodeFlag::ChildrenAreInline)
                    && !data.first_child.get().is_invalid())
            {
                return false;
            }
            // NB: What the box did to counters, quotes and list item numbers goes with it.
            if !matches!(box_subtree_numbering(arena, entry), BoxSubtreeNumbering::None) {
                return false;
            }
            // NB: The boxes that become neighbors where a run of entries leaves.
            let previous = data.previous_sibling.get();
            if previous != previous_entry && !previous.is_invalid() && !keeps_its_decision_beside_a_removal(previous) {
                return false;
            }
            let next = data.next_sibling.get();
            let next_leaves = place.removed_entries.get(index + 1) == Some(&next);
            if !next_leaves && !next.is_invalid() && !keeps_its_decision_beside_a_removal(next) {
                return false;
            }
            previous_entry = entry;
        }
        // NB: A list that loses every box decides whether its children are inline again, as a fresh walk of an
        //     empty list does; a node left in it without a box, such as white space, would be decided again too.
        let first = arena
            .data(*place.removed_entries.first().expect("an entry left the list"))
            .previous_sibling
            .get();
        let last = arena
            .data(*place.removed_entries.last().expect("an entry left the list"))
            .next_sibling
            .get();
        let loses_every_box = first.is_invalid()
            && last.is_invalid()
            && place.removed_entries.len() == {
                let mut count = 0;
                let mut row = root_data.first_child.get();
                while !row.is_invalid() && count <= place.removed_entries.len() {
                    count += 1;
                    row = arena.data(row).next_sibling.get();
                }
                count
            };
        !loses_every_box || engine.first_dom_child(element).is_none()
    })
}

/// The rows of `plan`, a plan of the child list of `root` from `window`, where the plan only puts new entries between
/// the two boxes the window starts and ends at, which are neighbors in the list: the list then takes the new boxes in
/// place (see `insert_planned_entries_in_place`), without being reconciled with the plan as a whole. Each case below is
/// one the reconcile handles beyond that.
// OPTIMIZATION: A list that gains entries between two of its boxes then costs what it gains.
fn inserted_entries_between_kept_boxes(
    host: &TreeBuilderHost<'_>,
    root: LayoutNode,
    plan: &SubtreePlan,
    window: Option<ListWindow>,
    watched: &[NodeSlotId],
) -> Option<Vec<PlannedRow>> {
    let arena = host.arena();
    // NB: The window starts and ends at boxes the list keeps, not at an anonymous box that takes over another, and no
    //     first letter at or above the list asks where the list changed.
    let window = window?;
    let (Some(start), Some(end)) = (window.start, window.end) else {
        return None;
    };
    if !window.wrapper.is_invalid() || !watched.is_empty() {
        return None;
    }
    let (start, end) = (arena.bound_row(start), arena.bound_row(end));
    // NB: The list had nothing between the two boxes, so no box of it goes.
    if start.is_invalid()
        || end.is_invalid()
        || arena.data(start).parent.get() != root
        || arena.data(end).parent.get() != root
        || arena.data(start).next_sibling.get() != end
    {
        return None;
    }
    // NB: Whether the list's children are inline stays as it was.
    if plan.tree.node(root).has_flag(NodeFlag::ChildrenAreInline)
        != node_facts::has_flag(arena.data(root), NodeFlag::ChildrenAreInline)
    {
        return None;
    }
    // NB: The plan keeps the two boxes and puts only new boxes of nodes between them, in the list itself: no anonymous
    //     box the list makes, such as one that wraps inline content or a table that wraps table parts, holds a kept
    //     box or a new one. The table fixup after the commit wraps a new table part.
    let (planned, _) = read_planned_list(arena, plan, root);
    let [first, between @ .., last] = planned.as_slice() else {
        return None;
    };
    let keeps =
        |row: &PlannedRow, slot: NodeSlotId| row.role == PlannedRole::Kept && row.slot == slot && row.depth == 0;
    if !keeps(first, start)
        || !keeps(last, end)
        || between.is_empty()
        || between.iter().any(|row| {
            row.role != PlannedRole::New
                || row.depth != 0
                || (row.flags & NodeFlag::Anonymous as u32 != 0 && plan.tree.node(row.slot).generated_for() == 0)
                || plan.tree.node(row.slot).is_table_non_root_for_table_fixup()
        })
    {
        return None;
    }
    Some(planned)
}

/// Puts the new boxes of `plan` between the two kept boxes `planned` starts and ends at, in the child list of `root`,
/// where `inserted_entries_between_kept_boxes` holds: the rows are adopted, linked and committed as the reconcile
/// does with those of any plan (see `adopt_planned_rows`, `link_planned_list` and `finish_planned_list`), and only
/// the new boxes lay out again.
fn insert_planned_entries_in_place(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    root: LayoutNode,
    mut plan: SubtreePlan,
    planned: &[PlannedRow],
) {
    let (start, end) = (planned[0].slot, planned[planned.len() - 1].slot);
    let descendant_count = adopt_planned_rows(host, &mut plan);
    {
        let arena = host.arena_mut();
        let before_start = arena.data(start).previous_sibling.get();
        let after_end = arena.data(end).next_sibling.get();
        link_planned_list(
            arena,
            root,
            planned,
            &|slot| slot,
            (before_start, start),
            (end, after_end, end),
        );
    }
    let old_parents: HashSet<NodeSlotId> = [root].into_iter().collect();
    let unit = UnitCommit {
        taken_over: &HashMap::default(),
        old_parents: &old_parents,
        kept: &|slot| plan.tree.stands_in_for(slot),
    };
    commit_plan(host, state, root, &plan, descendant_count, false, Some(&unit));
    label_planned_list(host.arena_mut(), root, start, end, &HashSet::default());
    let new_top_rows: Vec<NodeSlotId> = planned[1..planned.len() - 1].iter().map(|row| row.slot).collect();
    for &row in &new_top_rows {
        host.arena_mut().note_new_layout_subtree_attached(row);
    }
    invalidate_layout_for_list_change(host, state, root, ListChange::InsertedOnly, &new_top_rows);
    state.child_lists_that_gained_boxes.push((root, new_top_rows));
    finish_planned_list(host, state, root, &plan);
}

/// Takes the entries `place.removed_entries` out of the child list of `root`, the box of `element`, where
/// `loses_entries_in_place` holds: the boxes beside each run of them become neighbors, and the boxes leave as the
/// boxes a walk of the list no longer plans do (see `drop_list_rows`).
fn remove_entries_in_place(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    root: LayoutNode,
    element: StyleNodeID,
    place: &UnitPlace,
) {
    let children_were_inline = node_facts::has_flag(host.data(root), NodeFlag::ChildrenAreInline);
    for &entry in &place.removed_entries {
        let arena = host.arena_mut();
        let previous = arena.data(entry).previous_sibling.get();
        let next = arena.data(entry).next_sibling.get();
        if previous.is_invalid() {
            arena.write_shape(root).set_first_child(next);
        } else {
            arena.write_shape(previous).set_next_sibling(next);
        }
        if next.is_invalid() {
            arena.write_shape(root).set_last_child(previous);
        } else {
            arena.write_shape(next).set_previous_sibling(previous);
        }
    }
    let dropped: Vec<OldRow> = place
        .removed_entries
        .iter()
        .map(|&slot| OldRow {
            slot,
            parent: root,
            is_wrapper: false,
        })
        .collect();
    // NB: No box of the list takes the place of one that leaves, and their nodes left the document.
    drop_list_rows(host, &dropped);
    // NB: A list without boxes holds no inline content, as a fresh walk of it finds.
    let emptied = host.data(root).first_child.get().is_invalid();
    if emptied && children_were_inline {
        host.arena_mut().set_node_flag(root, NodeFlag::ChildrenAreInline, false);
    }
    let change = if emptied && children_were_inline {
        ListChange::Restructured
    } else {
        ListChange::RemovedOnly
    };
    invalidate_layout_for_list_change(host, state, root, change, &[]);
    if kept_box_verification_is_enabled() || cfg!(debug_assertions) {
        verify_list_edited_in_place(host, root, element, place);
    }
    host.arena_mut().layout_tree_update_marks_mut().merge(element, false, 0);
}

/// Checks that the child list of `root`, the box of `element`, edited in place (see `remove_entries_in_place` and
/// `insert_planned_entries_in_place`), is the list a walk of it plans: one that keeps every box the list has, and
/// moves no counter or quote.
fn verify_list_edited_in_place(
    host: &mut TreeBuilderHost<'_>,
    root: LayoutNode,
    element: StyleNodeID,
    place: &UnitPlace,
) {
    let unit = UnitPlanning {
        keeps_pseudo_element_boxes: true,
        fresh_from_entry: u32::MAX,
        verifies_kept_boxes: true,
        window: None,
    };
    let plan = plan_child_list(host, root, element, unit, place);
    let arena = host.arena();
    let (planned, _) = read_planned_list(arena, &plan, root);
    let old_rows = read_old_rows_from(arena, arena.data(root).first_child.get());
    let keeps_every_box = planned.len() == old_rows.len()
        && planned.iter().zip(&old_rows).all(|(planned, old)| match planned.role {
            PlannedRole::Kept => planned.slot == old.slot,
            PlannedRole::Wrapper => old.is_wrapper && wrapper_matches(arena, old.slot, planned),
            PlannedRole::New => false,
        });
    let plans_inline_children = plan.tree.node(root).has_flag(NodeFlag::ChildrenAreInline);
    let has_inline_children = node_facts::has_flag(arena.data(root), NodeFlag::ChildrenAreInline);
    let moves_generated_content = first_entry_after_changed_generated_content(&*host, element, &plan).is_some();
    assert!(
        keeps_every_box && plans_inline_children == has_inline_children && !moves_generated_content,
        "a child list edited in place is the list a walk plans: planned {:?}, rows {:?}, inline {} / {}",
        planned
            .iter()
            .map(|row| (row.kind, row.role == PlannedRole::Kept))
            .collect::<Vec<_>>(),
        old_rows
            .iter()
            .map(|row| {
                let data = arena.data(row.slot);
                (data.kind.get(), data.generated_for.get(), data.flags.get())
            })
            .collect::<Vec<_>>(),
        plans_inline_children,
        has_inline_children,
    );
    let free_slots = plan.discard();
    host.arena_mut().give_back_free_slots(free_slots);
}

/// Whether `LIBWEB_VERIFY_KEPT_LAYOUT_BOXES=1` asks the walk to check every box it keeps against a fresh build, read once.
fn kept_box_verification_is_enabled() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ENABLED.get_or_init(|| std::env::var("LIBWEB_VERIFY_KEPT_LAYOUT_BOXES").is_ok_and(|value| value == "1"))
}

/// What the quotes of a child list planned again read of the boxes the builds before made: the depth before the list,
/// from the walk of the document the build goes along, and what each box the list keeps does to it.
struct UnitQuotes<'a> {
    arena: &'a LayoutNodeArena,
    cursor: &'a mut Option<crate::layout::quote_depth::QuoteDepthCursor>,
    /// The node the walk of the list starts at, or none where the list is the viewport's, which nothing precedes.
    entry: Option<StyleNodeID>,
    /// Whether the walk of the document is past the list already, which only a build of the whole tree tells again.
    lost_its_place: bool,
}

impl crate::layout::tree_plan::content_items::RetainedQuotes for UnitQuotes<'_> {
    fn entry_depth(&mut self) -> u32 {
        let Some(entry) = self.entry else {
            return 0;
        };
        if let Some(depth) = self
            .cursor
            .as_mut()
            .and_then(|cursor| cursor.depth_before_node(self.arena, entry))
        {
            return depth;
        }
        *self.cursor = None;
        self.lost_its_place = true;
        0
    }

    fn after_kept_element(&mut self, element: StyleNodeID, depth: u32) -> u32 {
        crate::layout::quote_depth::QuoteDepthCursor::depth_after_subtree(self.arena, element, depth)
    }

    fn after_kept_pseudo_element(&mut self, element: StyleNodeID, generated_for: u8, depth: u32) -> u32 {
        crate::layout::quote_depth::pseudo_element_quote_transfer(self.arena, element, generated_for).apply(depth)
    }
}

/// Where the walk plans the child list of `root`, the box of `element`, from, where the list only gained children at
/// its end, or inside the run of inline content its last anonymous box wraps: the last child that has a box, whose box
/// goes last among the boxes of the list, or the first child whose box that anonymous box holds. Nothing before it asks
/// to build again. The list's own pseudo-element boxes and the anonymous boxes of a box that lays its contents out in
/// some need the whole list, as do the lists of shadow hosts, slots, tables and SVG.
// OPTIMIZATION: A list that gains children at its end then costs what it gains, and what its last run of inline content
//               holds, not what the list holds.
fn append_window(
    host: &TreeBuilderHost<'_>,
    root: LayoutNode,
    element: StyleNodeID,
    reasons: u8,
    marked_children: &[StyleNodeID],
) -> Option<ListWindow> {
    let arena = host.arena();
    if reasons != layout_tree_update_reuse_reason::CHILD_LIST_INSERTION {
        return None;
    }
    let root_data = arena.data(root);
    let root_kind = root_data.kind.get();
    if !matches!(
        root_kind,
        NodeKind::BlockContainer | NodeKind::Box | NodeKind::ListItemBox | NodeKind::InlineNode
    ) || node_facts::has_flag(root_data, NodeFlag::UsesButtonLayout)
        || is_tabular_container(host, root)
        || !arena
            .bound_pseudo_element_row(element, GENERATED_FOR_AFTER)
            .is_invalid()
    {
        return None;
    }
    arena.with_style_store(|engine| {
        if engine.shadow_root_of(element).is_some()
            || engine.assigned_node_count(element) != 0
            || engine.element_adjustment_facts(element)
                & (element_adjustment_fact::IS_SVG_ELEMENT | element_adjustment_fact::IS_SVG_SWITCH_ELEMENT)
                != 0
            || engine
                .element_published_box_facts(element)
                .is_none_or(|facts| facts.content_visibility != content_visibility::VISIBLE)
            || arena
                .writing_suggestion()
                .is_some_and(|suggestion| engine.text_parent(suggestion.text) == Some(element))
        {
            return None;
        }
        let is_in_flow = |row: NodeSlotId| {
            !arena
                .style_view(row)
                .is_some_and(|style| style.is_floating() || style.is_absolutely_positioned())
        };
        // NB: The window starts at the child whose box is the last of the list, or, where that is an anonymous box
        //     wrapping a run of inline content after a block of the list, at the child whose box the run starts with:
        //     the walk plans the run again in an anonymous box that takes that one over. The children after it are in
        //     the run or inserted, each of which holds a mark. A child without a box and without a mark, such as white
        //     space whose parent changed how it collapses, may gain one where the window does not reach.
        let marks = arena.layout_tree_update_marks();
        let last = root_data.last_child.get();
        if last.is_invalid() {
            return None;
        }
        let last_data = arena.data(last);
        let wrapper = if node_facts::has_flag(last_data, NodeFlag::Anonymous) {
            let previous = last_data.previous_sibling.get();
            // NB: The run follows an in-flow block, after which a fresh walk starts a run as it starts a list.
            if !matches!(root_kind, NodeKind::BlockContainer | NodeKind::ListItemBox)
                || last_data.kind.get() != NodeKind::BlockContainer
                || !node_facts::has_flag(last_data, NodeFlag::ChildrenAreInline)
                || !arena.is_list_closure_wrapper(last)
                || previous.is_invalid()
                || node_facts::has_flag(arena.data(previous), NodeFlag::Anonymous)
                || !is_in_flow(previous)
            {
                return None;
            }
            Some(last)
        } else {
            None
        };
        let entry_top = match wrapper {
            Some(wrapper) => arena.data(wrapper).first_child.get(),
            None => last,
        };
        if entry_top.is_invalid() || node_facts::has_flag(arena.data(entry_top), NodeFlag::Anonymous) {
            return None;
        }
        let anchor = arena.dom_node_style_node(entry_top)?;
        let parent_of = |node: StyleNodeID| match node.text_index() {
            Some(_) => engine.text_parent(node),
            None => engine.dom_parent(node),
        };
        let row = arena.bound_row(anchor);
        if parent_of(anchor) != Some(element) || row.is_invalid() || arena.list_entry_top_row(row) != entry_top {
            return None;
        }
        let mut in_window = HashSet::default();
        let mut gained = false;
        let mut child = engine.next_sibling_in_dom_order(anchor);
        while let Some(current) = child {
            let current_row = arena.bound_row(current);
            if current_row.is_invalid() {
                // NB: In the run, a child without a box may be one without a box of its own, such as a hidden one.
                if marks.needs(current) {
                    gained = true;
                } else if wrapper.is_none() {
                    return None;
                }
            } else {
                let current_top = arena.list_entry_top_row(current_row);
                if wrapper.is_none_or(|wrapper| arena.data(current_top).parent.get() != wrapper) {
                    return None;
                }
            }
            in_window.insert(current);
            child = engine.next_sibling_in_dom_order(current);
        }
        if !gained || (anchor.text_index().is_some() && engine.text_is_ascii_whitespace(anchor)) {
            return None;
        }
        // NB: The walk does not keep the box of a text with a source range, as a `::first-letter` split leaves.
        if anchor.text_index().is_some() && arena.text_has_source_range(row) {
            return None;
        }
        if marks.needs(anchor) && marks.reuse_reasons(anchor) & !layout_tree_update_reuse_reason::RESTYLE == 0 {
            return None;
        }
        // NB: An in-flow box tells the walk what the boxes of the list before it are: inline ones where it is inline,
        //     or blocks. An out-of-flow one tells it nothing.
        if !is_in_flow(row) {
            return None;
        }
        // NB: A child before the window whose box goes, or that gained one, needs the whole list. The marks name every
        //     such child, and every node flattened into the list, whose box goes where it is built again or restyled,
        //     whatever else its list was marked for, and every node whose boxes left the list for another one, such as
        //     the viewport's for the top layer, whichever list answered its mark.
        let earlier_child_changed = marked_children.iter().any(|&child| {
            let reasons = marks.reuse_reasons(child);
            child != anchor
                && !in_window.contains(&child)
                && (arena.layout_parent_with_box(child) != Some(element)
                    || (marks.needs(child)
                        && (reasons == 0
                            || reasons & layout_tree_update_reuse_reason::RESTYLE != 0
                            || arena.bound_row(child).is_invalid())))
        });
        (!earlier_child_changed).then_some(ListWindow {
            start: Some(anchor),
            wrapper: wrapper.unwrap_or(NodeSlotId::INVALID),
            end: None,
        })
    })
}

/// Whether the child list of `root`, the box of `element`, may change in part, planned from a window of it or edited in
/// place, as far as the list's box tells: it is a block container, a box, a list item or an inline box, whose list holds
/// blocks or inline content, and no button, part of a table, shadow host, slot, SVG element or element whose contents
/// are skipped, nor the parent of the text a writing suggestion is in. The anonymous boxes of a box that lays its
/// contents out in some, and the lists of shadow hosts and slots, which hold nodes of other parents, need the whole list.
fn list_may_change_in_part(host: &TreeBuilderHost<'_>, root: LayoutNode, element: StyleNodeID) -> bool {
    let arena = host.arena();
    let root_data = arena.data(root);
    let children_are_inline = node_facts::has_flag(root_data, NodeFlag::ChildrenAreInline);
    if !matches!(
        root_data.kind.get(),
        NodeKind::BlockContainer | NodeKind::Box | NodeKind::ListItemBox | NodeKind::InlineNode
    ) || (root_data.kind.get() == NodeKind::InlineNode && !children_are_inline)
        || node_facts::has_flag(root_data, NodeFlag::UsesButtonLayout)
        || is_tabular_container(host, root)
    {
        return false;
    }
    arena.with_style_store(|engine| {
        engine.shadow_root_of(element).is_none()
            && engine.assigned_node_count(element) == 0
            && engine.element_adjustment_facts(element)
                & (element_adjustment_fact::IS_SVG_ELEMENT | element_adjustment_fact::IS_SVG_SWITCH_ELEMENT)
                == 0
            && engine
                .element_published_box_facts(element)
                .is_some_and(|facts| facts.content_visibility == content_visibility::VISIBLE)
            && arena
                .writing_suggestion()
                .is_none_or(|suggestion| engine.text_parent(suggestion.text) != Some(element))
    })
}

/// Where a change of the list of `root`, the box of `element`, lies in it: at a box of the list, right after one, or
/// before every box of it.
#[derive(Clone, Copy)]
enum ListChangeAt {
    Box(NodeSlotId),
    After(NodeSlotId),
    Start,
}

/// The part of the child list of `root`, the box of `element`, that the walk plans, where the list only gained or lost
/// children: from the last box the list keeps before the changes that is like the boxes of the list, blocks in a list
/// of blocks or inline-level boxes in a list of inline content, both in flow, or from the start of the list where none
/// is, to the first such box after them, or to the end of the list. What a fresh walk decides after such a box reads
/// nothing before it: a block ends a run of inline content, and white space after an inline-level box reads that box
/// alone. A block that goes among inline content wraps the boxes before the window, which the walk notes, and a change
/// that moves a counter or a quote depth moves them for the boxes after the window, which the plan tells: the list is
/// then planned whole. A new list item moves the `list-item` value of the list items after it, which the plan does not
/// tell. The lists that an append window cannot have need the whole list, as does a window that would run from the
/// start of the list to its end.
// OPTIMIZATION: A list that gains or loses a child among others then costs what changed, not what the list holds.
fn bounded_window(
    host: &TreeBuilderHost<'_>,
    root: LayoutNode,
    element: StyleNodeID,
    reasons: u8,
    marked_children: &[StyleNodeID],
) -> Option<ListWindow> {
    use layout_tree_update_reuse_reason::{CHILD_LIST_INSERTION, CHILD_LIST_REMOVAL};
    let arena = host.arena();
    if reasons == 0 || reasons & !(CHILD_LIST_INSERTION | CHILD_LIST_REMOVAL) != 0 {
        return None;
    }
    if !list_may_change_in_part(host, root, element) {
        return None;
    }
    let root_data = arena.data(root);
    let children_are_inline = node_facts::has_flag(root_data, NodeFlag::ChildrenAreInline);
    let marks = arena.layout_tree_update_marks();
    arena.with_style_store(|engine| {
        let parent_of = |node: StyleNodeID| match node.text_index() {
            Some(_) => engine.text_parent(node),
            None => engine.dom_parent(node),
        };
        // NB: The box a row of the list is, or is in, among the children of `root`.
        let child_of_root = |row: NodeSlotId| {
            let mut child = arena.list_entry_top_row(row);
            loop {
                let parent = arena.data(child).parent.get();
                if parent == root {
                    return Some(child);
                }
                if parent.is_invalid() || !arena.is_list_closure_wrapper(parent) {
                    return None;
                }
                child = parent;
            }
        };
        // NB: What the new boxes do to counters and quotes, and what the boxes that go did, is read once the walk
        //     planned them, see `first_entry_after_changed_generated_content`.
        let marked: HashSet<StyleNodeID> = marked_children.iter().copied().collect();
        let mut changes = Vec::new();
        for &child in marked_children {
            // NB: A node flattened into the list, such as a child of a `display: contents` child, needs the whole list.
            if parent_of(child) != Some(element) {
                return None;
            }
            if child.element_index().is_some()
                && (marks.generated_content_changed(child)
                    || marks.list_item_changed(child)
                    || engine
                        .element_published_box_facts(child)
                        .is_some_and(|facts| facts.display.is_list_item()))
            {
                return None;
            }
            let row = arena.bound_row(child);
            if !row.is_invalid()
                && let Some(box_of_row) = child_of_root(row)
            {
                changes.push(ListChangeAt::Box(box_of_row));
            }
            // NB: The new box goes after the box of the last child before it that has one in the list, or first in the
            //     list where none has one. A marked child before it without one finds that box itself.
            let mut previous = engine.previous_sibling_in_dom_order(child);
            loop {
                let Some(current) = previous else {
                    changes.push(ListChangeAt::Start);
                    break;
                };
                let row = arena.bound_row(current);
                if let Some(box_of_row) = (!row.is_invalid()).then(|| child_of_root(row)).flatten() {
                    changes.push(ListChangeAt::After(box_of_row));
                    break;
                }
                if marked.contains(&current) {
                    break;
                }
                previous = engine.previous_sibling_in_dom_order(current);
            }
        }
        // NB: The boxes of the nodes that left the list are still in it, and no mark names them.
        if reasons & CHILD_LIST_REMOVAL != 0 {
            for row in read_old_rows_from(arena, root_data.first_child.get()) {
                // NB: The list's own pseudo-element boxes go first and last in it, outside every window.
                let data = arena.data(row.slot);
                if row.is_wrapper || data.generated_for.get() != 0 {
                    continue;
                }
                let stays = !node_facts::has_flag(data, NodeFlag::Anonymous)
                    && arena.dom_node_style_node(row.slot).is_some_and(|node| {
                        arena.bound_row(node) == row.slot
                            && (parent_of(node) == Some(element) || arena.layout_parent_with_box(node) == Some(element))
                    });
                if stays {
                    continue;
                }
                changes.push(ListChangeAt::Box(child_of_root(row.slot)?));
            }
        }
        if changes.is_empty() {
            return None;
        }
        let changed_boxes: HashSet<NodeSlotId> = changes
            .iter()
            .filter_map(|change| match *change {
                ListChangeAt::Box(row) => Some(row),
                ListChangeAt::After(_) | ListChangeAt::Start => None,
            })
            .collect();
        // NB: A box in flow like the boxes of the list that the list keeps: the box of a child element of `element`, a
        //     child of `root`.
        let bounds = |row: NodeSlotId| {
            let data = arena.data(row);
            !node_facts::has_flag(data, NodeFlag::Anonymous)
                && data.generated_for.get() == 0
                && !changed_boxes.contains(&row)
                && arena.style_view(row).is_some_and(|style| {
                    let display = style.display();
                    (if children_are_inline {
                        display.is_inline_outside()
                    } else {
                        display.is_block_outside()
                    }) && !style.is_floating()
                        && !style.is_absolutely_positioned()
                })
                && arena.dom_node_style_node(row).is_some_and(|node| {
                    node.element_index().is_some()
                        && arena.bound_row(node) == row
                        && !marks.needs(node)
                        && engine.dom_parent(node) == Some(element)
                })
        };
        // NB: The nearest such box before the first change goes before every change, and the nearest one after the last
        //     change after every change, so the window runs between those two. It runs from the start of the list
        //     where no such box goes before the first change, and to its end where none goes after the last one.
        let position = |change: &ListChangeAt| match *change {
            ListChangeAt::Start => (0, 0, 0),
            ListChangeAt::Box(row) => (1, arena.node_pre_order_label(row), 0),
            ListChangeAt::After(row) => (1, arena.node_pre_order_label(row), 1),
        };
        let mut before = match changes.iter().min_by_key(|change| position(change))? {
            ListChangeAt::Box(row) => arena.data(*row).previous_sibling.get(),
            ListChangeAt::After(row) => *row,
            ListChangeAt::Start => NodeSlotId::INVALID,
        };
        let mut after = match changes.iter().max_by_key(|change| position(change))? {
            ListChangeAt::Box(row) | ListChangeAt::After(row) => arena.data(*row).next_sibling.get(),
            ListChangeAt::Start => root_data.first_child.get(),
        };
        while !before.is_invalid() && !bounds(before) {
            before = arena.data(before).previous_sibling.get();
        }
        while !after.is_invalid() && !bounds(after) {
            after = arena.data(after).next_sibling.get();
        }
        let (from_start, to_end) = (before.is_invalid(), after.is_invalid());
        // NB: The list's own pseudo-element boxes go first and last in it, and stay as they are, so a window that runs
        //     from the start of the list or to its end needs a list without them there. A window that runs from the
        //     start to the end is the whole list.
        let has_pseudo_element_box =
            |generated_for: u8| !arena.bound_pseudo_element_row(element, generated_for).is_invalid();
        if (from_start && to_end)
            || (from_start
                && (has_pseudo_element_box(GENERATED_FOR_BEFORE) || has_pseudo_element_box(GENERATED_FOR_MARKER)))
            || (to_end && has_pseudo_element_box(GENERATED_FOR_AFTER))
        {
            return None;
        }
        let start = if from_start {
            None
        } else {
            Some(arena.dom_node_style_node(before)?)
        };
        let end = if to_end {
            None
        } else {
            Some(arena.dom_node_style_node(after)?)
        };
        Some(ListWindow {
            start,
            wrapper: NodeSlotId::INVALID,
            end,
        })
    })
}

fn plan_child_list(
    host: &mut TreeBuilderHost<'_>,
    root: LayoutNode,
    element: StyleNodeID,
    unit: UnitPlanning,
    place: &UnitPlace,
) -> SubtreePlan {
    let free_slots = host.arena_mut().take_free_slots();
    let arena = host.arena();
    arena.with_style_store(|engine| {
        Planner::plan_unit(
            engine,
            arena,
            root,
            element,
            place.is_viewport,
            unit,
            PlanContext {
                has_svg_root: place.has_svg_root,
                enclosing_ancestor_rows: &place.ancestor_rows,
                free_slots,
            },
        )
    })
}

/// The entry of the list from which every entry has to be planned afresh, where a new element before a kept box sets,
/// resets or increments a counter or opens or closes a quote: the entry after the first such element, or every entry
/// where a counter that counts down from its last item is reset in the list, whose start value counts the items before
/// the element too. An element whose box is built again only changes that where its style changed it, and a list item
/// box only changes what a kept box after it shows the `list-item` counter value of.
fn first_entry_after_changed_generated_content(
    host: &TreeBuilderHost<'_>,
    element: StyleNodeID,
    plan: &SubtreePlan,
) -> Option<u32> {
    let last_kept_entry = plan.last_kept_entry?;
    let arena = host.arena();
    let marks = arena.layout_tree_update_marks();
    arena.with_style_store(|engine| {
        // NB: Read only for a list item, as it reads every child of the list.
        let resets_reversed_counter = std::cell::OnceCell::new();
        let resets_reversed_counter = || {
            *resets_reversed_counter.get_or_init(|| {
                (element.element_index().is_some() && engine.element_counter_reset_has_reversed_counter(element))
                    || engine.dom_children(element).any(|child| {
                        child.element_index().is_some() && engine.element_counter_reset_has_reversed_counter(child)
                    })
            })
        };
        // NB: Read once, as it reads the boxes of the kept entries.
        let last_showing = std::cell::OnceCell::new();
        let kept_entries_show_counter_values = |from: u32| {
            last_showing
                .get_or_init(|| last_kept_entry_showing_counter_value(arena, element, plan))
                .is_some_and(|last| last >= from)
        };
        let mut first = None;
        let mut increments_or_sets_counters = false;
        for &(entry, child) in &plan.fresh_element_entries {
            let Some(facts) = engine
                .element_published_box_facts(child)
                .filter(|facts| !facts.display.is_none())
            else {
                continue;
            };
            // NB: The box built again, or restyled and not kept, does to what follows what the old one did, unless the
            //     style changed it.
            let reasons = marks.reuse_reasons(child);
            if !arena.bound_row(child).is_invalid()
                && marks.needs(child)
                && (reasons == 0 || reasons & layout_tree_update_reuse_reason::RESTYLE != 0)
            {
                if marks.generated_content_changed(child)
                    || (marks.list_item_changed(child)
                        && (resets_reversed_counter() || kept_entries_show_counter_values(entry)))
                {
                    first.get_or_insert(entry);
                    increments_or_sets_counters = true;
                }
                continue;
            }
            // NB: The children of a `display: contents` element are entries of the list of their own.
            if facts.display.is_contents() {
                if engine
                    .published_style_view(child, None)
                    .is_some_and(|style| style.affects_generated_content_state())
                {
                    first.get_or_insert(entry);
                    increments_or_sets_counters = true;
                }
                continue;
            }
            let effects = engine.subtree_generated_content_effects(child);
            // NB: A list item counts down the items before it in a reversed list.
            let increments = effects.increments_or_sets_counters
                || (facts.display.is_list_item()
                    && (resets_reversed_counter() || kept_entries_show_counter_values(entry)));
            if effects.has_element_with_unbalanced_quotes || effects.resets_counters || increments {
                first.get_or_insert(entry);
                increments_or_sets_counters |= increments;
            }
        }
        let first = first?;
        if increments_or_sets_counters && resets_reversed_counter() {
            return Some(0);
        }
        (first < last_kept_entry).then_some(first + 1)
    })
}

/// The first entry the list keeps after a box it no longer has that moved a counter or a quote depth, or was a list
/// item a kept box after it shows the `list-item` counter value of: the box of a node that left, or that has none now,
/// or of one whose style changed that. Another box built again for its node is the new entry's to answer for. A list
/// planned from `window` loses boxes inside it alone.
fn first_entry_after_dropped_generated_content(
    host: &TreeBuilderHost<'_>,
    root: LayoutNode,
    element: StyleNodeID,
    plan: &SubtreePlan,
    window: Option<ListWindow>,
) -> Option<u32> {
    let arena = host.arena();
    // NB: A window that runs to the end of a list that lost no children only gains boxes.
    if window.is_some_and(|window| window.end.is_none())
        && arena.layout_tree_update_reuse_reasons(element) & layout_tree_update_reuse_reason::CHILD_LIST_REMOVAL == 0
    {
        return None;
    }
    let last_kept = plan.kept_entries.last()?.1;
    let marks = arena.layout_tree_update_marks();
    let last_kept_label = arena.node_pre_order_label(last_kept);
    let last_showing = std::cell::OnceCell::new();
    let kept_entries_show_counter_values = |from: u32| {
        last_showing
            .get_or_init(|| last_kept_entry_showing_counter_value(arena, element, plan))
            .is_some_and(|last| last >= from)
    };
    let old_rows = match window {
        Some(window) => read_old_rows_between(
            arena,
            window
                .start
                .map_or_else(|| arena.data(root).first_child.get(), |start| arena.bound_row(start)),
            window.end.map_or(NodeSlotId::INVALID, |end| arena.bound_row(end)),
        ),
        None => read_old_rows_from(arena, arena.data(root).first_child.get()),
    };
    let first_dropped_label = old_rows
        .into_iter()
        .filter(|row| {
            if row.is_wrapper
                || plan.tree.stands_in_for(row.slot)
                || arena.node_pre_order_label(row.slot) >= last_kept_label
            {
                return false;
            }
            let node = arena.dom_node_style_node(row.slot);
            let label = arena.node_pre_order_label(row.slot);
            // NB: The boxes after a window may show counter values the plan does not see.
            let kept_after_show_counter_values = || {
                window.is_some()
                    || plan
                        .kept_entries
                        .iter()
                        .find(|&&(_, kept)| arena.node_pre_order_label(kept) > label)
                        .is_some_and(|&(entry, _)| kept_entries_show_counter_values(entry))
            };
            if node.is_some_and(|node| marks.generated_content_changed(node)) {
                return true;
            }
            if node.is_some_and(|node| marks.list_item_changed(node)) && kept_after_show_counter_values() {
                return true;
            }
            // NB: A node that has no box now leaves with everything its box did.
            if node.is_some_and(|node| {
                plan.fresh_element_entries.iter().any(|&(_, fresh)| fresh == node)
                    && arena.with_style_store(|engine| {
                        engine
                            .element_published_box_facts(node)
                            .is_some_and(|facts| !facts.display.is_none())
                    })
            }) {
                return false;
            }
            match box_subtree_numbering(arena, row.slot) {
                BoxSubtreeNumbering::None => false,
                BoxSubtreeNumbering::CountersOrQuotes => true,
                BoxSubtreeNumbering::ListItems => kept_after_show_counter_values(),
            }
        })
        .map(|row| arena.node_pre_order_label(row.slot))
        .next()?;
    plan.kept_entries
        .iter()
        .find(|&&(_, row)| arena.node_pre_order_label(row) > first_dropped_label)
        .map(|&(entry, _)| entry)
}

/// What a box subtree does to the numbering of what follows it.
enum BoxSubtreeNumbering {
    None,
    /// It holds list items, and changes no counter otherwise.
    ListItems,
    /// It moves a counter or a quote depth.
    CountersOrQuotes,
}

fn box_subtree_numbering(arena: &LayoutNodeArena, root: NodeSlotId) -> BoxSubtreeNumbering {
    let mut numbering = BoxSubtreeNumbering::None;
    arena.for_each_node_in_layout_subtree_in_pre_order(root, |row| {
        let Some(style) = node_facts::node_style_view(arena.data(row)) else {
            return;
        };
        if style.affects_generated_content_state() {
            numbering = BoxSubtreeNumbering::CountersOrQuotes;
        } else if style.display().is_list_item() && matches!(numbering, BoxSubtreeNumbering::None) {
            numbering = BoxSubtreeNumbering::ListItems;
        }
    });
    numbering
}

/// The last entry of the list whose kept box shows the value of a counter, which the `list-item` counter value a list
/// item before it moves, or `u32::MAX` where the list's own `::after` box shows one. The boxes of the entries from the
/// entry `from` on show one where this is `from` or later.
fn last_kept_entry_showing_counter_value(
    arena: &LayoutNodeArena,
    element: StyleNodeID,
    plan: &SubtreePlan,
) -> Option<u32> {
    use crate::layout::counters::CounterOwner;
    let generated_content = arena.generated_content();
    if generated_content.shows_counter_value(CounterOwner {
        element,
        generated_for: crate::layout::node_data::GENERATED_FOR_AFTER,
    }) {
        return Some(u32::MAX);
    }
    let shows_counter_value = |row: NodeSlotId| {
        let generated_for = arena.data(row).generated_for.get();
        generated_for != 0
            && arena
                .node_style_node(row)
                .is_some_and(|element| generated_content.shows_counter_value(CounterOwner { element, generated_for }))
    };
    plan.kept_entries.iter().rev().find_map(|&(entry, kept)| {
        let mut shows = false;
        arena.for_each_node_in_layout_subtree_in_pre_order(kept, |row| shows |= shows_counter_value(row));
        shows.then_some(entry)
    })
}

/// What a row of the planned list is.
#[derive(Clone, Copy, PartialEq, Eq)]
enum PlannedRole {
    /// The box of an entry the list keeps.
    Kept,
    /// An anonymous box the list makes around some of its entries.
    Wrapper,
    /// The new box of an entry.
    New,
}

/// A row of the planned list, as the plan links it.
#[derive(Clone, Copy)]
struct PlannedRow {
    slot: NodeSlotId,
    role: PlannedRole,
    depth: u32,
    parent: NodeSlotId,
    previous_sibling: NodeSlotId,
    next_sibling: NodeSlotId,
    first_child: NodeSlotId,
    last_child: NodeSlotId,
    flags: u32,
    facts: u32,
    kind: NodeKind,
    /// For a kept box, the box it was a child of.
    old_parent: NodeSlotId,
}

/// The rows of the planned list in pre-order: the children of the list's box, and those of the anonymous boxes the list
/// makes, down to the box of each entry.
fn read_planned_list(
    arena: &LayoutNodeArena,
    plan: &SubtreePlan,
    root: LayoutNode,
) -> (Vec<PlannedRow>, (NodeSlotId, NodeSlotId)) {
    let tree = &plan.tree;
    let root_box = tree.node(root);
    let mut rows = Vec::new();
    // NB: The children of a box are pushed last to first, so that the walk visits them first to last.
    let mut stack: Vec<(NodeSlotId, u32)> = Vec::new();
    let mut child = root_box.last_child();
    while !child.is_invalid() {
        stack.push((child, 0));
        child = tree.node(child).previous_sibling();
    }
    while let Some((slot, depth)) = stack.pop() {
        let planned = tree.node(slot);
        let role = if tree.is_stand_in(slot) {
            PlannedRole::Kept
        } else if planned.has_flag(NodeFlag::Anonymous)
            && planned.generated_for() == 0
            && planned.kind() == NodeKind::BlockContainer
        {
            PlannedRole::Wrapper
        } else {
            PlannedRole::New
        };
        // NB: A box the list keeps stands in for a box whose children stay as they are.
        assert!(
            role != PlannedRole::Kept || planned.first_child().is_invalid(),
            "a child list planned again placed boxes below a box it keeps: kept {:?} {:?} child {:?} flags {:x} root {:?}",
            planned.kind(),
            arena.dom_node_style_node(slot),
            tree.node(planned.first_child()).kind(),
            tree.node(planned.first_child()).flags(),
            arena.data(root).kind.get(),
        );
        rows.push(PlannedRow {
            slot,
            role,
            depth,
            parent: planned.parent(),
            previous_sibling: planned.previous_sibling(),
            next_sibling: planned.next_sibling(),
            first_child: planned.first_child(),
            last_child: planned.last_child(),
            flags: planned.flags(),
            facts: planned.facts(),
            kind: planned.kind(),
            old_parent: if role == PlannedRole::Kept {
                arena.data(slot).parent.get()
            } else {
                NodeSlotId::INVALID
            },
        });
        if role == PlannedRole::Wrapper {
            let mut grandchild = planned.last_child();
            while !grandchild.is_invalid() {
                stack.push((grandchild, depth + 1));
                grandchild = tree.node(grandchild).previous_sibling();
            }
        }
    }
    (rows, (root_box.first_child(), root_box.last_child()))
}

/// A row of the list as it was, with the box it was a child of.
#[derive(Clone, Copy)]
struct OldRow {
    slot: NodeSlotId,
    parent: NodeSlotId,
    is_wrapper: bool,
}

/// The rows of the list as it was from its row `start` on, in pre-order: `start` and the rows after it, and the children
/// of the anonymous boxes the list made among them, see [`LayoutNodeArena::is_list_closure_wrapper`].
fn read_old_rows_from(arena: &LayoutNodeArena, start: NodeSlotId) -> Vec<OldRow> {
    read_old_rows_between(arena, start, NodeSlotId::INVALID)
}

/// The rows of the list as it was from its row `start` up to its row `last`, or to its end where `last` is invalid, as
/// [`read_old_rows_from`] reads them.
fn read_old_rows_between(arena: &LayoutNodeArena, start: NodeSlotId, last: NodeSlotId) -> Vec<OldRow> {
    let mut rows = Vec::new();
    let mut stack = Vec::new();
    let push_children_down_to = |stack: &mut Vec<NodeSlotId>, last: NodeSlotId, first: NodeSlotId| {
        let mut child = last;
        while !child.is_invalid() {
            stack.push(child);
            if child == first {
                break;
            }
            child = arena.data(child).previous_sibling.get();
        }
    };
    if !start.is_invalid() {
        let parent = arena.data(start).parent.get();
        let last = if last.is_invalid() {
            arena.data(parent).last_child.get()
        } else {
            last
        };
        push_children_down_to(&mut stack, last, start);
    }
    while let Some(slot) = stack.pop() {
        let is_wrapper = arena.is_list_closure_wrapper(slot);
        rows.push(OldRow {
            slot,
            parent: arena.data(slot).parent.get(),
            is_wrapper,
        });
        if is_wrapper {
            push_children_down_to(&mut stack, arena.data(slot).last_child.get(), NodeSlotId::INVALID);
        }
    }
    rows
}

/// The facts of a box's display that tell an anonymous box of a list from another.
const WRAPPER_KEY_FACTS: u32 = box_fact::INLINE_OUTSIDE
    | box_fact::FLOW_INSIDE
    | box_fact::FLEX_OR_GRID_INSIDE
    | box_fact::TABLE_INSIDE
    | box_fact::TABULAR_CONTAINER
    | box_fact::TABLE_NON_ROOT;

fn wrapper_matches(arena: &LayoutNodeArena, old: NodeSlotId, planned: &PlannedRow) -> bool {
    arena.data(old).kind.get() == planned.kind
        && arena.style_view(old).is_some_and(|style| {
            crate::layout::tree_plan::box_facts_of_style(style) & WRAPPER_KEY_FACTS == planned.facts & WRAPPER_KEY_FACTS
        })
}

/// How the child list of a box changed.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum ListChange {
    Unchanged,
    /// New boxes went into the list, and every box it had stays where it was.
    InsertedOnly,
    /// Boxes left the list, and every box it kept stays where it was.
    RemovedOnly,
    Restructured,
}

/// Reconciles the child list of `root` with `plan`, answering for each of the `watched` boxes whether the list changed at
/// or before it in pre-order, or it is not in the tree.
fn reconcile(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    root: LayoutNode,
    mut plan: SubtreePlan,
    window: Option<ListWindow>,
    watched: &[NodeSlotId],
) -> Vec<bool> {
    let (planned, (planned_first_child, planned_last_child)) = read_planned_list(host.arena(), &plan, root);
    let root_flags_before = host.data(root).flags.get();
    // NB: A list planned from a window starts at the box it keeps first, which is the last box the list had, or at
    //     an anonymous box that takes over the last one, which wraps the run of inline content the list ends with, or
    //     at the start of the list. The list keeps the boxes before that as they are.
    let window_wrapper = window.map_or(NodeSlotId::INVALID, |window| window.wrapper);
    let start = match window {
        Some(_) if !window_wrapper.is_invalid() => window_wrapper,
        Some(window) if window.start.is_some() => planned.first().expect("a window starts at a kept box").slot,
        _ => host.data(root).first_child.get(),
    };
    let before_start = if start.is_invalid() {
        NodeSlotId::INVALID
    } else {
        host.data(start).previous_sibling.get()
    };
    // NB: A window that ends at a box the list keeps keeps the boxes after it as they are.
    let end = window
        .and_then(|window| window.end)
        .map_or(NodeSlotId::INVALID, |end| host.arena().bound_row(end));
    let after_end = if end.is_invalid() {
        NodeSlotId::INVALID
    } else {
        host.data(end).next_sibling.get()
    };
    assert!(
        end.is_invalid()
            || planned
                .last()
                .is_some_and(|row| row.slot == end && row.role == PlannedRole::Kept && row.depth == 0),
        "a window ends at a box the list keeps"
    );
    let old_rows = read_old_rows_between(host.arena(), start, end);

    // The anonymous boxes the list makes again take over those it had: the one that held the first box below them the
    // list keeps, under the same parent, or else the next one under that parent, in the order of the list, that is
    // like it.
    let mut taken_over: HashMap<NodeSlotId, NodeSlotId> = HashMap::default();
    let mut taken: HashSet<NodeSlotId> = HashSet::default();
    {
        let arena = host.arena();
        let old_wrappers: HashSet<NodeSlotId> = old_rows
            .iter()
            .filter(|row| row.is_wrapper)
            .map(|row| row.slot)
            .collect();
        // NB: Where the search for the next anonymous box under each parent goes on from, as an index into the old rows.
        let mut next_old_wrapper: HashMap<NodeSlotId, usize> = HashMap::default();
        for (index, wrapper) in planned.iter().enumerate() {
            if wrapper.role != PlannedRole::Wrapper {
                continue;
            }
            let parent = taken_over.get(&wrapper.parent).copied().unwrap_or(wrapper.parent);
            if parent != root && !taken.contains(&parent) {
                continue;
            }
            let first_kept = planned[index + 1..]
                .iter()
                .take_while(|row| row.depth > wrapper.depth)
                .find(|row| row.role == PlannedRole::Kept)
                .map(|row| row.slot);
            let anchored = first_kept
                .and_then(|kept_row| {
                    // NB: The rows are linked as the list had them until the planned links are written below.
                    let mut ancestor = kept_row;
                    loop {
                        let ancestor_parent = arena.data(ancestor).parent.get();
                        if ancestor_parent == parent {
                            return (ancestor != kept_row).then_some(ancestor);
                        }
                        if ancestor_parent == root || !old_wrappers.contains(&ancestor_parent) {
                            return None;
                        }
                        ancestor = ancestor_parent;
                    }
                })
                .filter(|old| {
                    old_wrappers.contains(old) && !taken.contains(old) && wrapper_matches(arena, *old, wrapper)
                });
            let candidate = anchored.or_else(|| {
                let start = next_old_wrapper.entry(parent).or_insert(0);
                let found = old_rows[*start..].iter().position(|old| {
                    old.is_wrapper
                        && old.parent == parent
                        && !taken.contains(&old.slot)
                        && wrapper_matches(arena, old.slot, wrapper)
                })?;
                *start += found + 1;
                Some(old_rows[*start - 1].slot)
            });
            if let Some(old) = candidate {
                taken_over.insert(wrapper.slot, old);
                taken.insert(old);
            }
        }
    }
    assert!(
        window_wrapper.is_invalid() || taken.contains(&window_wrapper),
        "the anonymous box a window starts at takes over the one the list ended with"
    );
    let map = |slot: NodeSlotId| taken_over.get(&slot).copied().unwrap_or(slot);
    let dropped: Vec<OldRow> = old_rows
        .iter()
        .filter(|row| !taken.contains(&row.slot) && !plan.tree.stands_in_for(row.slot))
        .copied()
        .collect();

    // The rows of the plan go to the arena. Nothing below binds them to their nodes yet.
    let descendant_count = adopt_planned_rows(host, &mut plan);

    // Every box the list no longer has goes, before the new boxes are bound to their nodes. A new box for the same node
    // takes over the fragments of the one that goes, as a box replaced in place does.
    // NB: A box that goes and moved the quote depth changes the quotes after its list, which only a build of the whole
    //     tree finds again.
    if dropped
        .iter()
        .any(|row| !row.is_wrapper && crate::layout::quote_depth::box_subtree_moves_quote_depth(host.arena(), row.slot))
    {
        state.asks_for_every_box_again = true;
    }
    // OPTIMIZATION: Absolutely positioned boxes that leave the box that contains them leave the rest of its layout as
    //               it was.
    //               So does an anonymous box that held only such boxes, as an absolutely positioned box that is
    //               inline-level in flow has among block-level siblings.
    let drops_contained_abspos_boxes_only = !dropped.is_empty() && {
        let arena = host.arena();
        let dropped_slots: HashSet<NodeSlotId> = dropped.iter().map(|row| row.slot).collect();
        let is_contained_abspos_box = |row: &OldRow| {
            !row.is_wrapper
                && (row.parent == root || dropped_slots.contains(&row.parent))
                && node_kind_is_node_with_style(arena.data(row.slot).kind.get())
                && node_facts::node_style_view(arena.data(row.slot))
                    .is_some_and(|style| style.position() == crate::css::css_enums::positioning::ABSOLUTE)
                && arena.containing_block_by_walking_ancestors(row.slot) == root
        };
        dropped.iter().all(|row| {
            if !row.is_wrapper {
                return is_contained_abspos_box(row);
            }
            row.parent == root
                && old_rows
                    .iter()
                    .filter(|child| child.parent == row.slot)
                    .all(|child| dropped_slots.contains(&child.slot) && is_contained_abspos_box(child))
        })
    };
    if drops_contained_abspos_boxes_only {
        for row in dropped.iter().filter(|row| !row.is_wrapper) {
            host.arena_mut().note_contained_abspos_child_removal(root, row.slot);
        }
    }
    // NB: Whether the list changed at or before each of the watched boxes, in pre-order: a box the list no longer has
    //     goes before it, or it has none in the tree.
    let mut changed_at_or_before: Vec<bool> = {
        let arena = host.arena();
        watched
            .iter()
            .map(|&row| {
                if row.is_invalid() || !arena.slot_is_live(row) || arena.data(row).parent.get().is_invalid() {
                    return true;
                }
                let label = arena.node_pre_order_label(row);
                dropped.iter().any(|old| arena.node_pre_order_label(old.slot) <= label)
            })
            .collect()
    };
    let replaced_fragments = drop_list_rows(host, &dropped);

    // The list links its rows as the plan does, with the anonymous boxes it had in place of the ones that took them
    // over, whose rows go.
    {
        let arena = host.arena_mut();
        link_planned_list(
            arena,
            root,
            &planned,
            &map,
            (before_start, planned_first_child),
            (end, after_end, planned_last_child),
        );
        for &planned_wrapper in taken_over.keys() {
            let mut shape = arena.write_shape(planned_wrapper);
            shape.set_parent(NodeSlotId::INVALID);
            shape.set_previous_sibling(NodeSlotId::INVALID);
            shape.set_next_sibling(NodeSlotId::INVALID);
            shape.set_first_child(NodeSlotId::INVALID);
            shape.set_last_child(NodeSlotId::INVALID);
        }
    }
    for (&planned_wrapper, &wrapper) in &taken_over {
        host.free_subtree(planned_wrapper);
        // NB: Whether an anonymous box is a flex or grid item depends on what it holds now.
        host.arena_mut().derive_flex_or_grid_item_flags(wrapper);
    }

    let mut old_parents_of_list: HashSet<NodeSlotId> = taken.clone();
    old_parents_of_list.insert(root);
    let unit = UnitCommit {
        taken_over: &taken_over,
        old_parents: &old_parents_of_list,
        kept: &|slot| plan.tree.stands_in_for(slot),
    };
    commit_plan(host, state, root, &plan, descendant_count, false, Some(&unit));

    // A kept box that moved into another box of the list derives what it takes from it, as an attached box does.
    let mut kept_moved = false;
    for row in &planned {
        if row.role != PlannedRole::Kept {
            continue;
        }
        let parent = map(row.parent);
        if row.old_parent != parent {
            kept_moved = true;
            host.arena_mut().note_kept_child_moved(parent, row.slot);
        }
    }
    let first_labeled = if before_start.is_invalid() {
        host.first_child(root)
    } else {
        map(planned[0].slot)
    };
    label_planned_list(host.arena_mut(), root, first_labeled, end, &taken);

    for (node, old_row, link, containing_block) in replaced_fragments {
        let arena = host.arena_mut();
        let new_row = arena.bound_row(node);
        if new_row.is_invalid() || !node_facts::kind_is_box(arena.data(new_row).kind.get()) {
            continue;
        }
        if let Some(link) = link {
            arena.set_committed_fragment_link(new_row, link, None);
        }
        if let Some(containing_block) = containing_block {
            arena.transfer_fragments_to_replacement_node(containing_block, old_row, new_row);
        }
    }

    // What the change invalidates: the new boxes of a list that only gained boxes, or the whole list.
    let new_top_rows: Vec<NodeSlotId> = planned
        .iter()
        .filter(|row| {
            row.role != PlannedRole::Kept
                && !taken_over.contains_key(&row.slot)
                && old_parents_of_list.contains(&map(row.parent))
        })
        .map(|row| row.slot)
        .collect();
    let children_are_inline_changed =
        (root_flags_before ^ host.data(root).flags.get()) & NodeFlag::ChildrenAreInline as u32 != 0;
    let change = if kept_moved || children_are_inline_changed {
        ListChange::Restructured
    } else if drops_contained_abspos_boxes_only && new_top_rows.is_empty() {
        ListChange::Unchanged
    } else if dropped.iter().any(|row| row.is_wrapper || row.parent != root) {
        ListChange::Restructured
    } else if !dropped.is_empty() && new_top_rows.is_empty() {
        ListChange::RemovedOnly
    } else if !dropped.is_empty() {
        ListChange::Restructured
    } else if new_top_rows.is_empty() {
        ListChange::Unchanged
    } else {
        ListChange::InsertedOnly
    };
    // NB: A new box goes before a watched box, or the list moved its boxes around.
    for (changed, &row) in changed_at_or_before.iter_mut().zip(watched) {
        *changed = *changed || change == ListChange::Restructured || !host.arena().slot_is_live(row) || {
            let label = host.arena().node_pre_order_label(row);
            new_top_rows
                .iter()
                .any(|&new_row| host.arena().node_pre_order_label(new_row) <= label)
        };
    }
    // NB: A new anonymous box that holds a kept box, at any depth, is not new as a whole: the kept box noted its move
    //     above.
    let parents_in_plan: HashMap<NodeSlotId, NodeSlotId> = planned.iter().map(|row| (row.slot, row.parent)).collect();
    let mut ancestors_of_kept_rows: HashSet<NodeSlotId> = HashSet::default();
    for row in planned.iter().filter(|row| row.role == PlannedRole::Kept) {
        let mut ancestor = row.parent;
        while ancestor != root && ancestors_of_kept_rows.insert(ancestor) {
            ancestor = parents_in_plan[&ancestor];
        }
    }
    for &row in &new_top_rows {
        if !ancestors_of_kept_rows.contains(&row) {
            host.arena_mut().note_new_layout_subtree_attached(row);
        }
    }
    // An anonymous box that took another over and whose children changed lays out again, as its list's box does.
    let mut changed_wrappers: HashSet<NodeSlotId> = HashSet::default();
    for row in &planned {
        let parent = map(row.parent);
        // NB: An anonymous box takes another over only under the same parent.
        let changed = match row.role {
            PlannedRole::Kept => row.old_parent != parent,
            PlannedRole::Wrapper => !taken_over.contains_key(&row.slot),
            PlannedRole::New => true,
        };
        if changed && taken.contains(&parent) {
            changed_wrappers.insert(parent);
        }
        // NB: An anonymous box a kept box moved out of lost a child.
        if row.role == PlannedRole::Kept && row.old_parent != parent && taken.contains(&row.old_parent) {
            changed_wrappers.insert(row.old_parent);
        }
    }
    for row in &dropped {
        if taken.contains(&row.parent) {
            changed_wrappers.insert(row.parent);
        }
    }
    for wrapper in changed_wrappers {
        host.arena_mut().note_child_list_restructured(wrapper);
    }
    invalidate_layout_for_list_change(host, state, root, change, &new_top_rows);
    if !new_top_rows.is_empty() {
        state.child_lists_that_gained_boxes.push((root, new_top_rows));
    }

    finish_planned_list(host, state, root, &plan);
    changed_at_or_before
}

/// Hands the rows of `plan` to the arena, where nothing binds them to their nodes yet, and answers how many there are.
fn adopt_planned_rows(host: &mut TreeBuilderHost<'_>, plan: &mut SubtreePlan) -> u64 {
    let mut descendant_count = 0;
    plan.for_each_plan(&mut |plan| descendant_count += plan.tree.row_count() as u64);
    let (filled, reused) = plan.take_rows();
    host.arena_mut()
        .give_back_free_slots(plan.tree.claims().take_free_slots());
    host.arena_mut().adopt_filled_chunks(filled, None);
    host.arena_mut().adopt_reused_rows(reused, None);
    plan.for_each_plan(&mut |plan| {
        for &(previous, next) in &plan.sibling_links_between_runs {
            host.arena_mut().link_planned_siblings(previous, next);
        }
    });
    descendant_count
}

/// Links the rows `planned` of the list of `root` as the plan links them, each as the row `map` answers for it: the
/// rows from `before_start`, the box before the first planned row that the list keeps as it is, or from the start of
/// the list, where the plan's first child goes first, to `end`, the box after which the list stays as it is, followed
/// by `after_end`, or to the end of the list, where the plan's last child goes last.
fn link_planned_list(
    arena: &mut LayoutNodeArena,
    root: LayoutNode,
    planned: &[PlannedRow],
    map: &dyn Fn(NodeSlotId) -> NodeSlotId,
    (before_start, planned_first_child): (NodeSlotId, NodeSlotId),
    (end, after_end, planned_last_child): (NodeSlotId, NodeSlotId, NodeSlotId),
) {
    let root_data = arena.data(root);
    let first_child = if before_start.is_invalid() {
        map(planned_first_child)
    } else {
        root_data.first_child.get()
    };
    let last_child = if end.is_invalid() {
        map(planned_last_child)
    } else {
        root_data.last_child.get()
    };
    if root_data.first_child.get() != first_child || root_data.last_child.get() != last_child {
        let mut shape = arena.write_shape(root);
        shape.set_first_child(first_child);
        shape.set_last_child(last_child);
    }
    for (index, row) in planned.iter().enumerate() {
        let slot = map(row.slot);
        let previous_sibling = if index == 0 && !before_start.is_invalid() {
            before_start
        } else {
            map(row.previous_sibling)
        };
        let next_sibling = if slot == end { after_end } else { map(row.next_sibling) };
        arena.link_planned_row(
            slot,
            PlannedLinks {
                parent: map(row.parent),
                previous_sibling,
                next_sibling,
                children: (row.role == PlannedRole::Wrapper).then(|| (map(row.first_child), map(row.last_child))),
            },
        );
        if slot != row.slot {
            const CHILDREN_ARE_INLINE: u32 = NodeFlag::ChildrenAreInline as u32;
            let flags = arena.data(slot).flags.get();
            let planned_flags = (flags & !CHILDREN_ARE_INLINE) | (row.flags & CHILDREN_ARE_INLINE);
            if flags != planned_flags {
                arena.write_shape(slot).set_flags(planned_flags);
            }
        }
    }
}

/// What the commit of a planned child list of `root` does once its rows are linked and labeled: the plan's events, the
/// quotes a new box moves, the fixup of the table the list is in, and what the walk reports.
fn finish_planned_list(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    root: LayoutNode,
    plan: &SubtreePlan,
) {
    run_post_commit_events(host, plan);
    // NB: A new box that moves the quote depth changes the quotes after its list, which only a build of the whole tree
    //     finds again. The walk listed the elements it planned a new pseudo-element box with quotes for.
    let mut moves_quote_depth = false;
    plan.for_each_plan(&mut |plan| {
        moves_quote_depth = moves_quote_depth
            || plan.tree.quote_generators.iter().any(|&generator| {
                crate::layout::quote_depth::pseudo_element_boxes_move_quote_depth(host.arena(), generator)
            });
    });
    if moves_quote_depth {
        state.asks_for_every_box_again = true;
    }
    let table_roots: Vec<LayoutNode> = (is_tabular_container(host, root)
        || display_for_table_fixup(host, root).is_table_column_group())
    .then(|| nearest_table_root(host, root))
    .flatten()
    .into_iter()
    .collect();
    super::planned_build::finish_table_fixup(host, plan, table_roots);
    state.reports.extend(plan.reports_in_walk_order());
    state.reached_unstyled_element |= plan.reached_unstyled_element;
}

/// Marks for layout what the change `change` of the list of `root` invalidates: the new boxes `new_top_rows` of a list
/// that only gained boxes, or the list's box.
fn invalidate_layout_for_list_change(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    root: LayoutNode,
    change: ListChange,
    new_top_rows: &[NodeSlotId],
) {
    // NB: The viewport's list holds the boxes of the top layer, whose changes no partial relayout confines.
    if change != ListChange::Unchanged && host.data(root).kind.get() == NodeKind::Viewport {
        state.layout_tree_update_escaped_rebuild_roots = true;
    }
    match change {
        ListChange::Unchanged => {}
        ListChange::InsertedOnly => state.rebuilt_subtree_roots.extend(new_top_rows.iter().copied()),
        // NB: Boxes that left the list leave the boxes that stay as they were, so the list's box lays out again with them.
        ListChange::RemovedOnly => host.arena_mut().set_needs_layout_update(root, true),
        ListChange::Restructured => {
            host.arena_mut().note_child_list_restructured(root);
            state.rebuilt_subtree_roots.push(root);
        }
    }
}

/// The fragments of a box that leaves its list, for a new box of the same node to take over: the node, the box, its
/// committed fragment link, and the box that holds the lines it was on.
type ReplacedFragments = (
    StyleNodeID,
    NodeSlotId,
    Option<crate::layout::fragment_tree::FragmentLink>,
    Option<NodeSlotId>,
);

/// Takes the boxes `dropped`, which their list no longer has, out of the tree, and frees them with their subtrees: what
/// they painted is damaged, and what a new box of the same node takes over of their fragments is answered. The rows of
/// the list around them are left to the caller to link again.
fn drop_list_rows(host: &mut TreeBuilderHost<'_>, dropped: &[OldRow]) -> Vec<ReplacedFragments> {
    let mut replaced_fragments = Vec::new();
    if !dropped.is_empty() {
        let arena = host.arena_mut();
        if arena.paintable_row_count() > 0 {
            arena.push_enclosing_paint_order_damage(dropped[0].slot);
        }
        for row in dropped {
            let data = arena.data(row.slot);
            if !node_facts::kind_is_box(data.kind.get()) {
                continue;
            }
            let Some(node) = arena.dom_node_style_node(row.slot) else {
                continue;
            };
            if arena.bound_row(node) != row.slot {
                continue;
            }
            let containing_block = arena.containing_block_by_walking_ancestors(row.slot);
            let has_lines = !containing_block.is_invalid() && {
                let paintable_rows = arena.paintable_rows();
                paintable_rows.paintable_row_is_populated(containing_block)
                    && crate::painting::node_painting::has_lines(&paintable_rows, containing_block)
            };
            let link = arena.take_committed_fragment_link(row.slot);
            if link.is_some() || has_lines {
                replaced_fragments.push((node, row.slot, link, has_lines.then_some(containing_block)));
            }
        }
        for row in dropped {
            let mut shape = arena.write_shape(row.slot);
            shape.set_parent(NodeSlotId::INVALID);
            shape.set_previous_sibling(NodeSlotId::INVALID);
            shape.set_next_sibling(NodeSlotId::INVALID);
            if row.is_wrapper {
                shape.set_first_child(NodeSlotId::INVALID);
                shape.set_last_child(NodeSlotId::INVALID);
            }
        }
        for row in dropped {
            let host_calls = host.host_calls();
            crate::layout::layout_node_arena::prepare_subtree_for_detach(host_calls, host.arena_mut(), row.slot);
            host.free_subtree(row.slot);
        }
    }
    replaced_fragments
}

/// Labels the new rows of the list `root` planned again in pre-order, from `first` up to `last`, a box the list kept, or
/// to the end of the list: each run of new rows between two rows that keep their labels, the boxes the list kept with
/// their subtrees and the anonymous boxes that took over others, takes labels between theirs. Where a run does not fit,
/// the subtree of `root`, or of an ancestor, is labeled again.
fn label_planned_list(
    arena: &mut LayoutNodeArena,
    root: LayoutNode,
    first: NodeSlotId,
    last: NodeSlotId,
    taken: &HashSet<NodeSlotId>,
) {
    let end = arena.pre_order_label_of_subtree_successor(root);
    let mut lower = arena.node_pre_order_label(root);
    let mut run = Vec::new();
    let mut fits = true;
    let mut cursor = first;
    while !cursor.is_invalid() && fits {
        let node = cursor;
        // NB: A new row is unlabeled as the arena adopts it, and every other row below the root has a label.
        let is_new = arena.node_pre_order_label(node) == 0;
        let is_kept = !is_new && !taken.contains(&node);
        let keeps_label = !is_new;
        if keeps_label {
            let label = arena.node_pre_order_label(node);
            fits = arena.label_rows_between(&run, lower, label, true);
            run.clear();
            lower = label;
            if node == last {
                break;
            }
        } else {
            run.push(node);
        }
        // NB: The rows below a kept box keep their labels with it.
        let descends = !is_kept;
        if is_kept {
            lower = arena.last_pre_order_label_of_subtree(node);
        }
        let first_child = arena.data(node).first_child.get();
        cursor = if descends && !first_child.is_invalid() {
            first_child
        } else {
            let mut current = node;
            loop {
                let next = arena.data(current).next_sibling.get();
                if !next.is_invalid() {
                    break next;
                }
                current = arena.data(current).parent.get();
                if current == root {
                    break NodeSlotId::INVALID;
                }
            }
        };
    }
    if fits {
        fits = arena.label_rows_between(&run, lower, end, false);
    }
    if !fits {
        arena.spread_pre_order_labels_below(root);
    }
}

/// Splits the first letter of `owner`, a box planned fresh or one whose `::first-letter` a child list planned again may
/// have moved, out of its text again: the split there was goes, its text renders whole again, and the letter is looked
/// for anew. A letter of generated content, which comes before everything a list holds, stays where it is.
pub(super) fn split_first_letter_again(host: &mut TreeBuilderHost<'_>, owner: StyleNodeID) {
    let owner_row = host.arena().bound_row(owner);
    if owner_row.is_invalid() || !node_facts::kind_is_block_container(host.data(owner_row).kind.get()) {
        return;
    }
    let first_letter = host.arena().bound_pseudo_element_row(owner, GENERATED_FOR_FIRST_LETTER);
    if !first_letter.is_invalid() {
        let remainder = host.next_sibling(first_letter);
        let remainder_kind = (!remainder.is_invalid()).then(|| host.data(remainder).kind.get());
        let letter = host.first_child(first_letter);
        let whole = if remainder_kind == Some(NodeKind::TextNode) && host.arena().text_has_source_range(remainder) {
            let Some(text) = host.arena().node_style_node(remainder) else {
                return;
            };
            let whole = host.create_text_box(text);
            host.arena_mut()
                .enroll_built_text_rows_for_content_sync(std::iter::once(whole));
            whole
        } else if remainder_kind == Some(NodeKind::GeneratedTextNode)
            && !letter.is_invalid()
            && host.data(letter).kind.get() == NodeKind::GeneratedTextNode
        {
            // NB: A letter split out of generated text goes back into one generated text with the text after it.
            let mut source = host
                .arena()
                .published_text_source(letter, false)
                .data
                .to_utf16()
                .into_owned();
            source.extend_from_slice(&host.arena().published_text_source(remainder, false).data.to_utf16());
            host.stamp_generated_text_box(&source)
        } else {
            return;
        };
        let parent = host.parent(first_letter);
        host.attach_child(parent, host.created(whole), remainder);
        host.arena_mut().detach_child(parent, remainder);
        host.free_subtree(remainder);
        host.arena_mut()
            .clear_pseudo_element_box(owner, GENERATED_FOR_FIRST_LETTER);
        host.arena_mut().detach_child(parent, first_letter);
        host.free_subtree(first_letter);
        host.arena_mut().note_child_list_restructured(parent);
        host.arena_mut().set_needs_layout_update(parent, true);
    }
    if let FirstLetterSearch::Found(target) = find_first_letter_in_block(host, owner_row) {
        create_first_letter_boxes(host, owner, target);
        // NB: The box that holds the first letter now lays out again, as it would after any other change of its
        //     list.
        let first_letter = host.arena().bound_pseudo_element_row(owner, GENERATED_FOR_FIRST_LETTER);
        if !first_letter.is_invalid() {
            let parent = host.parent(first_letter);
            host.arena_mut().note_child_list_restructured(parent);
            host.arena_mut().set_needs_layout_update(parent, true);
        }
    }
}
