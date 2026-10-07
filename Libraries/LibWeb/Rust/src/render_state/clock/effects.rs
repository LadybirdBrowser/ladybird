/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the ticks of a lane sample of each element, through one path: the animations the host runs on the element, or
//! transitions the lane's hover started on it. Each samples over a record, shows in the element's box with the visual
//! contexts it moves, and shows what it animates that descendants inherit in their boxes.

use super::{Lane, Park};
use crate::css::animated_overlay::AnimatedOverlay;
use crate::css::style::animations::AnimationTimelineSamples;
use crate::css::style::bridge::FfiStyleDelta;
use crate::css::style::engine_sample::NeedsHost;
use crate::css::style::hover_lane::{INHERITING_DESCENDANTS_LIMIT, InheritingDescendants};
use crate::css::style::layout_style::DerivedStyleRecord;
use crate::css::style::tree::StyleNodeID;
use crate::css::style::{SampleBounds, StyleEngine};
use crate::css::transition::HoverTransitions;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{GENERATED_FOR_AFTER, GENERATED_FOR_BEFORE, GENERATED_FOR_MARKER, NodeSlotId};
use crate::painting::record::damage::PaintDamage;
use crate::render_state::RenderState;

/// What the ticks of a lane sample of an element.
pub(crate) enum ElementEffects {
    /// The animations the host runs on the element, whose timing it published, sampled over the record the host
    /// installed in the element's box at the timeline times of the tick, with the descendants that inherit what they
    /// animate once a tick has found them.
    Host {
        node: StyleNodeID,
        inheriting: Option<Inheriting>,
    },
    /// Transitions the lane's hover started on the element at `start_time`, in the document's milliseconds, sampled over
    /// their after-change style until they end.
    Started {
        transitions: HoverTransitions,
        start_time: f64,
        ended: bool,
    },
}

impl ElementEffects {
    fn node(&self) -> StyleNodeID {
        match self {
            Self::Host { node, .. } => *node,
            Self::Started { transitions, .. } => transitions.node,
        }
    }

    fn runs_started_transitions(&self) -> bool {
        matches!(self, Self::Started { ended: false, .. })
    }
}

/// The descendants of an element that inherit what its effects animate, which show it with the element, and whether
/// every styled element in its subtree does.
pub(crate) struct Inheriting {
    descendants: InheritingDescendants,
    covers_subtree: bool,
}

/// How a sample shows in a box: as one of the host's animations, or as transitions a hover started, which leave the
/// records the host pinned for its own readers theirs until the host starts the transitions in its turn.
#[derive(Clone, Copy)]
enum SampleKind {
    Animation,
    Transition,
}

impl Lane {
    /// The effects the ticks sample of the elements the plan animates.
    pub(super) fn host_effects(elements: &[StyleNodeID]) -> Vec<ElementEffects> {
        elements
            .iter()
            .map(|&node| ElementEffects::Host { node, inheriting: None })
            .collect()
    }

    /// Whether a transition the hover started has yet to end, which the ticks sample until it does, or until a tick
    /// leaves its frame to the host.
    pub(super) fn transitions_run(&self) -> bool {
        !self.frame_left_to_host && self.effects.iter().any(ElementEffects::runs_started_transitions)
    }

    /// Whether a transition the hover started on the element `node` names has yet to end.
    pub(super) fn transitions_run_on(&self, node: StyleNodeID) -> bool {
        self.effects
            .iter()
            .any(|effects| effects.runs_started_transitions() && effects.node() == node)
    }

    /// The transitions the hover started, with when it started them.
    pub(super) fn started_transitions(&self) -> impl Iterator<Item = (&HoverTransitions, f64)> {
        self.effects.iter().filter_map(|effects| match effects {
            ElementEffects::Started {
                transitions,
                start_time,
                ..
            } => Some((transitions, *start_time)),
            ElementEffects::Host { .. } => None,
        })
    }

    /// Has the ticks sample `transitions`, which the hover started at `start_time`, in place of the animations the host
    /// runs on their element: the step the hover decided over those replaced them.
    pub(super) fn start_transitions(&mut self, transitions: HoverTransitions, start_time: f64) {
        let node = transitions.node;
        self.effects
            .retain(|effects| !matches!(effects, ElementEffects::Host { node: animated, .. } if *animated == node));
        self.effects.push(ElementEffects::Started {
            transitions,
            start_time,
            ended: false,
        });
    }

    /// Has the ticks sample the animations of the elements `plan` animates, in place of those of the plan before, beside
    /// the transitions the hover started.
    pub(super) fn replan_host_effects(&mut self, elements: &[StyleNodeID]) {
        self.effects
            .retain(|effects| matches!(effects, ElementEffects::Started { .. }));
        let started: smallvec::SmallVec<[StyleNodeID; 4]> = self.effects.iter().map(ElementEffects::node).collect();
        self.effects.extend(
            elements
                .iter()
                .filter(|node| !started.contains(node))
                .map(|&node| ElementEffects::Host { node, inheriting: None }),
        );
    }

