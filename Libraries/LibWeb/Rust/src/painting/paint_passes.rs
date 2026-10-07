/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The steps of paint preparation, as passes the host sends its document's render state and waits for. A pass reads
//! and writes the arena alone: what it owes the host, such as the scroll offsets it clamped, comes back in its answer.

use crate::css::css_pixels::CssPixelPoint;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::host::{FfiVisualContextTreeInputs, FfiVisualContextUpdateOutcome, RootBackgroundSource};
use crate::painting::paint_read::PaintRead;
use crate::painting::svg_paint_resources::{PublishedSvgFilter, PublishedSvgPaintServer, SvgPaintResourceKind};
use crate::painting::visual_context::dirty::VisualContextUpdateScope;
use crate::painting::visual_context::incremental::{
    IncrementalUpdateResult, debug_assert_every_live_node_is_owned, update_visual_context_tree,
};
use crate::painting::visual_context::{VisualContextState, VisualContextTree};
use crate::render_state::{BegunRead, DocumentHost};
use libgfx_rust::FloatPoint;
use std::sync::Arc;

/// What a paint pass writes, which decides what the host lets go of before it sends the pass, and whether the paint and
/// hit testing properties the host prepared stay current.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum PassEffect {
    /// Writes the rows throughout: measures every unmeasured overflow, or brings the visual context of every dirty box
    /// up to date. Such a pass is one of those that prepare the paint and hit testing properties.
    RewritesRows,
    /// Changes nothing the paint and hit testing properties are prepared from.
    KeepsPaintPreparation,
    /// Changes what the paint and hit testing properties are prepared from.
    StalesPaintPreparation,
}

impl PassEffect {
    pub(crate) fn leaves_paint_preparation_current(self) -> bool {
        self != Self::StalesPaintPreparation
    }
}

#[derive(Default)]
#[repr(C)]
pub struct FfiRenderingPreparationOutcome {
    pub requires_display_list_recording: bool,
    pub requires_visual_context_update: bool,
    pub visual_context_values_changed: bool,
}

/// What preparing for rendering answers: its outcome, and the scroll offsets the new overflow moved out of range,
/// each clamped into it, for the host to store.
pub(crate) struct RenderingPreparation {
    pub(crate) outcome: FfiRenderingPreparationOutcome,
    pub(crate) clamped_scroll_offsets: Vec<(NodeSlotId, CssPixelPoint)>,
}

