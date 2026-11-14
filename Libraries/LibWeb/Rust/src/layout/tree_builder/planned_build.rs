/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The boxes below a box whose whole subtree is new, built from a plan: the planner decides every box of the subtree,
//! the plan is committed to the arena, and one walk over what the planner recorded then does what needs the boxes in
//! the tree, in tree order. The first build of a document, or one that builds every box
//! again, builds the whole tree this way, and a child list planned again commits its new boxes the same way.

use super::*;
use crate::layout::tree_plan::content_items::{
    ContentCounters, DocumentStart, PendingContent, RemovedItem, fill_pending_content,
};
use crate::layout::tree_plan::planner::{PlanContext, PlanEvent, Planner, SubtreePlan};
use crate::layout::tree_plan::{OneByOneRowSource, RowCommittedOneByOne, box_fact};

/// Builds every box below `viewport`, the viewport just made for `document`, from a plan.
pub(super) fn build_document_descendants(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    viewport: LayoutNode,
    document: StyleNodeID,
) {
    let (layout_node, identity) = (viewport, document);
    let free_slots = host.arena_mut().take_free_slots();
    let arena = host.arena();
    let mut plan = arena.with_style_store(|engine| {
        let mut plan = Planner::plan_descendants(
            engine,
            arena,
            layout_node,
            identity,
            true,
            PlanContext {
                has_svg_root: false,
                enclosing_ancestor_rows: &[],
                free_slots,
            },
        );
        plan.fill_generated_content(engine, arena, &mut DocumentStart);
        plan
    });
    let mut descendant_count = 0;
    plan.for_each_plan(&mut |plan| descendant_count += plan.tree.row_count() as u64);
    // NB: The rows take their pre-order labels as the arena adopts them, where they fit below the root and the build
    //     put them in the order of their positions.
    let labels = (!plan.pre_order_positions_out_of_order())
        .then(|| {
            host.arena()
                .planned_subtree_pre_order_labels(layout_node, descendant_count, plan.pre_order_extent())
        })
        .flatten()
        .map(
            |(lower, step)| crate::layout::layout_node_arena::PlannedPreOrderLabels {
                lower,
                step,
                starts: plan.pre_order_starts(),
            },
        );
    let (filled, reused) = plan.take_rows();
    host.arena_mut()
        .give_back_free_slots(plan.tree.claims().take_free_slots());
    host.arena_mut().adopt_filled_chunks(filled, labels.as_ref());
    host.arena_mut().adopt_reused_rows(reused, labels.as_ref());
    plan.for_each_plan(&mut |plan| {
        for &(previous, next) in &plan.sibling_links_between_runs {
            host.arena_mut().link_planned_siblings(previous, next);
        }
    });
    let root_row = plan.tree.node(layout_node).row();
    let (first_child, last_child) = (root_row.first_child.get(), root_row.last_child.get());
    if !first_child.is_invalid() {
        host.arena_mut()
            .link_planned_children(layout_node, first_child, last_child);
    }
    commit_plan(
        host,
        state,
        layout_node,
        &plan,
        descendant_count,
        labels.is_some(),
        None,
    );
    run_post_commit_events(host, &plan);
    finish_table_fixup(host, &plan, Vec::new());
    state.reports.extend(plan.reports_in_walk_order());
    state.reached_unstyled_element |= plan.reached_unstyled_element;
}

/// Does what is left of table fixup once every row of `plan` is committed: frees the rows of the boxes the walk removed
/// from their lists, fixes up the child lists the walk left to the commit (see [`PlanTree::table_fixup_lists`]), whose
/// boxes are complete once the commit made the generated content of pseudo-elements, and makes the missing cells of
/// the tables the walk made wrappers for, those the lists hold, and `table_roots`.
///
/// [`PlanTree::table_fixup_lists`]: crate::layout::tree_plan::PlanTree::table_fixup_lists
pub(super) fn finish_table_fixup(host: &mut TreeBuilderHost<'_>, plan: &SubtreePlan, mut table_roots: Vec<LayoutNode>) {
    let mut table_fixup_lists = Vec::new();
    plan.for_each_plan(&mut |plan| {
        for &row in &plan.tree.dropped_rows {
            host.free_subtree(row);
        }
        table_fixup_lists.extend_from_slice(&plan.tree.table_fixup_lists);
        table_roots.extend_from_slice(&plan.tree.table_roots);
    });
    if !table_fixup_lists.is_empty() || !table_roots.is_empty() {
        fixup_tables_in_child_lists(host, &table_fixup_lists, table_roots);
    }
}