    /// Samples the animations the host runs at `samples`, and shows them in their elements' boxes and in those of the
    /// descendants that inherit what they animate. An animated element with no box, more such descendants than a tick
    /// restyles, or a sample only the host computes, parks the lane.
    pub(super) fn sample_host_animations(
        &mut self,
        state: &mut RenderState,
        samples: AnimationTimelineSamples<'_>,
    ) -> Result<(), Park> {
        let scroll_snaps = state.arena.arena().may_have_scroll_snap_areas();
        for index in 0..self.effects.len() {
            let ElementEffects::Host {
                node,
                ref mut inheriting,
            } = self.effects[index]
            else {
                continue;
            };
            let row = state.arena.arena().bound_row(node);
            if row.is_invalid() {
                return Err(Park("an animated element has no box"));
            }
            let inheriting = match inheriting.take() {
                Some(inheriting) => inheriting,
                None => Self::find_inheriting(state, node)?,
            };
            // Every tick samples over the record the host installed.
            let host_record = self.host_record(state, row);
            let arena = state.arena.arena();
            let reference_box = crate::painting::ffi::committed_transform_reference_box(&arena.paintable_rows(), row);
            let shown = state
                .engine_mut()
                .sample_at(
                    node,
                    host_record,
                    samples,
                    reference_box,
                    SampleBounds {
                        scroll_snaps,
                        subtree_follows: inheriting.covers_subtree,
                    },
                )
                .and_then(|(sample, overlay)| {
                    self.show_sample(state, row, node, sample, SampleKind::Animation)?;
                    self.show_inherited(
                        state,
                        &overlay,
                        &inheriting.descendants,
                        SampleKind::Animation,
                        scroll_snaps,
                        inheriting.covers_subtree,
                    )
                });
            if let ElementEffects::Host { inheriting: slot, .. } = &mut self.effects[index] {
                *slot = Some(inheriting);
            }
            shown?;
        }
        Ok(())
    }

    /// The descendants of the element `node` names that inherit what the animations the host runs on it animate.
    fn find_inheriting(state: &RenderState, node: StyleNodeID) -> Result<Inheriting, Park> {
        let engine = state.engine_ref();
        let properties = engine.animated_inherited_properties(node);
        if properties.is_empty() {
            return Ok(Inheriting {
                descendants: Vec::new(),
                covers_subtree: false,
            });
        }
        let (descendants, covers_subtree) = engine
            .inheriting_descendants(node, &properties, INHERITING_DESCENDANTS_LIMIT)
            .ok_or(Park("more descendants inherit an animation than a tick restyles"))?;
        Ok(Inheriting {
            descendants,
            covers_subtree,
        })
    }

    /// The elements whose boxes the ticks show samples in: those whose effects they sample, and the descendants found
    /// to inherit what those animate.
    pub(super) fn sampled_elements(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        self.effects.iter().flat_map(|effects| {
            let inheriting = match effects {
                ElementEffects::Host { inheriting, .. } => {
                    inheriting.as_ref().map(|inheriting| &inheriting.descendants)
                }
                ElementEffects::Started { transitions, .. } => Some(&transitions.inheriting),
            };
            std::iter::once(effects.node()).chain(
                inheriting
                    .into_iter()
                    .flat_map(|descendants| descendants.iter().map(|(descendant, _)| *descendant)),
            )
        })
    }

    /// Shows the transitions the hover started at `timestamp`, and the after-change style of those that have ended by
    /// then. Answers whether that moved anything to lay out and present.
    pub(super) fn sample_started_transitions(&mut self, state: &mut RenderState, timestamp: f64) -> bool {
        let mut moved = false;
        // A plan the hover took for its own tick leaves the snapping of the document unknown.
        let scroll_snaps = self.plan.hover.as_ref().is_none_or(|plan| plan.scroll_snaps);
        for index in 0..self.effects.len() {
            let ElementEffects::Started {
                transitions,
                start_time,
                ended: false,
            } = &self.effects[index]
            else {
                continue;
            };
            let node = transitions.node;
            let elapsed = timestamp - start_time;
            let inheriting = transitions.inheriting.clone();
            let subtree_follows = transitions.inheriting_covers_subtree;
            let row = state.arena.arena().bound_row(node);
            if elapsed >= transitions.duration {
                if let ElementEffects::Started { ended, .. } = &mut self.effects[index] {
                    *ended = true;
                }
                // The boxes show the after-change styles of the element and of what inherits from it.
                moved |= self.show_host_styles(state, row, &inheriting);
                continue;
            }
            if row.is_invalid() {
                continue;
            }
            let (engine, arena) = state.engine_and_arena();
            let reference_box = super::hover::transform_reference_box(arena, node);
            let Ok((sample, overlay)) = transitions.sample(engine, elapsed, reference_box, scroll_snaps) else {
                continue;
            };
            if self
                .show_sample(state, row, node, sample, SampleKind::Transition)
                .is_err()
            {
                continue;
            }
            // NB: A descendant whose box takes no sample keeps the style the host installed in it.
            let _ = self.show_inherited(
                state,
                &overlay,
                &inheriting,
                SampleKind::Transition,
                scroll_snaps,
                subtree_follows,
            );
            moved = true;
        }
        moved
    }

