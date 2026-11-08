/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The records a pass computes are the ones the host renders, each installed in a slot of its own
//! and handed to the host as rows.

use super::super::pointer_key::{PointerKey, PointerLookup};
use super::super::*;
use super::install_scratch::InstallScratch;
use super::live::Live;
use super::main_effects::InstalledElementEffects;
use super::records::Record;
use std::sync::Arc;

/// Which style of an element a swap installs: the element's own, or that of its synthetic pseudo-element of
/// a kind.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(transparent)]
pub struct PseudoSlot(u8);

impl PseudoSlot {
    pub(crate) const ELEMENT: Self = Self(u8::MAX);

    pub(crate) const fn pseudo_element(kind: u8) -> Self {
        Self(kind)
    }
}

/// A style the main thread swaps in for an element or one of its pseudo-elements: the record the render side installed,
/// or none where the pseudo-element no longer generates a box. The render side holds the record until the host took
/// the swap, and the host counts a reference to the record as it stores it, so taking the swap is storing a pointer:
/// the host asks nothing of the engine to read the record.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct StyleSwap {
    style_node: u32,
    slot: PseudoSlot,
    highlight_name: u32,
    record: *const std::ffi::c_void,
}

// SAFETY: The pointer only borrows the immutable record the swap installs, which the render side holds until the host
// took the swap.
unsafe impl Send for StyleSwap {}
// SAFETY: As above; nothing writes through the pointer.
unsafe impl Sync for StyleSwap {}

impl StyleSwap {
    fn new(
        state: &RetainedState,
        node: StyleNodeID,
        slot: PseudoSlot,
        highlight_name: StyleAtomID,
        style_record: computed::FinalStyleRecordID,
    ) -> Self {
        let record = match style_record.raw() {
            0 => std::ptr::null(),
            raw => Arc::as_ptr(
                state
                    .computed_group_sets
                    .host_record(raw)
                    .expect("an installed record is held for the host"),
            )
            .cast(),
        };
        Self {
            style_node: node.raw(),
            slot,
            highlight_name: highlight_name.0,
            record,
        }
    }

    fn style_record(&self) -> u64 {
        // SAFETY: The render side holds the record while the swap lives.
        unsafe { self.record.cast::<Record>().as_ref() }.map_or(0, bridge::host_record_identity)
    }
}

/// What the main thread does to take a style installation: the styles it swaps in, and the side effects of the
/// elements whose styles move, both in tree order, each element's pseudo-elements following it. An element's effects
/// name its swaps, which the main thread performs before them, and before it swaps in the styles of the elements after
/// it, as an element's effects can clear the styles of its descendants. Once the render side installed the records,
/// only the elements the main thread has anything to do for keep their effects. `document_damage` is what all the
/// moves damage, merged.
#[derive(Default)]
pub(crate) struct StyleInstallation {
    pub(crate) swaps: Vec<StyleSwap>,
    pub(crate) effects: Vec<InstalledElementEffects>,
    pub(crate) document_damage: u32,
    /// The records the elements and their pseudo-elements move away from, which their moves and effects read, pinned
    /// until the host ends the transaction.
    pub(crate) pinned_records: Vec<u64>,
    /// The elements taking their first style whose parent, its layout tree parent as well, takes its first style in
    /// this installation too, in the order of their effects, each with the index of those and that parent.
    first_styles_below_first_styles: Vec<(u32, StyleNodeID)>,
}

impl StyleInstallation {
    /// The effects of `node`, where its style moves.
    #[cfg(test)]
    pub(crate) fn effects_of(&self, node: StyleNodeID) -> Option<&InstalledElementEffects> {
        self.effects.iter().find(|effects| effects.node() == node)
    }

    /// Takes what the main thread does, leaving the records the installation pinned, which stay pinned until the host
    /// ends the transaction.
    pub(crate) fn take_for_the_host(&mut self) -> Self {
        let pinned_records = std::mem::take(&mut self.pinned_records);
        std::mem::replace(
            self,
            Self {
                pinned_records,
                ..Default::default()
            },
        )
    }

    /// Notes in `state` what the host holds once it took the installation, as it swaps in the styles and clears those
    /// of hidden descendants without telling the render side: in the order the host takes them, each element's swaps,
    /// then the styles its effects clear, which the swaps of its descendants after them install again.
    pub(crate) fn note_held_by_the_host(&self, state: &mut RetainedState) {
        let note_swaps = |state: &mut RetainedState, swaps: &[StyleSwap]| {
            for swap in swaps {
                let node = StyleNodeID::from_raw(swap.style_node).expect("a swap names an element");
                match swap.slot {
                    // SAFETY: The render side holds the record while the swap lives.
                    PseudoSlot::ELEMENT => {
                        state.set_held_style_record(node, unsafe { bridge::retain_host_record(swap.record) });
                    }
                    PseudoSlot(kind) => {
                        let target = super::PseudoElementTarget {
                            kind,
                            highlight_name: StyleAtomID(swap.highlight_name),
                        };
                        state.set_held_pseudo_style_record(node, target, swap.style_record());
                    }
                }
            }
        };
        let mut next_swap = 0;
        for effects in &self.effects {
            let swaps_end = effects.swaps_end as usize;
            note_swaps(state, &self.swaps[next_swap..swaps_end.max(next_swap)]);
            next_swap = next_swap.max(swaps_end);
            if effects
                .effects
                .intersects(bridge::MainEffects::CLEAR_HIDDEN_DESCENDANTS)
            {
                state.clear_hidden_descendants(effects.node());
            }
        }
        note_swaps(state, &self.swaps[next_swap..]);
    }