/// What committing the plan of a child list planned again (see `list_unit.rs`) is told of the list: the anonymous boxes
/// of the plan that boxes the list had took over, by the box that took each over, the boxes the list had that hold new
/// boxes now, the list's own box among them, and the boxes the list kept.
pub(super) struct UnitCommit<'a> {
    pub(super) taken_over: &'a HashMap<NodeSlotId, NodeSlotId>,
    pub(super) old_parents: &'a HashSet<NodeSlotId>,
    pub(super) kept: &'a dyn Fn(NodeSlotId) -> bool,
}

/// Gives the rows of the planned boxes below `root`, which the plan made and linked, what the builder's own steps give
/// a row it makes and the walk did not: its binding to its node, what its style tells the rest of the document, and
/// what being attached to its parent derives. For a child list planned again, `unit` tells which rows are not new; its
/// labels and paint order damage are left to the caller.
// NB: Kept out of line so that profiles tell its time apart.
#[inline(never)]
pub(super) fn commit_plan(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    root: LayoutNode,
    plan: &SubtreePlan,
    descendant_count: u64,
    labels_are_spread: bool,
    unit: Option<&UnitCommit<'_>>,
) {
    // NB: A stale subtree root clears every row its subtree is bound to, so it goes before the rows are bound: the
    //     walk built the boxes below a `display: contents` element after it cleared the element's subtree.
    plan.for_each_stale_subtree(&mut |stale_root, scope| clear_stale_subtree(host, stale_root, scope));

    // NB: A child's ancestor facts read the layout algorithm of its parent as it is attached.
    plan.for_each_plan(&mut |plan| {
        let tree = &plan.tree;
        for (&node, &kind) in &tree.math_layout_kinds {
            host.arena_mut().note_planned_math_layout_kind(node, kind);
        }
    });

    // The nodes of the rows are bound and their records pinned a list at a time. Then what the rows owe in images goes
    // to the arena, and the rows that need more are committed one by one, both in pre-order: each row after its
    // parent.
    let mut has_text_rows = false;
    plan.for_each_plan(&mut |plan| {
        let lists = plan.commit_node_lists();
        let arena = host.arena_mut();
        arena.bind_planned_rows(lists.bindings);
        if !lists.pins.is_empty() {
            arena.with_style_engine(|engine| {
                for &(record, times) in lists.pins {
                    engine.pin_layout_style_record_times(record, times);
                }
            });
        }
        if !lists.text_rows.is_empty() {
            has_text_rows = true;
            // A text row renders what the mirror publishes for its node, synced before layout.
            arena.enroll_built_text_rows_for_content_sync(lists.text_rows.iter().copied());
        }
    });
    let mut one_by_one = RowsCommittedOneByOne::default();
    plan.for_each_commit_slice_in_walk_order(&mut |lists| {
        host.arena_mut()
            .owe_planned_image_resources(lists.owed_images.0, lists.owed_images.1);
        for row in lists.rows_committed_one_by_one {
            if let Some(unit) = unit
                && let Some(&old) = unit.taken_over.get(&row.slot)
            {
                // NB: The box the list had keeps its style, and the rows below it are derived as the planned one's.
                if row.facts & box_fact::SUBTREE_DERIVED_ON_COMMIT != 0 {
                    one_by_one.rows_derived_on_commit.push(old);
                }
                continue;
            }
            commit_row_one_by_one(host, state, root, row, unit, &mut one_by_one);
        }
    });
    host.arena_mut()
        .note_planned_style_may_have_facts(plan.style_may_have_facts());
    // NB: The texts of the generated content go to their rows once the boxes they are in are stamped.
    plan.for_each_plan(&mut |plan| {
        let content = &plan.content;
        if content.text_rows.is_empty() && content.records.is_empty() {
            return;
        }
        let arena = host.arena_mut();
        arena.commit_planned_generated_text_rows(&content.text_rows, &content.texts);
        for (owner, record) in &content.records {
            arena.note_generated_content(*owner, record.clone());
        }
    });
    plan.for_each_plan(&mut |plan| remove_generated_items(host, &plan.content.removed_items));
    let RowsCommittedOneByOne {
        elements_cleared_by_their_backdrop: _,
        rows_derived_on_commit,
        absolutely_positioned_rows,
    } = one_by_one;

    let children_are_inline = plan.tree.node(root).flags() & NodeFlag::ChildrenAreInline as u32 != 0;
    if children_are_inline != node_facts::has_flag(host.data(root), NodeFlag::ChildrenAreInline) {
        host.set_children_are_inline(root, children_are_inline);
    }
    plan.for_each_plan(&mut |plan| {
        let tree = &plan.tree;
        for &(node, inline_box) in &tree.lifted_out_of_inline_boxes {
            host.arena_mut().note_inline_box_lifted_out_of(node, Some(inline_box));
        }
    });

    // The rows the walk left to the commit to derive, which it attaches in pre-order once their parents have their
    // styles, and the boxes absolutely positioned rows escape, once the boxes above them know whether they contain
    // them.
    let parent_is_planned = |parent: NodeSlotId| match unit {
        Some(unit) => !unit.old_parents.contains(&parent),
        None => parent != root,
    };
    for subtree_root in rows_derived_on_commit {
        let mut slot = subtree_root;
        loop {
            let parent_slot = host.parent(slot);
            // NB: A box a child list planned again kept is attached as it was, or is noted as moved, by the caller.
            let is_kept = unit.is_some_and(|unit| (unit.kept)(slot));
            // NB: Without the facts of the row's style, the arena reads what it needs of the style itself.
            if !is_kept {
                host.arena_mut()
                    .note_planned_child_attached(parent_slot, slot, None, parent_is_planned(parent_slot));
            }
            let first_child = host.first_child(slot);
            if !first_child.is_invalid() && !is_kept {
                slot = first_child;
                continue;
            }
            while slot != subtree_root && host.next_sibling(slot).is_invalid() {
                slot = host.parent(slot);
            }
            if slot == subtree_root {
                break;
            }
            slot = host.next_sibling(slot);
        }
    }
    for slot in absolutely_positioned_rows {
        host.arena_mut().mark_nodes_escaped_by_planned_out_of_flow_box(slot);
    }

    if unit.is_none() {
        host.arena_mut()
            .finish_planned_subtree(root, descendant_count, labels_are_spread);
    }
    if has_text_rows {
        host.arena_mut()
            .paint_state_mut()
            .bump_custom_highlight_row_generation();
    }

    // The walk answers the layout tree update marks of every node it reached.
    plan.for_each_plan(&mut |plan| {
        for &identity in &plan.answered_marks {
            host.arena_mut().clear_layout_tree_update_marks(Some(identity));
        }
    });
    for &identity in &plan.answered_own_marks {
        host.arena_mut()
            .layout_tree_update_marks_mut()
            .merge(identity, false, 0);
    }
}