    /// The record the host installed in the box `row`, which a tick may show a sample in meanwhile.
    fn host_record(&self, state: &RenderState, row: NodeSlotId) -> u64 {
        self.ticked.iter().find(|(ticked, _)| *ticked == row).map_or_else(
            || state.arena.arena().node_style_record(row),
            |(_, host_style)| host_style.record(),
        )
    }

    /// Shows `sample`, of the effects of the element `node` names, in its box `row`, with the visual contexts the move
    /// from what the box showed moves, keeping the style the host installed in the box.
    fn show_sample(
        &mut self,
        state: &mut RenderState,
        row: NodeSlotId,
        node: StyleNodeID,
        sample: DerivedStyleRecord,
        kind: SampleKind,
    ) -> Result<(), NeedsHost> {
        let shown = state.arena.arena().node_style_record(row);
        let damage = state
            .engine_mut()
            .element_record_damage(node, false, shown, sample.record);
        let arena = state.arena.arena();
        let host_style = match kind {
            SampleKind::Animation => arena.install_animation_sample(row, sample)?,
            SampleKind::Transition => arena.install_transition_sample(row, sample)?,
        };
        self.ticked.extend(host_style.map(|host_style| (row, host_style)));
        arena.note_style_visual_context_moves(row, damage);
        arena.push_paint_damage_for_repaint(row, PaintDamage::ALL_PRODUCERS);
        Ok(())
    }

    /// Shows what `overlay`, a sample of `kind`, animates that each of the `inheriting` descendants inherits in its box,
    /// over the record the host installed there. A descendant with no box shows nothing; one whose box takes no sample
    /// fails the whole, after the others show theirs.
    fn show_inherited(
        &mut self,
        state: &mut RenderState,
        overlay: &AnimatedOverlay,
        inheriting: &InheritingDescendants,
        kind: SampleKind,
        scroll_snaps: bool,
        subtree_follows: bool,
    ) -> Result<(), NeedsHost> {
        let mut shown = Ok(());
        for (descendant, properties) in inheriting {
            let row = state.arena.arena().bound_row(*descendant);
            if row.is_invalid() {
                continue;
            }
            let host_record = self.host_record(state, row);
            let mut inherited = AnimatedOverlay::default();
            for &property in properties {
                if let Some(entry) = overlay.get(property) {
                    inherited.set_owned(property, entry.clone_value(), true, false);
                }
            }
            let sample = state.engine_mut().compose_overlay_over_record(
                *descendant,
                host_record,
                &inherited,
                SampleBounds {
                    scroll_snaps,
                    subtree_follows,
                },
            );
            if let Err(needs_host) = sample.and_then(|sample| self.show_sample(state, row, *descendant, sample, kind)) {
                shown = Err(needs_host);
            }
        }
        shown
    }

    /// Has the box `row`, and those of the `inheriting` descendants, show the styles the host installed in them again,
    /// where a tick showed a sample in them. Answers whether any did.
    fn show_host_styles(
        &mut self,
        state: &mut RenderState,
        row: NodeSlotId,
        inheriting: &InheritingDescendants,
    ) -> bool {
        let arena = state.arena.arena();
        let rows: smallvec::SmallVec<[NodeSlotId; 4]> = std::iter::once(row)
            .chain(inheriting.iter().map(|(descendant, _)| arena.bound_row(*descendant)))
            .collect();
        let mut moved = false;
        for row in rows {
            if let Some(position) = self.ticked.iter().position(|(ticked, _)| *ticked == row) {
                let (row, host_style) = self.ticked.remove(position);
                let arena = state.arena.arena();
                arena.restore_host_style(row, host_style);
                arena.push_paint_damage_for_repaint(row, PaintDamage::ALL_PRODUCERS);
                moved = true;
            }
        }
        moved
    }
}

/// Whether a `::before`, `::after` or `::marker` box of the element `node` names takes any of `properties` from it, or
/// one of the `inheriting` descendants' any of those it inherits, as their records before `rows` and in them: such a box
/// shows what the host composes over the element's effects, which no tick shows in it.
pub(super) fn pseudo_element_boxes_inherit(
    engine: &StyleEngine,
    arena: &LayoutNodeArena,
    node: StyleNodeID,
    properties: &[u16],
    inheriting: &InheritingDescendants,
    rows: &[FfiStyleDelta],
) -> bool {
    std::iter::once((node, properties))
        .chain(
            inheriting
                .iter()
                .map(|(descendant, properties)| (*descendant, &properties[..])),
        )
        .any(|(element, properties)| {
            [GENERATED_FOR_BEFORE, GENERATED_FOR_AFTER, GENERATED_FOR_MARKER]
                .iter()
                .any(|&pseudo| !arena.bound_pseudo_element_row(element, pseudo).is_invalid())
                && engine.box_pseudo_elements_inherit_any_of(element, rows, properties)
        })
}