    /// Notes the effects of the element whose styles the installation noted last, from the swap `first_swap` on.
    fn note_element_effects(&mut self, mut effects: InstalledElementEffects, first_swap: usize) {
        let index = |swap: usize| u32::try_from(swap).expect("an installation's swaps fit their index");
        effects.swaps_begin = index(first_swap);
        effects.swaps_end = index(self.swaps.len());
        self.effects.push(effects);
    }

    /// Leaves the boxes of every element to the host, for an installation the render side styles no box for.
    pub(crate) fn leave_boxes_to_the_host(&mut self) {
        for effects in &mut self.effects {
            effects.effects |= bridge::MainEffects::HOST_BOX_FACTS;
        }
        self.keep_effects_with_host_work();
    }

    /// Notes that the element whose effects the installation noted last takes its first style below `parent`, which
    /// takes its first style too.
    fn note_first_style_below_first_style(&mut self, parent: StyleNodeID) {
        let index = u32::try_from(self.effects.len() - 1).expect("an installation's effects fit their index");
        self.first_styles_below_first_styles.push((index, parent));
    }

    /// Takes the elements `note_first_style_below_first_style` noted.
    pub(crate) fn take_first_styles_below_first_styles(&mut self) -> Vec<(u32, StyleNodeID)> {
        std::mem::take(&mut self.first_styles_below_first_styles)
    }

    /// Keeps the effects of only the elements the main thread has anything to do for beside swapping in their styles.
    pub(crate) fn keep_effects_with_host_work(&mut self) {
        self.effects.retain(InstalledElementEffects::has_host_work);
    }

    /// Ends the element `node`, whose styles the installation noted from the swap `first_swap` on. The elements whose
    /// styles the main thread swaps in each have effects, which follow their swaps. An element can have effects with no
    /// style to swap in, as where it owes the host the plans of its animations.
    fn end_element(&self, node: StyleNodeID, first_swap: usize) {
        debug_assert!(
            self.swaps.len() == first_swap || self.effects.last().is_some_and(|effects| effects.node() == node),
            "every element with a style to swap in has effects"
        );
    }
}