/// What committing rows one by one collects for after the rows are committed.
#[derive(Default)]
struct RowsCommittedOneByOne {
    /// The elements a top layer member's backdrop, which goes before it, cleared the pseudo-element boxes of.
    elements_cleared_by_their_backdrop: Vec<StyleNodeID>,
    rows_derived_on_commit: Vec<NodeSlotId>,
    absolutely_positioned_rows: Vec<NodeSlotId>,
}

/// Commits `row`, a row of a plan that needs more than the lists of its plan give it (see
/// [`SubtreePlan::for_each_commit_slice_in_walk_order`]): rows the builder's own steps stamp, rows whose style tells the
/// rest of the document something, and rows that derive what attaching them derives.
fn commit_row_one_by_one(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    root: LayoutNode,
    row: &RowCommittedOneByOne,
    unit: Option<&UnitCommit<'_>>,
    one_by_one: &mut RowsCommittedOneByOne,
) {
    let slot = row.slot;
    let parent_slot = host.parent(slot);
    let derived_on_commit = row.facts & box_fact::DERIVED_ON_COMMIT != 0;
    let planned_flags = host.data(slot).flags.get();
    let mut stamped_by_builder = true;
    let mut style_facts = None;
    match row.source {
        OneByOneRowSource::Element {
            element,
            record,
            style_facts: facts,
        } => {
            // NB: A top layer member's backdrop goes before it, and cleared what the element held already.
            if !one_by_one.elements_cleared_by_their_backdrop.contains(&element) {
                clear_pseudo_element_boxes_of_rebuilt_element(host, state, element);
            }
            let kind = host.data(slot).kind.get();
            if row.facts & box_fact::STAMPED_BY_BUILDER != 0 {
                let taken = host.arena_mut().allocate_preallocated_row(slot);
                let stamped = host.create_element_box(element, kind, record);
                assert_eq!(stamped, slot, "the builder stamps the row the plan made");
                host.arena_mut().give_back_planned_row(slot, taken, !derived_on_commit);
                if row.facts & box_fact::OWNS_CONTENT_REPLACEMENT_IMAGE != 0 {
                    host.arena_mut().await_owned_image_provider(slot);
                }
                debug_assert_eq!(
                    node_is_inline_outside(host, slot),
                    row.facts & box_fact::INLINE_OUTSIDE != 0
                );
            } else {
                stamped_by_builder = false;
                style_facts = Some(facts);
                // NB: The walk stamped the row. Its flex and grid item flags are derived as it is attached where the
                //     walk left that to the commit.
                if derived_on_commit {
                    let flags = planned_flags & !(NodeFlag::IsFlexItem as u32 | NodeFlag::IsGridItem as u32);
                    if flags != planned_flags {
                        host.arena_mut().write_shape(slot).set_flags(flags);
                    }
                }
                host.arena_mut().note_planned_element_row_style(slot, facts);
                if row.facts & box_fact::HOLDS_HIGHLIGHT_STYLE != 0 {
                    crate::painting::selection::note_built_row_highlight_pseudo_styles(host.arena_mut(), slot, element);
                }
            }
        }
        OneByOneRowSource::Text(text) => {
            stamped_by_builder = false;
            if row.facts & box_fact::PARENT_HOLDS_HIGHLIGHT_STYLE != 0
                && let Some(parent) = host.arena().with_style_store(|engine| engine.text_parent(text))
                && host.arena().bound_row(parent).is_invalid()
            {
                crate::painting::selection::note_built_row_highlight_pseudo_styles(host.arena_mut(), slot, parent);
            }
        }
        OneByOneRowSource::PseudoElement {
            element,
            generated_for,
            decision,
        } => {
            let kind = host.data(slot).kind.get();
            if generated_for == GENERATED_FOR_BACKDROP {
                clear_pseudo_element_boxes_of_rebuilt_element(host, state, element);
                one_by_one.elements_cleared_by_their_backdrop.push(element);
            }
            host.arena_mut().clear_pseudo_element_box(element, generated_for);
            let taken = host.arena_mut().allocate_preallocated_row(slot);
            let stamped = stamp_pseudo_element_box_row(host, kind, element, generated_for, decision);
            assert_eq!(stamped, slot, "the builder stamps the row the plan made");
            host.arena_mut().give_back_planned_row(slot, taken, !derived_on_commit);
        }
        OneByOneRowSource::DisplayContentsTextWrapper { text, record } => {
            let kind = host.data(slot).kind.get();
            let taken = host.arena_mut().allocate_preallocated_row(slot);
            let wrapper = host
                .create_anonymous_box_from_style_record(
                    record,
                    AnonymousStyleKind::InlineStyleWrapper,
                    AnonymousStyleOverrides::default(),
                    kind,
                )
                .into_slot();
            assert_eq!(wrapper, slot, "the builder stamps the row the plan made");
            host.arena_mut().note_display_contents_text_wrapper(wrapper, text);
            host.arena_mut().give_back_planned_row(slot, taken, !derived_on_commit);
        }
        OneByOneRowSource::ContentsWrapper {
            style_parent,
            style_kind,
        } => {
            let kind = host.data(slot).kind.get();
            let overrides = match style_kind {
                AnonymousStyleKind::ButtonContentBox => AnonymousStyleOverrides {
                    columns_from_parent: !node_facts::has_flag(host.data(style_parent), NodeFlag::IsHtmlInputElement),
                    ..AnonymousStyleOverrides::default()
                },
                AnonymousStyleKind::FieldsetContentWrapper => {
                    let style = node_facts::node_style_view(host.data(style_parent)).expect("fieldset style");
                    AnonymousStyleOverrides {
                        overflow_x: style.overflow_x(),
                        overflow_y: style.overflow_y(),
                        columns_from_parent: true,
                    }
                }
                _ => AnonymousStyleOverrides::default(),
            };
            let taken = host.arena_mut().allocate_preallocated_row(slot);
            let wrapper = host
                .create_anonymous_box(style_parent, style_kind, overrides, kind)
                .into_slot();
            assert_eq!(wrapper, slot, "the builder stamps the row the plan made");
            host.arena_mut().give_back_planned_row(slot, taken, !derived_on_commit);
            // NB: The fieldset's overflow applies to its anonymous content box instead.
            if style_kind == AnonymousStyleKind::FieldsetContentWrapper {
                let host_calls = host.host_calls();
                host.arena_mut()
                    .update_layout_style(host_calls, style_parent, |style, _| {
                        style.set_overflow(
                            crate::css::css_enums::overflow::VISIBLE,
                            crate::css::css_enums::overflow::VISIBLE,
                        );
                    });
            }
        }
        OneByOneRowSource::AnonymousTableBox { style_parent, kind } => {
            let (style_kind, node_kind) = kind.style_and_node_kind();
            let taken = host.arena_mut().allocate_preallocated_row(slot);
            let wrapper = host
                .create_anonymous_box(style_parent, style_kind, AnonymousStyleOverrides::default(), node_kind)
                .into_slot();
            assert_eq!(wrapper, slot, "the builder stamps the row the plan made");
            host.arena_mut().give_back_planned_row(slot, taken, !derived_on_commit);
        }
        OneByOneRowSource::TableWrapper { table } => {
            let taken = host.arena_mut().allocate_preallocated_row(slot);
            let wrapper = host
                .create_anonymous_box(
                    table,
                    AnonymousStyleKind::TableWrapper,
                    AnonymousStyleOverrides::default(),
                    NodeKind::TableWrapper,
                )
                .into_slot();
            assert_eq!(wrapper, slot, "the builder stamps the row the plan made");
            host.arena_mut().give_back_planned_row(slot, taken, !derived_on_commit);
            // NB: The wrapper took over the table's outer properties, which the table's box no longer has.
            let host_calls = host.host_calls();
            host.arena_mut()
                .reset_table_box_style_used_by_wrapper(host_calls, table);
        }
        OneByOneRowSource::GeneratedImage {
            generator,
            generated_for,
            style_box,
            image,
        } => {
            let taken = host.arena_mut().allocate_preallocated_row(slot);
            let item = create_generated_image(host, generator, pseudo_element_of(generated_for), image, style_box);
            assert_eq!(item, slot, "the builder stamps the row the plan made");
            host.arena_mut().give_back_planned_row(slot, taken, !derived_on_commit);
        }
        OneByOneRowSource::NestedListMarker {
            generator,
            generated_for,
        } => {
            let taken = host.arena_mut().allocate_preallocated_row(slot);
            let marker = stamp_nested_list_marker_row(host, generator, pseudo_element_of(generated_for));
            assert_eq!(marker, slot, "the builder stamps the row the plan made");
            host.arena_mut().give_back_planned_row(slot, taken, !derived_on_commit);
        }
        OneByOneRowSource::AnonymousWrapper { style_parent } => {
            let kind = host.data(slot).kind.get();
            let taken = host.arena_mut().allocate_preallocated_row(slot);
            let wrapper = host
                .create_anonymous_box(
                    style_parent,
                    AnonymousStyleKind::Wrapper,
                    AnonymousStyleOverrides::default(),
                    kind,
                )
                .into_slot();
            assert_eq!(wrapper, slot, "the builder stamps the row the plan made");
            host.arena_mut().give_back_planned_row(slot, taken, !derived_on_commit);
        }
    }
    // NB: The builder's steps stamp a row without the flags the walk decided.
    if stamped_by_builder {
        const DECIDED_BY_THE_WALK: u32 = NodeFlag::ChildrenAreInline as u32
            | NodeFlag::ListMarkerIsInside as u32
            | NodeFlag::ReplacedBoxCanHaveChildren as u32;
        let stamped_flags = host.data(slot).flags.get();
        let flags = (stamped_flags & !DECIDED_BY_THE_WALK) | (planned_flags & DECIDED_BY_THE_WALK);
        if flags != stamped_flags {
            host.arena_mut().write_shape(slot).set_flags(flags);
        }
    }
    // NB: A row table fixup removed from its list is in none, and is freed once every row is committed.
    if parent_slot.is_invalid() {
        return;
    }
    if derived_on_commit {
        if row.facts & box_fact::SUBTREE_DERIVED_ON_COMMIT != 0 {
            one_by_one.rows_derived_on_commit.push(slot);
        }
        return;
    }
    let parent_is_planned = match unit {
        Some(unit) => !unit.old_parents.contains(&parent_slot),
        None => parent_slot != root,
    };
    if host.arena_mut().note_planned_child_attached_with_derived_facts(
        parent_slot,
        slot,
        style_facts,
        parent_is_planned,
    ) {
        one_by_one.absolutely_positioned_rows.push(slot);
    }
}

