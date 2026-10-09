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
use crate::css::transition::{HoverTransitions, LaneTransitions};
use crate::layout::node_data::{GENERATED_FOR_AFTER, GENERATED_FOR_BEFORE, GENERATED_FOR_MARKER, NodeSlotId};
use crate::layout::{LayoutNodeArena, SampleKind};
use crate::painting::host::FfiVisualAnimationTargetKind;
use crate::painting::record::damage::PaintDamage;
use crate::painting::visual_animation::VisualAnimation;
use crate::render_state::RenderState;
use smallvec::SmallVec;
use std::sync::Arc;

/// What the ticks of a lane sample of an element.
pub(crate) enum ElementEffects {
    /// The animations the host runs on the element, whose timing it published, sampled over the record the host
    /// installed in the element's box at the timeline times of the tick, with the descendants that inherit what they
    /// animate once a tick has found them.
    Host {
        node: StyleNodeID,
        inheriting: Option<Inheriting>,
    },
    /// The transitions a step the lane's hover decided at `start_time`, in the document's milliseconds, leaves the element
    /// running, those it started and those of the host's it left running, sampled over their after-change style while
    /// they `run`: until they end, or a tick finds a box that takes no sample of them,
    /// which leaves them to the host as the boxes showed them last. Once a rendering update `taken_in` the step, the
    /// host runs them as the lane does.
    Started {
        transitions: Arc<HoverTransitions>,
        start_time: f64,
        run: bool,
        taken_in: bool,
    },
}

impl ElementEffects {
    fn node(&self) -> StyleNodeID {
        match self {
            Self::Host { node, .. } => *node,
            Self::Started { transitions, .. } => transitions.node,
        }
    }

    pub(super) fn is_host(&self) -> bool {
        matches!(self, Self::Host { .. })
    }

    fn runs_started_transitions(&self) -> bool {
        matches!(self, Self::Started { run: true, .. })
    }
}

/// A sample of an element's effects to show in a box: the box, the element whose style it is, the sample, and the
/// properties it animates.
type BoxSample<'a> = (NodeSlotId, StyleNodeID, DerivedStyleRecord, &'a [u16]);

/// The descendants of an element that inherit what its effects animate, which show it with the element, and whether
/// every styled element in its subtree does.
pub(crate) struct Inheriting {
    descendants: InheritingDescendants,
    covers_subtree: bool,
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