/// What a clock tick's round changed that only the host settles: a root background, scrollability or SVG paint
/// resource, a visual context tree built anew, or a compositor animation whose node went away.
pub(crate) struct VisualContextsNeedHost(pub(crate) &'static str);

/// The visual context tree a clock tick's frame takes to the compositor, with the scroll offsets of its nodes where its
/// structure is not the one the compositor has.
pub(crate) struct ClockTickVisualContexts {
    pub(crate) tree: Arc<VisualContextTree>,
    pub(crate) restructured_scroll_offsets: Option<Vec<FloatPoint>>,
}

/// Brings the paint state of `arena` up to date with what a clock tick's round laid out, for the tick's frame, as the
/// host's rendering update does before it records, where the round moved no root background, flipped no scrollability
/// and asks the host to resolve no SVG paint resource, and the update changes the tree incrementally. Answers the tree
/// where the update ran. A tree of a new structure takes over the compositor animations the host published for the old
/// one, whose nodes it holds still. The rest of paint preparation, the scroll offsets new overflow clamps above all, is
/// the host's, in its next rendering update.
pub(crate) fn prepare_for_clock_tick(
    arena: &mut LayoutNodeArena,
    viewport: NodeSlotId,
) -> Result<Option<ClockTickVisualContexts>, VisualContextsNeedHost> {
    // Recording reads overflow, and measuring it notes a flip in scrollability for the check below.
    arena.measure_scrollable_overflow();
    if arena.svg_paint_resources().needs_sync()
        || arena.paint_state().borrow().root_background_source
            != Some(crate::layout::viewport_propagation::root_background_source(arena))
    {
        return Err(VisualContextsNeedHost(
            "SVG paint resources or the root background moved",
        ));
    }
    let (inputs, compositor_animations) = {
        let paint_state = arena.paint_state().borrow();
        let state = &paint_state.visual_context;
        let (Some(inputs), Some(tree)) = (state.last_tree_inputs, state.tree.as_deref()) else {
            return Err(VisualContextsNeedHost("no visual context tree"));
        };
        if state.dirty_boxes.scope.rebuilds_every_box() {
            return Err(VisualContextsNeedHost("every box rebuilds its visual contexts"));
        }
        if state.dirty_boxes.boxes.is_empty() && state.dirty_boxes.removed.is_empty() {
            return Ok(None);
        }
        (inputs, tree.shared_visual_animations())
    };
    let outcome = update_accumulated_visual_contexts(arena, viewport, inputs);
    if outcome.performed_full_build {
        return Err(VisualContextsNeedHost("a full visual context tree build"));
    }
    let paintable_rows = arena.paintable_rows();
    let mut paint_state = arena.paint_state().borrow_mut();
    let state = &mut paint_state.visual_context;
    let tree = state.tree.as_mut().expect("an incremental update keeps the tree");
    if !outcome.structural_epoch_changed {
        return Ok(Some(ClockTickVisualContexts {
            tree: tree.clone(),
            restructured_scroll_offsets: None,
        }));
    }
    // The update dropped the animations, which name nodes of the old structure: the host publishes them again in its
    // rendering update, and an animation whose node went away needs it to.
    if !Arc::make_mut(tree).carry_visual_animations_over(compositor_animations) {
        return Err(VisualContextsNeedHost("animations of a node that went away"));
    }
    let tree = tree.clone();
    // The host refreshes its own copy of the scroll state still.
    let scroll_offsets = scroll_state_snapshot(&paintable_rows, state, inputs.device_pixels_per_css_pixel);
    Ok(Some(ClockTickVisualContexts {
        tree,
        restructured_scroll_offsets: Some(scroll_offsets),
    }))
}

/// Runs `pass` over the render state of `host`'s document in `read`, as the host prepares to paint, and answers what it
/// answered.
pub(crate) fn run<R>(
    read: &BegunRead,
    host: &DocumentHost,
    effect: PassEffect,
    pass: impl FnOnce(&mut LayoutNodeArena) -> R,
) -> R {
    if effect == PassEffect::RewritesRows {
        host.let_go_of_rows();
    }
    host.run(read, !effect.leaves_paint_preparation_current(), move |state| {
        pass(state.arena_mut())
    })
}

/// Proof that preparing a document for rendering has something to do, which only [`rendering_preparation_pending`]
/// mints: a host never sends a preparation that would answer that nothing changed. It carries the root background
/// source it was found with, for the preparation to take over.
pub(crate) struct PendingPreparation {
    root_background_source: RootBackgroundSource,
}

impl PendingPreparation {
    /// Settles the scrollable overflow left unmeasured, and with it the root background and, with `sticky_inputs`, the
    /// sticky constraints.
    pub(crate) fn prepare(
        self,
        arena: &LayoutNodeArena,
        sticky_inputs: Option<&FfiVisualContextTreeInputs>,
    ) -> RenderingPreparation {
        prepare_for_rendering(arena, self.root_background_source, sticky_inputs)
    }
}

/// Whether preparing `arena` for rendering would change anything: its root background source moved, or scrollable
/// overflow is left to settle or to measure, or a measurement changed what the preparation answers.
pub(crate) fn rendering_preparation_pending(arena: &LayoutNodeArena) -> Option<PendingPreparation> {
    let root_background_source = crate::layout::viewport_propagation::root_background_source(arena);
    let overflow = &arena.scrollable_overflow;
    let pending = arena.paint_state().borrow().root_background_source != Some(root_background_source)
        || overflow.full_layout_commit.get()
        || overflow.measurement_awaits_preparation()
        || arena.scrollable_overflow_recalculation_is_scheduled()
        || !arena.scrollable_overflow_is_measured();
    pending.then_some(PendingPreparation { root_background_source })
}

/// Asks the render state of `host`'s document in `read` whether preparing it for rendering has something to do, unless
/// the host knows it has nothing.
pub(crate) fn pending_preparation(read: &BegunRead, host: &DocumentHost) -> Option<PendingPreparation> {
    if host
        .known_arena_facts()
        .is_some_and(|facts| !facts.rendering_preparation_pending)
    {
        return None;
    }
    host.ask(read, |state| rendering_preparation_pending(state.arena_mut()))
}

/// Prepares `arena` for rendering. The sticky constraints the new overflow moved are refreshed with `sticky_inputs`, the
/// inputs of the visual context tree, unless there are none: a visual context update is pending, which refreshes them.
pub(crate) fn prepare_for_rendering(
    arena: &LayoutNodeArena,
    root_background_source: RootBackgroundSource,
    sticky_inputs: Option<&FfiVisualContextTreeInputs>,
) -> RenderingPreparation {
    let background_source_changed = arena
        .paint_state()
        .borrow_mut()
        .update_root_background_source(arena, root_background_source);
    // This measures all overflow left unmeasured, including the root's: the root background covers
    // the viewport united with it, so a flip in its scrollability is seen here rather than while
    // recording holds the paint state.
    let clamped_scroll_offsets = crate::painting::scrollable_overflow::update_scrollable_overflow(arena);
    let changed = arena.scrollable_overflow.geometry_changed.replace(false);
    let flipped = arena.scrollable_overflow.scrollability_changed.replace(false);
    let mut visual_context_values_changed = false;
    if let Some(inputs) = sticky_inputs.filter(|_| changed && !flipped) {
        let rows = arena.paintable_rows();
        let mut state = arena.paint_state().borrow_mut();
        let state = &mut state.visual_context;
        if let Some(tree) = state.tree.as_mut() {
            visual_context_values_changed = crate::painting::visual_context::refresh::refresh_sticky_constraints(
                &rows,
                &state.scroll_state,
                tree,
                inputs,
            );
        }
        state.needs_to_refresh_scroll_state = true;
    }
    RenderingPreparation {
        outcome: FfiRenderingPreparationOutcome {
            requires_display_list_recording: changed || background_source_changed,
            requires_visual_context_update: flipped,
            visual_context_values_changed,
        },
        clamped_scroll_offsets,
    }
}

pub(crate) fn refresh_scroll_state(
    arena: &LayoutNodeArena,
    device_pixels_per_css_pixel: f64,
) -> Option<Vec<FloatPoint>> {
    arena.measure_scrollable_overflow();
    let paintable_rows = arena.paintable_rows();
    let mut paint_state = arena.paint_state().borrow_mut();
    let state = &mut paint_state.visual_context;
    if !state.needs_to_refresh_scroll_state {
        return None;
    }
    let snapshot = scroll_state_snapshot(&paintable_rows, state, device_pixels_per_css_pixel);
    state.needs_to_refresh_scroll_state = false;
    Some(snapshot)
}

/// The device scroll offsets of the nodes of `state`'s tree, refreshed from the rows where they may have moved.
fn scroll_state_snapshot(
    paintable_rows: &impl crate::painting::paintable_rows::PaintableRowsRead,
    state: &mut VisualContextState,
    device_pixels_per_css_pixel: f64,
) -> Vec<FloatPoint> {
    if state.needs_to_refresh_scroll_state {
        crate::painting::visual_context::refresh::refresh_scroll_state(paintable_rows, &mut state.scroll_state);
    }
    let mut snapshot = state.scroll_state.snapshot(device_pixels_per_css_pixel);
    // https://drafts.csswg.org/css-position/#sticky-pos
    if let Some(tree) = state.tree.as_deref() {
        tree.resolve_sticky_offsets_in_place(&mut snapshot);
    }
    snapshot
}

pub(crate) fn update_visual_viewport_transform(arena: &LayoutNodeArena, inputs: &FfiVisualContextTreeInputs) {
    let mut paint_state = arena.paint_state().borrow_mut();
    if let Some(tree) = &mut paint_state.visual_context.tree {
        std::sync::Arc::make_mut(tree).set_visual_viewport_transform(
            crate::painting::visual_context::node_values::visual_viewport_transform_data(inputs),
        );
    }
}

pub(crate) fn update_accumulated_visual_contexts(
    arena: &mut LayoutNodeArena,
    viewport: NodeSlotId,
    inputs: FfiVisualContextTreeInputs,
) -> FfiVisualContextUpdateOutcome {
    arena.measure_scrollable_overflow();
    if !arena.paintable_row_is_populated(viewport) {
        return FfiVisualContextUpdateOutcome::default();
    }
    let mut state = std::mem::take(&mut arena.paint_state().borrow_mut().visual_context);
    state.release_quarantined_slots_while_no_handle_is_retained();

    let mut scope = state.dirty_boxes.scope;
    if state.tree.is_none() {
        scope = VisualContextUpdateScope::FreshTree;
    }
    let viewport_overflow = arena
        .paintable_rows()
        .node_style_if_live(viewport)
        .map_or((0, 0), |style| {
            let box_values = style.box_values();
            (box_values.overflow_x, box_values.overflow_y)
        });
    let viewport_overflow_changed =
        std::mem::replace(&mut state.last_viewport_overflow, viewport_overflow) != viewport_overflow;
    if viewport_overflow_changed {
        // The wheel targets of every hit-test item end at the viewport where it scrolls, which no box capture of the
        // last recording knows: none of its scroll metadata stands.
        arena.push_scroll_metadata_damage_everywhere();
    }
    if state.last_tree_inputs.is_some_and(|last| {
        last.device_pixels_per_css_pixel != inputs.device_pixels_per_css_pixel || viewport_overflow_changed
    }) {
        scope = scope.max(VisualContextUpdateScope::EveryBox);
    }
    if state.tree.as_deref().is_some_and(|tree| tree.should_compact()) {
        scope = VisualContextUpdateScope::FreshTree;
    }

    while scope != VisualContextUpdateScope::FreshTree {
        let result = update_visual_context_tree(&arena.paintable_rows(), viewport, inputs, scope, &mut state);
        match result {
            IncrementalUpdateResult::Applied(mut outcome) => {
                super::ffi::apply_walk_assignments(arena, &mut outcome);
                arena.resort_stacking_context_entries_flagged_for_resort();
                crate::painting::fragment_ownership::assign_fragment_ownership_for_pending_line_roots(arena);
                let performed_full_build = scope == VisualContextUpdateScope::EveryBox;
                if performed_full_build {
                    debug_assert_every_live_node_is_owned(
                        &arena.paintable_rows(),
                        state.tree.as_deref().expect("an applied walk keeps the tree"),
                        viewport,
                    );
                }
                state.dirty_boxes.clear();
                state.last_tree_inputs = Some(inputs);
                let outcome = FfiVisualContextUpdateOutcome {
                    performed_full_build,
                    structural_epoch_changed: outcome.delta.structural_epoch_changed,
                    requires_display_list_recording: outcome.delta.requires_display_list_recording,
                    structural_epoch: state.structural_epoch(),
                };
                arena.paint_state().borrow_mut().visual_context = state;
                return outcome;
            }
            IncrementalUpdateResult::NeedsFullBuild(fallback_scope) => {
                assert!(fallback_scope > scope, "a fallback widens the update scope");
                scope = fallback_scope;
            }
        }
    }

    let outcome = fresh_visual_context_tree_build(arena, viewport, inputs, &mut state);
    state.last_tree_inputs = Some(inputs);
    arena.paint_state().borrow_mut().visual_context = state;
    outcome
}

fn fresh_visual_context_tree_build(
    arena: &mut LayoutNodeArena,
    viewport: NodeSlotId,
    inputs: crate::painting::host::FfiVisualContextTreeInputs,
    state: &mut VisualContextState,
) -> FfiVisualContextUpdateOutcome {
    let mut fresh_tree = crate::painting::visual_context::build::create_fresh_tree_with_viewport_nodes(
        &arena.paintable_rows(),
        viewport,
        &inputs,
    );
    fresh_tree.viewport_assignment.node_identity = arena.unique_node_ids().id(viewport);
    {
        let mut paintable_rows = arena.paintable_rows_mut();
        paintable_rows.drop_all_visual_context_records();
        fresh_tree.viewport_assignment.apply(&mut paintable_rows);
    }
    state.tree = Some(std::sync::Arc::new(fresh_tree.tree));
    state.dirty_boxes.clear();
    let mut outcome = match update_visual_context_tree(
        &arena.paintable_rows(),
        viewport,
        inputs,
        VisualContextUpdateScope::FreshTree,
        state,
    ) {
        IncrementalUpdateResult::Applied(outcome) => *outcome,
        IncrementalUpdateResult::NeedsFullBuild(_) => {
            unreachable!("a fresh tree walk has a tree and a viewport record")
        }
    };
    // Everything records again; pushing that first keeps the per-row pushes below free.
    arena.push_all_paint_damage();
    super::ffi::apply_walk_assignments(arena, &mut outcome);
    arena.rebuild_all_stacking_context_entries_from_records(viewport);
    arena.take_line_roots_needing_fragment_ownership();
    crate::painting::fragment_ownership::assign_fragment_ownership(&arena.paintable_rows(), viewport);
    state.quarantined_slots_are_releasable = false;
    debug_assert_every_live_node_is_owned(
        &arena.paintable_rows(),
        state.tree.as_deref().expect("a fresh tree walk keeps the tree"),
        viewport,
    );
    FfiVisualContextUpdateOutcome {
        performed_full_build: true,
        structural_epoch_changed: true,
        requires_display_list_recording: true,
        structural_epoch: state.structural_epoch(),
    }
}

/// An SVG paint resource of an enrolled row the host resolves from the DOM.
pub(crate) enum SvgPaintResourceRequest {
    PaintServer {
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
    },
    /// A filter list, with the `url()` of each filter that names an SVG filter.
    Filter {
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
        urls: Vec<crate::css::style_value::RetainedStyleValueData>,
    },
}

/// What the host resolved of an [`SvgPaintResourceRequest`].
pub(crate) enum ResolvedSvgPaintResource {
    PaintServer {
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
        published: PublishedSvgPaintServer,
    },
    Filter {
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
        published: PublishedSvgFilter,
    },
}

/// What the host resolves of the enrolled SVG paint resources, if they changed since they were
/// resolved last. A row that went away, or whose filters name no SVG filter any more, leaves them.
pub(crate) fn svg_paint_resource_requests(arena: &LayoutNodeArena) -> Option<Vec<SvgPaintResourceRequest>> {
    let resources = arena.svg_paint_resources();
    if !resources.take_needs_sync() {
        return None;
    }
    let mut requests = Vec::new();
    for (slot, kind) in resources.enrolled_entries() {
        let Some(style) = arena.node_style_if_live(slot) else {
            resources.forget_slot(slot);
            continue;
        };
        if matches!(kind, SvgPaintResourceKind::Fill | SvgPaintResourceKind::Stroke) {
            requests.push(SvgPaintResourceRequest::PaintServer { slot, kind });
            continue;
        }
        let effects = style.effects();
        let filter_list = match kind {
            SvgPaintResourceKind::Filter => &effects.filter,
            SvgPaintResourceKind::BackdropFilter => &effects.backdrop_filter,
            SvgPaintResourceKind::Fill | SvgPaintResourceKind::Stroke => unreachable!(),
        };
        if !crate::painting::css_filter::contains_url(filter_list) {
            resources.withdraw(slot, kind);
            continue;
        }
        let urls = filter_list
            .operations
            .as_slice()
            .iter()
            .filter(|operation| operation.kind == crate::painting::css_filter::FILTER_KIND_URL)
            .map(|operation| {
                // SAFETY: The row's style holds the value live; the request holds a reference of its
                // own, as the host may restyle the row while it resolves.
                unsafe {
                    crate::css::style_value::RetainedStyleValueData::from_retained_optional_pointer(
                        crate::css::style_value::retain_style_value(operation.url_value.pointer.cast()),
                    )
                }
            })
            .collect();
        requests.push(SvgPaintResourceRequest::Filter { slot, kind, urls });
    }
    Some(requests)
}

/// Publishes what the host resolved of the enrolled SVG paint resources, and answers whether any
/// changed.
pub(crate) fn publish_resolved_svg_paint_resources(
    arena: &LayoutNodeArena,
    resolved: Vec<ResolvedSvgPaintResource>,
) -> bool {
    use crate::painting::record::damage::PaintDamage;
    let resources = arena.svg_paint_resources();
    let mut any_changed = false;
    for resolved in resolved {
        match resolved {
            ResolvedSvgPaintResource::PaintServer { slot, kind, published } => {
                if arena.slot_is_live(slot) && resources.publish_paint_server(slot, kind, published) {
                    any_changed = true;
                    arena.push_paint_damage(slot, PaintDamage::SVG | PaintDamage::SCOPE_PREAMBLE);
                }
            }
            ResolvedSvgPaintResource::Filter { slot, kind, published } => {
                if !arena.slot_is_live(slot) || !resources.publish_filter(slot, kind, published) {
                    continue;
                }
                any_changed = true;
                if arena.paintable_row_is_populated(slot) {
                    arena.note_visual_context_box_dirty(
                        slot,
                        crate::painting::visual_context::dirty::VisualContextBoxDirtyKind::StyleValueChange,
                    );
                    arena.push_paint_damage(slot, PaintDamage::SVG | PaintDamage::SCOPE_PREAMBLE);
                }
            }
        }
    }
    any_changed
}