/// An element whose box is built again from scratch loses every pseudo-element box it held. Its ::backdrop is a
/// sibling of the element, a viewport child, so removing it restructures the tree outside every rebuild root.
fn clear_pseudo_element_boxes_of_rebuilt_element(
    host: &mut TreeBuilderHost<'_>,
    state: &mut TreeBuilderState,
    element: StyleNodeID,
) {
    let old_backdrop = host.arena().bound_pseudo_element_row(element, GENERATED_FOR_BACKDROP);
    if !old_backdrop.is_invalid() {
        state.layout_tree_update_escaped_rebuild_roots = true;
        let backdrop_parent = host.parent(old_backdrop);
        assert!(!backdrop_parent.is_invalid());
        host.arena_mut().detach_child(backdrop_parent, old_backdrop);
        host.free_subtree(old_backdrop);
    }
    clear_synthetic_pseudo_element_boxes(host.host_calls(), host.arena, element);
}

fn pseudo_element_of(generated_for: u8) -> FfiPseudoElement {
    match generated_for {
        GENERATED_FOR_BEFORE => FfiPseudoElement::Before,
        GENERATED_FOR_AFTER => FfiPseudoElement::After,
        GENERATED_FOR_MARKER => FfiPseudoElement::Marker,
        GENERATED_FOR_BACKDROP => FfiPseudoElement::Backdrop,
        _ => unreachable!("only a ::before, ::after, ::marker or ::backdrop box is planned"),
    }
}