impl RetainedState {
    /// The identity `record` is installed under: the one it names, the one another element took
    /// for a copy of it in this installation, kept in `installed`, or a new one.
    #[expect(
        clippy::mutable_key_type,
        reason = "a key hashes and compares only the address it holds"
    )]
    pub(super) fn install_record(
        &mut self,
        record: &Arc<Record>,
        installed: &mut HashMap<PointerKey<Arc<Record>>, computed::FinalStyleRecordID>,
    ) -> computed::FinalStyleRecordID {
        let records = &mut self.computed_group_sets;
        if let Some(id) = records.installed_record_named_by(record) {
            return id;
        }
        if let Some(&id) = installed.get(&PointerLookup::new(record, ())) {
            return id;
        }
        let id = records.install_record(record, || record.dependency_flags_for_a_slot());
        // NB: A record a fork's slot names took a copy in this slot, which it finds again here.
        if records.installed_record_named_by(record).is_none() {
            installed.insert(PointerKey::new(record, ()), id);
        }
        id
    }

    /// What moving each of `elements` from the record assigned it to the one `live` holds for it damages, on the threads
    /// of the pool where many move. Installing an element reads only its own records, which none of this changes.
    fn element_move_damages(&self, live: &Live, elements: &[StyleNodeID]) -> Vec<u32> {
        const ELEMENTS_PER_TASK: usize = 64;
        let moves = |&(index, new): &(usize, &Arc<Record>)| {
            let node = elements[index];
            let old = self.computed_group_sets.assigned_style_record(node);
            old.map_or(0, |old| self.element_record_damage_to(node, old.raw(), new))
        };
        // NB: An element's first style moves from nothing, which is every element of a page arriving.
        let moving: Vec<(usize, &Arc<Record>)> = elements
            .iter()
            .enumerate()
            .filter(|&(_, &node)| self.computed_group_sets.assigned_style_record(node).is_some())
            .filter_map(|(index, &node)| Some((index, live.demanded_record(node)?)))
            .collect();
        let pool = self.style_layout_pool.as_ref().and_then(|pool| pool.started());
        let answers: Vec<u32> = match pool.as_deref() {
            Some(pool) if moving.len() > ELEMENTS_PER_TASK => {
                use rayon::prelude::*;
                pool.install(|| {
                    moving
                        .par_chunks(ELEMENTS_PER_TASK)
                        .flat_map_iter(|chunk| chunk.iter().map(moves))
                        .collect()
                })
            }
            _ => moving.iter().map(moves).collect(),
        };
        let mut damages = vec![0; elements.len()];
        for (&(index, _), answer) in moving.iter().zip(answers) {
            damages[index] = answer;
        }
        damages
    }

    /// The rows installing the records `live` holds for the elements of `changed`, in tree order,
    /// each element's pseudo-elements following it, noting in `installation` what the main thread
    /// does to take them. The elements of `plans_owed`, sorted, owe the host the plans of the
    /// animations they and their pseudo-elements name, however their records moved, and those of
    /// `compositions_owed`, sorted, the composition of their animations. The installation works
    /// in `scratch`, which it leaves for the caller to recycle.
    #[allow(clippy::too_many_arguments)]
    pub(super) fn installation_rows(
        &mut self,
        root: StyleNodeID,
        live: &Live,
        changed: impl IntoIterator<Item = StyleNodeID>,
        plans_owed: &[StyleNodeID],
        compositions_owed: &[StyleNodeID],
        scratch: &mut InstallScratch,
        installation: &mut StyleInstallation,
    ) -> Vec<PublishedStyleDeltaRecord> {
        scratch.start(&self.projection);
        scratch.order(&self.projection, root, changed);
        let ordered = std::mem::take(&mut scratch.ordered);
        let element_damages = self.element_move_damages(live, &ordered);
        let mut rows = Vec::with_capacity(ordered.len());
        for (index, (&node, &element_damage)) in ordered.iter().zip(&element_damages).enumerate() {
            if let Some(ahead) = ordered
                .get(index + super::records::RECORDS_AHEAD)
                .and_then(|&ahead| live.demanded_record(ahead))
            {
                ahead.prefetch();
            }
            let Some(record) = live.demanded_record(node) else {
                continue;
            };
            let first_swap = installation.swaps.len();
            let records = &self.computed_group_sets;
            let old_record = records
                .assigned_style_record(node)
                .unwrap_or(computed::FinalStyleRecordID::NONE);
            let old = old_record.raw();
            // The record of the parent an element inherits from is installed before its own.
            let parent = self.projection.inheritance_parent(node);
            let parent_environment = parent
                .and_then(|parent| records.assigned_style_record(parent))
                .and_then(|record| records.style_record_custom_property_environment(record.raw()))
                .unwrap_or(0);
            let parent_record = parent
                .and_then(|parent| live.demanded_record(parent))
                .map(|record| &**record);
            let environment =
                self.record_environment(node, record, parent_record, parent_environment, &mut scratch.minted);
            let computed_record = record;
            let record: &Arc<Record> = &self.record_in_environment(record, environment);
            let pseudo_element_records = live.demanded_pseudo_element_records(node);
            assert_eq!(
                record.pseudo_element_styles(),
                super::records::PseudoElementStyles::of(pseudo_element_records),
                "a record styles the pseudo-elements its element holds records of"
            );
            // The host clears the styles of the descendants of a display:none element, so an element
            // below one may hold no style on the main thread, as may one below an element that left
            // display:none in an earlier transaction of the style update.
            let below_display_none = !self.held_style_records.contains(node)
                || parent.is_some_and(|parent| self.hidden_before(parent, scratch));
            scratch.note_hidden_before(node, below_display_none || self.record_is_display_none(old));
            let new_record = self.install_record(record, &mut scratch.installed);
            // NB: Where the element installs a copy of the record, as in another custom property environment, the element
            //     keeps what it installed, and its own goes: the composition of its animations lies over what it holds.
            if let Some(installed) = self.computed_group_sets.installed_record(new_record)
                && !Arc::ptr_eq(installed, computed_record)
            {
                scratch
                    .adopted
                    .push((node, Arc::as_ptr(computed_record) as usize, installed.clone()));
            }
            // NB: Installing the record lets go of the element's animation overlay, which the host
            //     composes again over it. Where the overlay lies over this very record, the pass
            //     computed the element's children and pseudo-elements from the overlay, not from
            //     the record: what the host's composition moves is told from the overlay.
            if self.computed_group_sets.composition_over_record(node, record).is_some()
                && let Some(held) = self.hold_composition(node)
            {
                scratch.released_compositions.push((node, held));
            }
            // The element's move and its effects read the record it moves from, which the assignment below lets go
            // of: an animation overlay no one else holds, as below an element that was display:none, would go with
            // it. It stays pinned until the host ends the transaction. A record the element keeps stays assigned.
            if old != 0 && old != new_record.raw() {
                self.computed_group_sets.pin_style_record(old);
                installation.pinned_records.push(old);
            }
            // The installation is final: the main thread installs what the render side did, and nothing takes
            // it back.
            self.assign_element_record(node, record, new_record);
            // What the container conditions of the rules it and its pseudo-elements matched read
            // of the containers above it is the host's to record, which restyles it when they move.
            if let Some(reads) = live.demanded_container_reads(node) {
                self.note_container_effects_for_host(node, reads);
            }
            let new = new_record.raw();
            // The main thread swaps in the record where it holds another one, or none, as below an element that was
            // display:none.
            let main_holds_new = self.held_style_record(node) == Some(new);
            if !main_holds_new {
                installation.swaps.push(StyleSwap::new(
                    self,
                    node,
                    PseudoSlot::ELEMENT,
                    StyleAtomID::NONE,
                    new_record,
                ));
            }
            let record_damage = match old != 0 && old != new {
                true => element_damage | bridge::FfiStyleInvalidationField::EngineComputed as u32,
                false => 0,
            };
            let plan_owed = plans_owed.binary_search(&node).is_ok();
            let composition_owed = compositions_owed.binary_search(&node).is_ok();
            let owes_an_animation_plan = (plan_owed && self.record_declares_animations(new))
                || self.row_owes_an_animation_plan(node, old, new, record);
            let owes_a_transition_step = self.row_owes_a_transition_step(node, old_record, new_record);
            // The host composes the record where the element has animations, or the row owes their plan
            // or the transition step, before it settles the element's pseudo-elements. It composes it too
            // where it composes over the record the element holds: the element is not yet associated
            // with a transition the stabilization epoch started until the epoch commits, and installing
            // the record lets go of the composition.
            let composed_by_the_host = owes_an_animation_plan
                || owes_a_transition_step
                || old_record.is_animation_overlay()
                || self.projection.adjustment_facts(node) & bridge::element_adjustment_fact::HAS_ANIMATIONS != 0;
            let mut pseudo_elements = super::main_effects::PseudoElementFacts::default();
            rows.push(PublishedStyleDeltaRecord {
                style_node: node.raw(),
                old_style_record: old,
                new_style_record: new,
                reaction: input_types::STYLE_REACTION_PUBLISHED_STYLE | input_types::STYLE_REACTION_RECOMPUTE_STYLE,
                inherited_style_groups: 0,
                pseudo_kind: u8::MAX,
                explicitly_inherited_groups: record.rare().reads.parent_reset_groups,
                record_damage,
                owes_an_animation_plan,
                owes_a_transition_step,
                composed_by_the_host,
            });
            // The pseudo-elements follow their element, the ones that no longer generate a box
            // with no record.
            let main_holds_pseudo = |state: &Self, target: super::PseudoElementTarget| {
                state
                    .held_pseudo_style_records
                    .get(&node)
                    .and_then(|held| held.iter().find(|&&(held_target, _)| held_target == target))
                    .map_or(0, |&(_, record)| record)
            };
            for kind in super::PSEUDO_ELEMENT_KINDS {
                let old_pseudo_record = self
                    .computed_group_sets
                    .pseudo_style_record(node, kind)
                    .unwrap_or(computed::FinalStyleRecordID::NONE);
                let old_pseudo = old_pseudo_record.raw();
                let new_pseudo_record = match pseudo_element_records
                    .iter()
                    .find(|(held, _)| *held == super::PseudoElementTarget::of_kind(kind))
                {
                    Some((_, pseudo_record)) => {
                        // A pseudo-element's containers are its originating element's.
                        self.note_container_unit_reads(node, pseudo_record, true);
                        let pseudo_environment = self.pseudo_element_environment(
                            node,
                            pseudo_record,
                            record,
                            environment,
                            &mut scratch.minted,
                        );
                        let pseudo_record: &Arc<Record> =
                            &self.record_in_environment(pseudo_record, pseudo_environment);
                        let new_pseudo_record = self.install_record(pseudo_record, &mut scratch.installed);
                        // The pseudo-element's move reads the record it moves from, which the assignment lets go
                        // of, as the element's does.
                        if old_pseudo != 0 && old_pseudo != new_pseudo_record.raw() {
                            self.computed_group_sets.pin_style_record(old_pseudo);
                            installation.pinned_records.push(old_pseudo);
                        }
                        self.computed_group_sets
                            .assign_installed_pseudo_record(node, kind, new_pseudo_record);
                        new_pseudo_record
                    }
                    // The main thread may hold a record a read demanded for a pseudo-element that generates no
                    // box, which goes as the element's styles move.
                    None if old_pseudo == 0
                        && main_holds_pseudo(self, super::PseudoElementTarget::of_kind(kind)) == 0 =>
                    {
                        continue;
                    }
                    None => computed::FinalStyleRecordID::NONE,
                };
                let new_pseudo = new_pseudo_record.raw();
                let main_holds_new_pseudo =
                    main_holds_pseudo(self, super::PseudoElementTarget::of_kind(kind)) == new_pseudo;
                if !main_holds_new_pseudo {
                    installation.swaps.push(StyleSwap::new(
                        self,
                        node,
                        PseudoSlot::pseudo_element(kind),
                        StyleAtomID::NONE,
                        new_pseudo_record,
                    ));
                }
                if old_pseudo == new_pseudo {
                    // A pseudo-element whose record stays plans the animations it names again where
                    // their `@keyframes` may have moved, as for a first record.
                    if plan_owed && main_holds_new_pseudo && new_pseudo != 0 {
                        self.note_pseudo_element_composition(
                            &mut pseudo_elements,
                            node,
                            kind,
                            computed::FinalStyleRecordID::NONE,
                            new_pseudo_record,
                        );
                    }
                    // The main thread cleared the style of a pseudo-element whose record stays, as below an element
                    // that was display:none: it takes the record again, with what it installs beside it, and the
                    // animations it names start as for a first record.
                    if !main_holds_new_pseudo {
                        self.note_pseudo_element_reinstall(&mut pseudo_elements, new_pseudo);
                        if new_pseudo != 0 {
                            self.note_pseudo_element_composition(
                                &mut pseudo_elements,
                                node,
                                kind,
                                computed::FinalStyleRecordID::NONE,
                                new_pseudo_record,
                            );
                        }
                    }
                    continue;
                }
                let record_damage = self.pseudo_element_record_damage(node, kind, old_pseudo, new_pseudo, new, false)
                    | bridge::FfiStyleInvalidationField::EngineComputed as u32;
                // Below an element that was display:none, the main thread holds no record the pseudo-element moves
                // from.
                let composition = match new_pseudo {
                    0 => Default::default(),
                    _ => self.note_pseudo_element_composition(
                        &mut pseudo_elements,
                        node,
                        kind,
                        match below_display_none {
                            true => computed::FinalStyleRecordID::NONE,
                            false => old_pseudo_record,
                        },
                        new_pseudo_record,
                    ),
                };
                self.note_pseudo_element_move(
                    &mut pseudo_elements,
                    node,
                    kind,
                    (old_pseudo, new_pseudo, new),
                    record_damage,
                    below_display_none,
                );
                // A pseudo-element that no longer generates a box goes now; the main thread holds the record it
                // had until it clears the pseudo-element's style.
                if new_pseudo_record == computed::FinalStyleRecordID::NONE {
                    self.remove_computed_pseudo(node, kind);
                }
                rows.push(PublishedStyleDeltaRecord {
                    style_node: node.raw(),
                    old_style_record: old_pseudo,
                    new_style_record: new_pseudo,
                    reaction: input_types::STYLE_REACTION_PUBLISHED_STYLE,
                    inherited_style_groups: 0,
                    pseudo_kind: kind,
                    explicitly_inherited_groups: 0,
                    record_damage,
                    owes_an_animation_plan: composition.owes_an_animation_plan,
                    owes_a_transition_step: composition.owes_a_transition_step,
                    composed_by_the_host: composition.composed_by_the_host,
                });
            }
            let custom_highlights = self
                .computed_group_sets
                .custom_highlight_records(node)
                .map(|(name, _)| super::PseudoElementTarget::custom_highlight(name))
                .chain(
                    pseudo_element_records
                        .iter()
                        .map(|&(target, _)| target)
                        .filter(|target| target.is_custom_highlight()),
                )
                .fold(
                    SmallVec::<[super::PseudoElementTarget; 2]>::new(),
                    |mut targets, target| {
                        if !targets.contains(&target) {
                            targets.push(target);
                        }
                        targets
                    },
                );
            for target in custom_highlights {
                let old_pseudo_record = self
                    .computed_group_sets
                    .pseudo_style_record_of(node, target)
                    .unwrap_or(computed::FinalStyleRecordID::NONE);
                let old_pseudo = old_pseudo_record.raw();
                let new_pseudo_record = match pseudo_element_records.iter().find(|(held, _)| *held == target) {
                    Some((_, pseudo_record)) => {
                        self.note_container_unit_reads(node, pseudo_record, true);
                        let pseudo_environment = self.pseudo_element_environment(
                            node,
                            pseudo_record,
                            record,
                            environment,
                            &mut scratch.minted,
                        );
                        let pseudo_record: &Arc<Record> =
                            &self.record_in_environment(pseudo_record, pseudo_environment);
                        let new_pseudo_record = self.install_record(pseudo_record, &mut scratch.installed);
                        if old_pseudo != 0 && old_pseudo != new_pseudo_record.raw() {
                            self.computed_group_sets.pin_style_record(old_pseudo);
                            installation.pinned_records.push(old_pseudo);
                        }
                        self.computed_group_sets
                            .assign_installed_pseudo_record_of(node, target, new_pseudo_record);
                        new_pseudo_record
                    }
                    None if old_pseudo == 0 && main_holds_pseudo(self, target) == 0 => continue,
                    None => computed::FinalStyleRecordID::NONE,
                };
                let new_pseudo = new_pseudo_record.raw();
                if main_holds_pseudo(self, target) != new_pseudo {
                    installation.swaps.push(StyleSwap::new(
                        self,
                        node,
                        PseudoSlot::pseudo_element(target.kind),
                        target.highlight_name,
                        new_pseudo_record,
                    ));
                }
                if old_pseudo == new_pseudo {
                    continue;
                }
                let record_damage =
                    self.pseudo_element_record_damage(node, target.kind, old_pseudo, new_pseudo, new, false)
                        | bridge::FfiStyleInvalidationField::EngineComputed as u32;
                self.note_pseudo_element_move(
                    &mut pseudo_elements,
                    node,
                    target.kind,
                    (old_pseudo, new_pseudo, new),
                    record_damage,
                    below_display_none,
                );
                if new_pseudo_record == computed::FinalStyleRecordID::NONE {
                    self.remove_computed_pseudo_of(node, target);
                }
            }
            if old != new
                || !main_holds_new
                || plan_owed
                || composition_owed
                || pseudo_elements.moved
                || self.container_effects_for_host.get(&node).is_some()
            {
                let sets = &self.computed_group_sets;
                let old_view = sets.style_record_view(old).filter(|_| !below_display_none);
                let new_view = sets.style_record_view(new).expect("an installed record is live");
                let facts = super::main_effects::InstallationFacts {
                    composed_by_the_host,
                    owes_an_animation_plan,
                    owes_a_transition_step,
                    inherits_sampled_custom_properties: parent
                        .is_some_and(|parent| self.element_samples_custom_properties(parent)),
                    old_environment: old_view.and_then(|_| sets.style_record_custom_property_environment(old)),
                    new_environment: environment,
                    // NB: A record the main thread already holds moves nothing, wherever the element stands.
                    element_damage: (old_view.is_some() || (old == new && main_holds_new)).then_some(record_damage),
                    old_record: old_view.and(computed::FinalStyleRecordID::from_raw(old)),
                    pseudo_elements,
                };
                let first_style = facts.element_damage.is_none();
                let effects = self.main_effects(node, old_view, new_view, facts);
                scratch.document_damage.push(effects.damage());
                installation.note_element_effects(effects, first_swap);
                if first_style {
                    scratch.note_first_style(node);
                    if let Some(parent) = parent.filter(|&parent| {
                        scratch.takes_first_style(parent) && self.projection.parent(node) == Some(parent)
                    }) {
                        installation.note_first_style_below_first_style(parent);
                    }
                }
            }
            installation.end_element(node, first_swap);
        }
        scratch.ordered = ordered;
        installation.document_damage =
            super::super::style_invalidation::merged_damage(Some(0), scratch.document_damage.iter().copied());
        rows
    }

    /// Whether a row moving `node` from the record the host holds, `old`, to the installed `new`
    /// one owes the host the transition step: the record beneath any composition moves, the host
    /// holds a style to start transitions from, and the record it moves from declares transitions
    /// or the move changes their declarations.
    fn row_owes_a_transition_step(
        &self,
        node: StyleNodeID,
        old: computed::FinalStyleRecordID,
        new: computed::FinalStyleRecordID,
    ) -> bool {
        if old == computed::FinalStyleRecordID::NONE {
            return false;
        }
        let Some(underlying) = self.computed_group_sets.underlying_style_record(old) else {
            return false;
        };
        if underlying == new {
            return false;
        }
        let moves_transition_declarations = self.transition_declarations_moved(underlying.raw(), new.raw());
        self.decide_transition_step(node, underlying, moves_transition_declarations)
    }

    /// Assign `node` the record `new_record` installs `record` as, which the main thread installs.
    pub(super) fn assign_element_record(
        &mut self,
        node: StyleNodeID,
        record: &Record,
        new_record: computed::FinalStyleRecordID,
    ) {
        self.computed_group_sets.assign_installed_record(node, new_record);
        // The containers the element's descendants ask about are the ones its new record describes.
        self.set_element_container_query_inputs(node, new_record.raw());
        // What its container-relative lengths read of the containers above it is the host's to
        // record, which restyles it when they resize.
        self.note_container_unit_reads(node, record, false);
        if self.projection.adjustment_facts(node) & bridge::element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0 {
            self.prepare_root_font_metrics_from_record(new_record);
        }
    }

    /// Keep for the host what the container-relative lengths of a record of `node`'s, or of one of
    /// its pseudo-elements, read of `node`'s query containers, which their installed records
    /// describe now. A pseudo-element's query containers include `node` itself.
    pub(super) fn note_container_unit_reads(&mut self, node: StyleNodeID, record: &Record, pseudo: bool) {
        if record.rare().reads.container_units == 0 {
            return;
        }
        let values = crate::css::computed_value_views::ComputedValuesView::of(&record.payloads);
        let inline_axis_is_horizontal = values.writing_mode() == crate::css::css_enums::writing_mode::HORIZONTAL_TB;
        self.note_container_unit_reads_for_host(
            node,
            record.rare().reads.container_units,
            inline_axis_is_horizontal,
            pseudo,
        );
    }

    /// Whether `node`, or an ancestor of it, was display:none before this installation, noting the
    /// answer for each element asked in `scratch`, which holds it for the elements this
    /// installation installed already.
    fn hidden_before(&self, node: StyleNodeID, scratch: &mut InstallScratch) -> bool {
        // The elements up to the nearest one with an answer, or the root, answered from the top down.
        let mut chain = std::mem::take(&mut scratch.chain);
        chain.clear();
        let mut ancestor = Some(node);
        let mut hidden = false;
        while let Some(current) = ancestor {
            if let Some(answer) = scratch.hidden_before(current) {
                hidden = answer;
                break;
            }
            chain.push(current);
            ancestor = self.projection.inheritance_parent(current);
        }
        for &current in chain.iter().rev() {
            // An element this installation has not reached yet holds the record it held before.
            let record = self
                .computed_group_sets
                .assigned_style_record(current)
                .map_or(0, computed::FinalStyleRecordID::raw);
            hidden = hidden || self.record_is_display_none(record);
            scratch.note_hidden_before(current, hidden);
        }
        scratch.chain = chain;
        hidden
    }

    /// Whether the host clears the styles of `node`'s descendants as `node` moves from a record whose display was none
    /// or not, `old_is_none`, to one whose display is none or not, `new_is_none`, the move changing the style groups of
    /// `inherited_groups_changed`, an `FfiStyleInvalidationField` inherited groups mask, and moving the element's
    /// custom-property environment or not. The descendants of an element entering display:none hold no styles, so
    /// they cannot become transition before-change styles. Those of an element that stays display:none have no boxes,
    /// so their styles only serve a CSSOM read or the subtree's reveal, both of which materialize them again: where
    /// what they inherit moves, they are cleared rather than computed again, unless an SVG element content can
    /// reference keeps its style among them.
    pub(crate) fn clears_hidden_descendants(
        &self,
        node: StyleNodeID,
        old_is_none: Option<bool>,
        new_is_none: bool,
        inherited_groups_changed: u32,
        environment_moved: bool,
    ) -> bool {
        match old_is_none {
            None => false,
            Some(false) => new_is_none,
            Some(true) => {
                new_is_none
                    && inherited_groups_changed != 0
                    && !environment_moved
                    && !self.has_referenceable_svg_descendant(node)
            }
        }
    }

    /// Whether a descendant of `node` in the flat tree that holds a style is an SVG element content can reference,
    /// which keeps its style while hidden. An element without a style holds none below it.
    fn has_referenceable_svg_descendant(&self, node: StyleNodeID) -> bool {
        let mut found = false;
        self.for_each_held_flat_tree_descendant(node, |state, element| {
            found = found || state.is_referenceable_in_display_none_subtree(element);
            !found
        });
        found
    }

    /// Notes that the host cleared the styles of `node`'s descendants that hold one, in the flat tree, as it does for
    /// the descendants of a hidden element.
    pub(crate) fn clear_hidden_descendants(&mut self, node: StyleNodeID) {
        let mut cleared = Vec::new();
        self.for_each_held_flat_tree_descendant(node, |_, element| {
            cleared.push(element);
            true
        });
        for element in cleared {
            self.set_held_style_record(element, None);
        }
    }

    /// Calls `callback` with each descendant of `node` in the flat tree that holds a style, in no particular order,
    /// skipping the descendants of those that hold none, until it answers false. As in the walk with which the host
    /// clears the styles, a slottable is below the slot it is assigned to, and not reached from its host.
    fn for_each_held_flat_tree_descendant(
        &self,
        node: StyleNodeID,
        mut callback: impl FnMut(&Self, StyleNodeID) -> bool,
    ) {
        let projection = &self.projection;
        let mut pending: smallvec::SmallVec<[StyleNodeID; 32]> = smallvec::SmallVec::new();
        let push_children = |pending: &mut smallvec::SmallVec<[StyleNodeID; 32]>, parent: StyleNodeID| {
            pending.extend(projection.shadow_root_of(parent));
            let mut child = projection.first_element_child(parent);
            while let Some(current) = child {
                if projection.assigned_slot_of(current).is_none() {
                    pending.push(current);
                }
                child = projection.next_element_sibling(current);
            }
            pending.extend(
                projection
                    .assigned_nodes_of(parent)
                    .iter()
                    .copied()
                    .filter(|assigned| assigned.text_index().is_none()),
            );
        };
        push_children(&mut pending, node);
        while let Some(current) = pending.pop() {
            // A shadow root holds no style, but its children do.
            if projection.host_of(current).is_some() {
                push_children(&mut pending, current);
                continue;
            }
            if !self.held_style_records.contains(current) {
                continue;
            }
            if !callback(self, current) {
                return;
            }
            push_children(&mut pending, current);
        }
    }

    fn record_is_display_none(&self, record: u64) -> bool {
        self.computed_group_sets
            .style_record_payloads(record)
            .is_some_and(|payloads| {
                crate::css::computed_value_views::ComputedValuesView::of(payloads)
                    .display()
                    .is_none()
            })
    }

    /// The custom property environment of the record the engine computed for `node`, whose parent's record
    /// names `parent_environment`. A store the parent hands down is the environment it inherits; any
    /// other one is the element's previous environment where it holds the same values, and a new
    /// one otherwise, which the elements of `minted` sharing the store share.
    /// `record`, where the environment `environment` names is the one it holds the store of, or else a record holding
    /// what it does in that environment: a record a pass kept meeting the environment its element moved to since.
    pub(super) fn record_in_environment<'a>(
        &self,
        record: &'a Arc<Record>,
        environment: u64,
    ) -> std::borrow::Cow<'a, Arc<Record>> {
        if record.custom_property_environment() == environment {
            return std::borrow::Cow::Borrowed(record);
        }
        std::borrow::Cow::Owned(Arc::new(record.in_custom_property_environment(
            self.custom_property_environments.store(environment).cloned(),
        )))
    }

    pub(super) fn record_environment(
        &mut self,
        node: StyleNodeID,
        record: &Record,
        parent: Option<&Record>,
        parent_environment: u64,
        minted: &mut super::install_scratch::MintedEnvironments,
    ) -> u64 {
        // The element's own custom properties resolve over what its parent hands down, as the host's do.
        let inherited_environment = self.custom_property_environments.inheritable(parent_environment);
        let Some(store) = record.custom_properties() else {
            return inherited_environment;
        };
        let handed_down = parent.and_then(|parent| parent.inheritable_custom_properties());
        if handed_down.is_some_and(|handed_down| Arc::ptr_eq(handed_down, store)) {
            return inherited_environment;
        }
        if let Some(&identity) = minted.get(&PointerLookup::new(store, inherited_environment)) {
            return identity;
        }
        let registry = self.document_style_computation_inputs.custom_property_registry();
        let identity = match self.environment_resolving_to_the_same_values(node, None, store, registry) {
            Some(previous) => previous,
            None => self.custom_property_environments.adopt_engine_environment(
                store.clone(),
                record.inheritable_custom_properties(),
                inherited_environment,
                handed_down.map(|store| &**store),
                registry,
            ),
        };
        minted.insert(PointerKey::new(store, inherited_environment), identity);
        identity
    }

    /// The custom property environment of a pseudo-element's record the engine computed, over the
    /// environment of its originating element's `element` record.
    pub(super) fn pseudo_element_environment(
        &mut self,
        node: StyleNodeID,
        record: &Record,
        element: &Record,
        element_environment: u64,
        minted: &mut super::install_scratch::MintedEnvironments,
    ) -> u64 {
        // A pseudo-element's own custom properties resolve over what its element hands down, as the host's do.
        let inherited_environment = self.custom_property_environments.inheritable(element_environment);
        let Some(store) = record.custom_properties() else {
            return inherited_environment;
        };
        // NB: A pseudo-element declaring no custom properties holds what the element's computed record hands down,
        //     which is not the store of `element` where that is a copy in another environment. Naming the store over
        //     this environment would take it from the element above that holds it.
        if record
            .custom_declarations()
            .is_none_or(|declarations| declarations.is_empty())
            || element
                .inheritable_custom_properties()
                .as_ref()
                .is_some_and(|handed_down| Arc::ptr_eq(handed_down, store))
        {
            // NB: What the element's animations sampled over this environment is what it hands down.
            return match self.element_custom_property_data.get(&node) {
                Some(held) if held.sampled_over == Some(element_environment) => {
                    self.custom_property_environments.inheritable(held.identity)
                }
                _ => inherited_environment,
            };
        }
        if let Some(&identity) = minted.get(&PointerLookup::new(store, inherited_environment)) {
            return identity;
        }
        let registry = self.document_style_computation_inputs.custom_property_registry();
        // NB: A pseudo-element's record says nothing of what the store hands down, as it has no children to hand it to,
        //     and an element holding the same store shares this environment, so what its children inherit is the
        //     registrations' to say.
        let identity = self.custom_property_environments.adopt_engine_environment(
            store.clone(),
            None,
            inherited_environment,
            element.inheritable_custom_properties().map(|store| &**store),
            registry,
        );
        minted.insert(PointerKey::new(store, inherited_environment), identity);
        identity
    }
}