    /// The transitions the hover started on the element `node` names, which run on it in place of the host's, where
    /// it started any: a step of the element's decides over them.
    pub(super) fn lane_transitions(&self, node: StyleNodeID) -> Option<LaneTransitions<'_>> {
        self.effects.iter().find_map(|effects| match effects {
            ElementEffects::Started {
                transitions,
                start_time,
                ..
            } if transitions.node == node => Some(LaneTransitions {
                transitions,
                start_time: *start_time,
            }),
            _ => None,
        })
    }

    /// Takes the steps the hover decided that no rendering update took in yet, with the transitions each leaves its
    /// element running, which the host runs as the lane does from now on.
    pub(super) fn take_started_transitions(&mut self) -> Vec<Arc<HoverTransitions>> {
        self.effects
            .iter_mut()
            .filter_map(|effects| match effects {
                ElementEffects::Started {
                    transitions,
                    taken_in: taken_in @ false,
                    ..
                } => {
                    *taken_in = true;
                    Some(Arc::clone(transitions))
                }
                _ => None,
            })
            .collect()
    }

    /// Has the ticks sample `transitions`, which the hover started at `start_time`, in place of the animations the host
    /// runs on their element, or of the transitions the hover started on it before: the step the hover decided over
    /// those replaced them. Answers the transitions the hover started before, which a move half made gives back.
    pub(super) fn start_transitions(
        &mut self,
        transitions: HoverTransitions,
        start_time: f64,
    ) -> Option<ElementEffects> {
        let transitions = Arc::new(transitions);
        let node = transitions.node;
        self.effects
            .retain(|effects| !matches!(effects, ElementEffects::Host { node: animated, .. } if *animated == node));
        let replaced = self
            .effects
            .iter()
            .position(|effects| effects.node() == node)
            .map(|position| self.effects.remove(position));
        self.effects.push(ElementEffects::Started {
            transitions,
            start_time,
            run: true,
            taken_in: false,
        });
        replaced
    }

    /// Forgets the transitions a move of the hover started on the elements `nodes` at `start_time`, which no frame
    /// showed, and has the ticks sample those they `replaced` again, short of those the move started itself, as where
    /// two waves of it restyled one element.
    pub(super) fn forget_transitions_started_at(
        &mut self,
        nodes: &[StyleNodeID],
        start_time: f64,
        replaced: Vec<ElementEffects>,
    ) {
        let started_by_the_move = |effects: &ElementEffects| match effects {
            ElementEffects::Started {
                transitions,
                start_time: started_at,
                ..
            } => nodes.contains(&transitions.node) && *started_at == start_time,
            ElementEffects::Host { .. } => false,
        };
        self.effects.retain(|effects| !started_by_the_move(effects));
        self.effects
            .extend(replaced.into_iter().filter(|effects| !started_by_the_move(effects)));
    }

    /// Has the ticks sample the animations of the elements `plan` animates, in place of those of the plan before, beside
    /// the transitions the hover started. The boxes of an element the plan animates no more show the styles the host
    /// installed in them again, where the lane has a fork.
    pub(super) fn replan_host_effects(&mut self, elements: &[StyleNodeID]) {
        let unplanned: SmallVec<[StyleNodeID; 4]> = self
            .effects
            .iter()
            .filter(|effects| effects.is_host() && !elements.contains(&effects.node()))
            .map(ElementEffects::node)
            .collect();
        if !unplanned.is_empty()
            && let super::LaneState::Forked(mut fork) = std::mem::take(&mut self.state)
        {
            for &node in &unplanned {
                self.show_host_styles_of_started(&mut fork, node);
            }
            self.state = super::LaneState::Forked(fork);
        }
        self.effects
            .retain(|effects| !unplanned.contains(&effects.node()) || !effects.is_host());
        let kept: smallvec::SmallVec<[StyleNodeID; 4]> = self.effects.iter().map(ElementEffects::node).collect();
        self.effects.extend(
            elements
                .iter()
                .filter(|node| !kept.contains(node))
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
                    self.show_element(
                        state,
                        (row, node, sample),
                        &overlay,
                        &inheriting.descendants,
                        SampleKind::Animation,
                        SampleBounds {
                            scroll_snaps,
                            subtree_follows: inheriting.covers_subtree,
                        },
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
        Self::elements_shown_by(self.effects.iter())
    }

    /// The elements whose boxes the ticks show samples in again as they go: those of `sampled_elements`, short of those
    /// of transitions the ticks left to the host, whose boxes keep what they showed.
    pub(super) fn resampled_elements(&self) -> impl Iterator<Item = StyleNodeID> + '_ {
        Self::elements_shown_by(
            self.effects
                .iter()
                .filter(|effects| !matches!(effects, ElementEffects::Started { run: false, .. })),
        )
    }

    fn elements_shown_by<'a>(
        effects: impl Iterator<Item = &'a ElementEffects> + 'a,
    ) -> impl Iterator<Item = StyleNodeID> + 'a {
        effects.flat_map(|effects| {
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
    /// then. Answers whether that moved anything to lay out and present, or the park of a tick that left transitions to
    /// the host, as a box took no sample of them, after it showed the others.
    pub(super) fn sample_started_transitions(&mut self, state: &mut RenderState, timestamp: f64) -> Result<bool, Park> {
        let mut moved = false;
        let mut left_to_host = None;
        let scroll_snaps = state.arena.arena().may_have_scroll_snap_areas();
        for index in 0..self.effects.len() {
            let ElementEffects::Started {
                transitions,
                start_time,
                run: true,
                ..
            } = &self.effects[index]
            else {
                continue;
            };
            let node = transitions.node;
            let elapsed = timestamp - start_time;
            let ended = elapsed >= transitions.duration;
            let inheriting = transitions.inheriting.clone();
            let bounds = SampleBounds {
                scroll_snaps,
                subtree_follows: transitions.inheriting_covers_subtree,
            };
            let row = state.arena.arena().bound_row(node);
            let shown = if ended {
                // The boxes show the after-change styles of the element and of what inherits from it.
                let properties: SmallVec<[u16; 8]> = transitions.properties().iter().copied().collect();
                moved |= self.show_host_styles(state, row, &properties, &inheriting);
                None
            } else if row.is_invalid() {
                // An element with no box shows nothing of its transitions, which the ticks sample no more.
                if let ElementEffects::Started { run, .. } = &mut self.effects[index] {
                    *run = false;
                }
                continue;
            } else {
                let (engine, arena) = state.engine_and_arena();
                let reference_box = super::hover::transform_reference_box(arena, node);
                let shown = transitions
                    .sample(engine, elapsed, reference_box, scroll_snaps)
                    .and_then(|(sample, overlay)| {
                        self.show_element(
                            state,
                            (row, node, sample),
                            &overlay,
                            &inheriting,
                            SampleKind::Transition,
                            bounds,
                        )
                    });
                moved |= shown.is_ok();
                shown.err()
            };
            if (ended || shown.is_some())
                && let ElementEffects::Started { run, .. } = &mut self.effects[index]
            {
                *run = false;
            }
            if shown.is_some() {
                left_to_host = Some(Park("a box that takes no sample of a transition"));
            }
        }
        left_to_host.map_or(Ok(moved), Err)
    }

    /// The record the host installed in the box `row`, which a tick may show a sample in meanwhile.
    fn host_record(&self, state: &RenderState, row: NodeSlotId) -> u64 {
        self.ticked.iter().find(|(ticked, _)| *ticked == row).map_or_else(
            || state.arena.arena().node_style_record(row),
            |(_, host_style)| host_style.record(),
        )
    }

    /// Shows `sample`, of the effects of the element `node` names, which animate `properties`, in its box `row`, with
    /// the visual contexts the move from what the box showed moves, keeping the style the host installed in the box.
    fn show_sample(
        &mut self,
        state: &mut RenderState,
        row: NodeSlotId,
        node: Option<StyleNodeID>,
        sample: DerivedStyleRecord,
        properties: &[u16],
        kind: SampleKind,
    ) {
        let shown = state.arena.arena().node_style_record(row);
        let engine = state.engine_mut();
        // A sample holds the values it animates in its payloads alone, which a comparison of the records' longhand
        // tables does not see.
        let damage = node.map_or(0, |node| {
            engine.element_record_damage(node, false, shown, sample.record)
        }) | engine.sampled_properties_damage(shown, sample.record, properties);
        let arena = state.arena.arena();
        let host_style = arena.install_sample_beside_anonymous_boxes(row, sample);
        // The box shows what the sample of transitions the hover started animates of what the compositor animates, in
        // place of the compositor. The compositor runs the host's animations as the ticks sample them, and on once the
        // lane presents no more.
        if kind == SampleKind::Transition {
            let kinds: SmallVec<[FfiVisualAnimationTargetKind; 2]> = properties
                .iter()
                .filter_map(|&property| compositor_animation_kind(property))
                .collect();
            stop_compositor_animations(arena, row, &kinds);
        }
        self.ticked.extend(host_style.map(|host_style| (row, host_style)));
        arena.note_style_visual_context_moves(row, damage);
        arena.push_paint_damage_for_repaint(row, PaintDamage::ALL_PRODUCERS);
    }

    /// Shows `element`'s sample of the effects of the element in its box, and what `overlay`, the values it animates,
    /// gives each of the `inheriting` descendants in its box, over the record the host installed there, all at once:
    /// where one of the boxes takes no sample, none of them shows one. A descendant with no box shows nothing.
    fn show_element(
        &mut self,
        state: &mut RenderState,
        element: (NodeSlotId, StyleNodeID, DerivedStyleRecord),
        overlay: &AnimatedOverlay,
        inheriting: &InheritingDescendants,
        kind: SampleKind,
        bounds: SampleBounds,
    ) -> Result<(), NeedsHost> {
        let (element_row, element_node, element_sample) = element;
        let animated: SmallVec<[u16; 8]> = overlay.entries().iter().map(|entry| entry.property).collect();
        let mut samples: SmallVec<[BoxSample<'_>; 4]> =
            smallvec::smallvec![(element_row, element_node, element_sample, &animated[..])];
        let mut composed = Ok(());
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
            match state
                .engine_mut()
                .compose_overlay_over_record(*descendant, host_record, &inherited, bounds)
            {
                Ok(sample) => samples.push((row, *descendant, sample, &properties[..])),
                Err(needs_host) => {
                    composed = Err(needs_host);
                    break;
                }
            }
        }
        // The anonymous boxes below a box show what they inherit of its sample beside it, as the host reinherits them.
        let arena = state.arena.arena();
        let mut anonymous: SmallVec<[(NodeSlotId, DerivedStyleRecord, &[u16]); 2]> = SmallVec::new();
        let mut takes = composed.is_ok();
        for (row, _, sample, properties) in &samples {
            if !takes {
                break;
            }
            if arena.takes_sample(*row, sample, kind) {
                continue;
            }
            takes = arena.takes_sample_beside_anonymous_boxes(*row, kind)
                && arena
                    .anonymous_child_samples(*row, sample, |child| self.host_record(state, child))
                    .map(|children| {
                        anonymous.extend(children.into_iter().map(|(child, sample)| (child, sample, *properties)));
                    })
                    .is_some();
        }
        if !takes {
            let engine = state.engine_mut();
            for (_, _, sample, _) in samples {
                engine.unpin_layout_style_record(sample.record);
            }
            for (_, sample, _) in anonymous {
                engine.unpin_layout_style_record(sample.record);
            }
            return Err(NeedsHost);
        }
        for (row, node, sample, properties) in samples {
            self.show_sample(state, row, Some(node), sample, properties, kind);
        }
        for (row, sample, properties) in anonymous {
            self.show_sample(state, row, None, sample, properties, kind);
        }
        Ok(())
    }

    /// Has the box `row`, whose sample animated `properties`, and those of the `inheriting` descendants, show the styles
    /// the host installed in them again, with the visual contexts that moves, where a tick showed a sample in them.
    /// Answers whether any did.
    fn show_host_styles(
        &mut self,
        state: &mut RenderState,
        row: NodeSlotId,
        properties: &[u16],
        inheriting: &InheritingDescendants,
    ) -> bool {
        let arena = state.arena.arena();
        let rows: smallvec::SmallVec<[(NodeSlotId, &[u16]); 4]> = std::iter::once((row, properties))
            .chain(
                inheriting
                    .iter()
                    .map(|(descendant, properties)| (arena.bound_row(*descendant), &properties[..])),
            )
            .collect();
        self.show_host_styles_in(state, rows)
    }

    /// Has the box of the element `node`, and those of the descendants that inherit what the transitions the hover
    /// started on it, or those the host runs on it, animate, show the styles the host installed in them again, as a
    /// step decided over those transitions replaces them, with the visual contexts that moves.
    /// Answers whether any did.
    pub(super) fn show_host_styles_of_started(&mut self, state: &mut RenderState, node: StyleNodeID) -> bool {
        let row = state.arena.arena().bound_row(node);
        let (properties, inheriting) = self
            .effects
            .iter()
            .find_map(|effects| match effects {
                ElementEffects::Started { transitions, .. } if transitions.node == node => Some((
                    SmallVec::<[u16; 2]>::from_slice(transitions.properties()),
                    transitions.inheriting.clone(),
                )),
                ElementEffects::Host {
                    node: animated,
                    inheriting,
                } if *animated == node => Some((
                    state.engine_ref().animated_properties(node),
                    inheriting
                        .as_ref()
                        .map(|inheriting| inheriting.descendants.clone())
                        .unwrap_or_default(),
                )),
                _ => None,
            })
            .unwrap_or_default();
        self.show_host_styles(state, row, &properties, &inheriting)
    }

    /// Has the box of the element `node`, which shows what it inherits of the samples of elements above it, show the
    /// style the host installed in it again, with the visual contexts that moves. Answers whether it did.
    pub(super) fn show_host_style_of_inheriting(&mut self, state: &mut RenderState, node: StyleNodeID) -> bool {
        let mut properties: SmallVec<[u16; 8]> = SmallVec::new();
        for effects in &self.effects {
            let inheriting = match effects {
                ElementEffects::Host { inheriting, .. } => {
                    inheriting.as_ref().map(|inheriting| &inheriting.descendants)
                }
                ElementEffects::Started { transitions, .. } => Some(&transitions.inheriting),
            };
            for (_, inherited) in inheriting
                .into_iter()
                .flatten()
                .filter(|(descendant, _)| *descendant == node)
            {
                for &property in inherited {
                    if !properties.contains(&property) {
                        properties.push(property);
                    }
                }
            }
        }
        let row = state.arena.arena().bound_row(node);
        self.show_host_styles_in(state, smallvec::smallvec![(row, &properties[..])])
    }

    /// Whether a box would show the samples of two elements' effects at once, were the ticks to sample `started` beside
    /// the effects they sample: an element `started` runs on, or one whose effects the ticks sample, inherits what the
    /// other's effects animate. Each sample composes over a record that holds nothing of the other's.
    pub(super) fn samples_overlap(&self, state: &RenderState, started: &[&HoverTransitions]) -> bool {
        let replaced = |node: StyleNodeID| started.iter().any(|transitions| transitions.node == node);
        let mut sampled: SmallVec<[(StyleNodeID, SmallVec<[StyleNodeID; 4]>); 4]> = SmallVec::new();
        let descendants_of =
            |inheriting: &InheritingDescendants| inheriting.iter().map(|(descendant, _)| *descendant).collect();
        for effects in self.effects.iter().filter(|effects| !replaced(effects.node())) {
            let descendants = match effects {
                ElementEffects::Host {
                    inheriting: Some(inheriting),
                    ..
                } => descendants_of(&inheriting.descendants),
                ElementEffects::Host { node, inheriting: None } => match Self::find_inheriting(state, *node) {
                    Ok(inheriting) => descendants_of(&inheriting.descendants),
                    Err(_) => return true,
                },
                ElementEffects::Started { transitions, .. } => descendants_of(&transitions.inheriting),
            };
            sampled.push((effects.node(), descendants));
        }
        sampled.extend(
            started
                .iter()
                .map(|transitions| (transitions.node, descendants_of(&transitions.inheriting))),
        );
        sampled.iter().enumerate().any(|(index, (node, _))| {
            sampled
                .iter()
                .enumerate()
                .any(|(other, (_, descendants))| other != index && descendants.contains(node))
        })
    }

    /// Finds again the descendants that inherit what the effects animate, which a move of the hover may have moved: an
    /// element whose style now declares what it inherited shows its own style, and one that inherits it now shows the
    /// samples. More such descendants than a tick restyles leave the move to the host.
    pub(super) fn find_inheriting_again(&mut self, state: &RenderState) -> Result<(), Park> {
        for index in 0..self.effects.len() {
            match &self.effects[index] {
                ElementEffects::Host { node, inheriting } => {
                    if inheriting.is_some() {
                        let found = Self::find_inheriting(state, *node)?;
                        self.effects[index] = ElementEffects::Host {
                            node: *node,
                            inheriting: Some(found),
                        };
                    }
                }
                ElementEffects::Started { transitions, .. } => {
                    let inherited = transitions.inherited_properties();
                    if inherited.is_empty() {
                        continue;
                    }
                    let (descendants, covers_subtree) = state
                        .engine_ref()
                        .inheriting_descendants(transitions.node, &inherited, INHERITING_DESCENDANTS_LIMIT)
                        .ok_or(Park(
                            "more descendants inherit a transition than the render owner restyles",
                        ))?;
                    if descendants == transitions.inheriting && covers_subtree == transitions.inheriting_covers_subtree
                    {
                        continue;
                    }
                    let mut found = HoverTransitions::clone(transitions);
                    found.inheriting = descendants;
                    found.inheriting_covers_subtree = covers_subtree;
                    if let ElementEffects::Started { transitions, .. } = &mut self.effects[index] {
                        *transitions = Arc::new(found);
                    }
                }
            }
        }
        Ok(())
    }

    /// Has the boxes `rows`, whose samples animated the properties beside each, and the anonymous boxes below them, show
    /// the styles the host installed in them again, with the visual contexts that moves. Answers whether any did.
    fn show_host_styles_in(&mut self, state: &mut RenderState, mut rows: SmallVec<[(NodeSlotId, &[u16]); 4]>) -> bool {
        let arena = state.arena.arena();
        // The anonymous boxes below them showed what they inherited of their samples.
        for index in 0..rows.len() {
            let (row, properties) = rows[index];
            if !row.is_invalid() {
                rows.extend(
                    arena
                        .anonymous_descendants(row)
                        .into_iter()
                        .map(|anonymous| (anonymous, properties)),
                );
            }
        }
        let mut moved = false;
        for (row, properties) in rows {
            if let Some(position) = self.ticked.iter().position(|(ticked, _)| *ticked == row) {
                let (row, host_style) = self.ticked.remove(position);
                let shown = state.arena.arena().node_style_record(row);
                let damage = state
                    .engine_mut()
                    .sampled_properties_damage(shown, host_style.record(), properties);
                let arena = state.arena.arena();
                arena.restore_host_style(row, host_style);
                arena.note_style_visual_context_moves(row, damage);
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

/// The kind of compositor animation that drives `property`, where one does.
fn compositor_animation_kind(property: u16) -> Option<FfiVisualAnimationTargetKind> {
    use crate::css::property_metadata::property_id as prop;
    match property {
        prop::OPACITY => Some(FfiVisualAnimationTargetKind::Opacity),
        prop::BACKGROUND_COLOR => Some(FfiVisualAnimationTargetKind::BackgroundColor),
        prop::FILTER => Some(FfiVisualAnimationTargetKind::Filter),
        prop::TRANSFORM | prop::TRANSLATE | prop::ROTATE | prop::SCALE => Some(FfiVisualAnimationTargetKind::Transform),
        _ => None,
    }
}

/// The kinds of the compositor animations the host published for the transitions the step of `transitions` ended, none
/// where a transition the step leaves running drives a compositor animation of one of those kinds, which the lane
/// cannot tell apart from them.
pub(super) fn ended_compositor_animation_kinds(
    transitions: &HoverTransitions,
) -> Option<SmallVec<[FfiVisualAnimationTargetKind; 2]>> {
    let ended: SmallVec<[FfiVisualAnimationTargetKind; 2]> = transitions
        .ended_host_properties
        .iter()
        .filter_map(|&property| compositor_animation_kind(property))
        .collect();
    let kept_alike = !ended.is_empty()
        && transitions
            .kept_host_properties()
            .filter_map(compositor_animation_kind)
            .any(|kind| ended.contains(&kind));
    (!kept_alike).then_some(ended)
}

/// Stops the compositor animations of the box of the element `transitions` restyle that drive what the host's
/// transitions the step ended animated, which the host published for them: the frames of the lane show the step's
/// style in their place.
pub(super) fn stop_ended_compositor_animations(arena: &LayoutNodeArena, transitions: &HoverTransitions) {
    let Some(ended) = ended_compositor_animation_kinds(transitions).filter(|ended| !ended.is_empty()) else {
        return;
    };
    stop_compositor_animations(arena, arena.bound_row(transitions.node), &ended);
}

/// Stops the compositor animations of the box of the element of `row`, a hover's row, that drive what the row moves: the
/// frames of the lane show the record it installs in their place.
pub(super) fn stop_compositor_animations_a_row_moves(
    engine: &StyleEngine,
    arena: &LayoutNodeArena,
    row: &FfiStyleDelta,
) {
    use crate::css::property_metadata::property_id as prop;
    let Some(node) = StyleNodeID::from_raw(row.style_node) else {
        return;
    };
    if row.new_style_record == 0 || row.new_style_record == row.old_style_record {
        return;
    }
    let mut kinds: SmallVec<[FfiVisualAnimationTargetKind; 2]> = SmallVec::new();
    for property in [
        prop::OPACITY,
        prop::BACKGROUND_COLOR,
        prop::FILTER,
        prop::TRANSFORM,
        prop::TRANSLATE,
        prop::ROTATE,
        prop::SCALE,
    ] {
        if let Some(kind) = compositor_animation_kind(property)
            && !kinds.contains(&kind)
            && crate::css::transition::records_compute_differently(
                engine,
                row.old_style_record,
                row.new_style_record,
                property,
            )
        {
            kinds.push(kind);
        }
    }
    stop_compositor_animations(arena, arena.bound_row(node), &kinds);
}

/// Stops the compositor animations of the box `row` of the `kinds`, which the host published: the frames of the lane
/// show what the lane gave the box in their place, which an animation the compositor runs on, or holds at its end
/// until the host publishes again, would hide.
fn stop_compositor_animations(arena: &LayoutNodeArena, row: NodeSlotId, kinds: &[FfiVisualAnimationTargetKind]) {
    if row.is_invalid() || kinds.is_empty() {
        return;
    }
    let nodes = arena.box_animation_nodes(row);
    let mut paint_state = arena.paint_state().borrow_mut();
    let Some(tree) = paint_state.visual_context.tree.as_mut() else {
        return;
    };
    let stops = |animation: &VisualAnimation| kinds.contains(&animation.target_kind) && nodes.driven_by(animation);
    if tree.visual_animations().iter().any(stops) {
        let running = tree
            .visual_animations()
            .iter()
            .filter(|animation| !stops(animation))
            .cloned()
            .collect();
        std::sync::Arc::make_mut(tree).set_visual_animations(running);
    }
}