/// Does what needs the boxes in the tree, in tree order.
// NB: Kept out of line so that profiles tell its time apart.
#[inline(never)]
pub(super) fn run_post_commit_events(host: &mut TreeBuilderHost<'_>, plan: &SubtreePlan) {
    // NB: The walk resolved the counters of every owner outside the top layer and SVG resource content, which the
    //     events below read.
    if let Some(resolved) = &plan.resolved_counters {
        host.arena_mut().store_resolved_counters(resolved);
    }
    host.arena_mut()
        .with_counter_tables_lent(|_, tables| tables.counters_sets.begin_tree_order_walk());
    plan.for_each_event_in_walk_order(&mut |plan, event| run_event(host, plan, event));
    host.arena_mut()
        .with_counter_tables_lent(|_, tables| tables.counters_sets.end_tree_order_walk());
}

/// Does what `event` of `plan` asks for.
fn run_event(host: &mut TreeBuilderHost<'_>, plan: &SubtreePlan, event: &PlanEvent) {
    match *event {
        PlanEvent::ResolveCounters {
            element,
            may_change_counters,
            in_tree_order,
        } => {
            if !in_tree_order {
                host.arena_mut()
                    .with_counter_tables_lent(|_, tables| tables.counters_sets.restart_tree_order_walk());
            }
            // OPTIMIZATION: Resolving the counters of an element that changes none, while no element has any,
            //               leaves it with none.
            if may_change_counters || !host.arena().has_no_counters() {
                resolve_counters(host, element, FfiPseudoElement::None);
            }
        }
        PlanEvent::FillGeneratedContent {
            element,
            generated_for,
            first_pending,
            end_pending,
        } => {
            let pseudo_element = pseudo_element_of(generated_for);
            // NB: The counters of a ::marker resolved after the commit are resolved before its element's children.
            let owner = if generated_for == GENERATED_FOR_MARKER {
                counter_owner_of_pseudo_element(element, pseudo_element)
            } else {
                resolve_counters(host, element, pseudo_element)
            };
            for pending in &plan.content.pending[first_pending as usize..end_pending as usize] {
                debug_assert!(matches!(pending.counters, ContentCounters::AfterCommit));
                debug_assert_eq!(pending.owner, owner);
                fill_content_after_commit(host, pending);
            }
        }
        PlanEvent::ResolveMarkerCounters(element) => {
            resolve_counters(host, element, FfiPseudoElement::Marker);
        }
        PlanEvent::SplitFirstLetter(element) => super::list_unit::split_first_letter_again(host, element),
        PlanEvent::CreateWritingSuggestionBoxes { element, node } => {
            create_writing_suggestion_boxes(host, element, node);
        }
        PlanEvent::Nothing => {}
    }
}