#[cfg(test)]
mod tests {
    use super::super::super::TreeScopeID;
    use super::super::super::computed;
    use super::super::super::custom_property_environments::environment_identity;
    use super::super::test_support::*;
    use std::collections::HashMap;

    #[test]
    fn a_store_holding_what_the_record_of_an_element_holds_resolves_to_its_environment() {
        let (mut engine, nodes) = nested_document();
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(&mut engine, sheet, ".theme { --theme: 1px; }");
        add_class(&mut engine, nodes[0], "theme");
        start_live(&mut engine, nodes[0]);

        let record = live_record(&engine, nodes[0]);
        let store = record.custom_properties().unwrap().clone();
        let state = &mut engine.retained;
        let registry = state.document_style_computation_inputs.custom_property_registry();
        let environment =
            state
                .custom_property_environments
                .adopt_engine_environment(store.clone(), None, 0, None, registry);
        let installed = state.computed_group_sets.install_derived_record(&record, 0, || {
            computed::record_dependency_flags(&record.facts(), &record.payloads)
        });
        state.computed_group_sets.assign_installed_record(nodes[0], installed);

        // NB: The element holds no environment of its own, as nothing lies over its record.
        assert!(!state.element_custom_property_data.contains_key(&nodes[0]));
        assert_eq!(
            state.environment_resolving_to_the_same_values(nodes[0], None, &store, registry),
            Some(environment)
        );
    }