/// Takes the rows of the items of generated content that turned out to make nothing out of the tree, each with the
/// anonymous box that held it where that holds nothing else now, and gives the boxes they leave empty the flags they
/// had before their items went in.
fn remove_generated_items(host: &mut TreeBuilderHost<'_>, removed: &[RemovedItem]) {
    for item in removed {
        let parent = host.parent(item.row);
        host.arena_mut().detach_child(parent, item.row);
        host.free_subtree(item.row);
        if parent != item.box_row && host.first_child(parent).is_invalid() {
            let grandparent = host.parent(parent);
            host.arena_mut().detach_child(grandparent, parent);
            host.free_subtree(parent);
        }
        if host.first_child(item.box_row).is_invalid() {
            let children_are_inline = item.flags_before_items & NodeFlag::ChildrenAreInline as u32 != 0;
            if children_are_inline != node_facts::has_flag(host.data(item.box_row), NodeFlag::ChildrenAreInline) {
                host.set_children_are_inline(item.box_row, children_are_inline);
            }
        }
    }
}

/// Spells the texts of `pending`, the content of a pseudo-element of the top layer or of SVG resource content, from the
/// counters sets the build stored, now that it resolved the pseudo-element's counters.
fn fill_content_after_commit(host: &mut TreeBuilderHost<'_>, pending: &PendingContent) {
    let (record, texts, removed) = host.arena_mut().with_counter_tables_lent(|arena, tables| {
        let mut values = crate::layout::generated_content::StoredCounterValues {
            arena,
            sets: &mut tables.counters_sets,
            owner: pending.owner,
        };
        let mut texts = Vec::new();
        let mut removed = Vec::new();
        let record = arena.with_style_store(|engine| {
            fill_pending_content(engine, arena, pending, &mut values, &mut texts, &mut removed)
        });
        (record, texts, removed)
    });
    for (row, text) in texts {
        host.arena_mut()
            .set_generated_text(row, ak::Utf16String::from_utf16(&text));
    }
    remove_generated_items(host, &removed);
    if let Some(record) = record {
        host.arena_mut().note_generated_content(pending.owner, record);
    }
}