    #[test]
    fn a_pseudo_element_inheriting_a_store_leaves_the_environment_of_its_holder() {
        let (mut engine, nodes) = nested_document();
        let sheet = add_sheet(&mut engine, TreeScopeID::DOCUMENT);
        add_rules(
            &mut engine,
            sheet,
            ".theme { --theme: 1px; } .other { --other: 2px; } .pseudo::before { content: \"x\"; }",
        );
        add_class(&mut engine, nodes[0], "theme");
        add_class(&mut engine, nodes[2], "pseudo");
        add_class(&mut engine, nodes[3], "other");
        start_live(&mut engine, nodes[0]);

        let root_store = live_record(&engine, nodes[0]).custom_properties().unwrap().clone();
        let other_store = live_record(&engine, nodes[3]).custom_properties().unwrap().clone();
        let element = live_record(&engine, nodes[2]);
        assert!(std::sync::Arc::ptr_eq(
            element.custom_properties().unwrap(),
            &root_store
        ));
        let pseudo_record = {
            let records = engine.retained.live_style.as_ref().unwrap().records();
            records.pseudo_element_records(nodes[2])[0].1.clone()
        };

        // The document element's environment names the store over nothing, and the element holds a copy of its record
        // in another environment, as one does below an ancestor whose environment has not moved yet.
        let state = &mut engine.retained;
        let registry = state.document_style_computation_inputs.custom_property_registry();
        let root_environment =
            state
                .custom_property_environments
                .adopt_engine_environment(root_store.clone(), None, 0, None, registry);
        let other_environment =
            state
                .custom_property_environments
                .adopt_engine_environment(other_store, None, 0, None, registry);
        let copy = state.record_in_environment(&element, other_environment).into_owned();

        let environment = state.pseudo_element_environment(
            nodes[2],
            &pseudo_record,
            &copy,
            other_environment,
            &mut HashMap::default(),
        );

        assert_eq!(environment, other_environment);
        assert_eq!(root_environment, environment_identity(&root_store));
        assert_eq!(
            state
                .custom_property_environments
                .engine_environment(root_environment)
                .map(|(_, parent)| parent),
            Some(0)
        );
    }
}
