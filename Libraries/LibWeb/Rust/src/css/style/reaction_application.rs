/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Applying the reactions of a style transaction to the elements they name.
//!
//! The engine decides what each reaction installs: the record it settled, a record it demands now, or nothing, where
//! it refuses the reaction. The host installs a record on its element, runs the element's animations and transitions,
//! invalidates its layout, and reports what the installation did, from which the engine derives the reactions of the
//! element's children.

use super::StyleNodeID;
use super::boundary::StyleChange;
use super::bridge::{
    FfiEngineComputedRecord, FfiRecordDemand, FfiStyleDelta, FfiStyleDeltaDamage, FfiStyleDeltaGap,
    FfiStyleInvalidationField, PSEUDO_RECORD_SLOTS,
};
use super::engine_calls::{absorb_element_style_input, has_deferred_element_style_input, with_engine};
use super::publication::RecordDemand;
use super::transaction::{
    STYLE_REACTION_ANCESTOR_BECAME_VISIBLE, STYLE_REACTION_INHERITED_CUSTOM_PROPERTIES, STYLE_REACTION_INHERITED_STYLE,
    STYLE_REACTION_PUBLISHED_STYLE, STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES, STYLE_REACTION_RECOMPUTE_STYLE,
};
use crate::render_state::{ArenaChange, BegunRead, DocumentHost};

/// A `Web::DOM::Element`, which Rust names by pointer only.
#[repr(C)]
pub struct HostElement {
    _opaque: [u8; 0],
    _not_send_or_sync: std::marker::PhantomData<*const ()>,
}

/// The host's application of style reactions to its document's elements, which Rust names by pointer only.
#[repr(C)]
pub struct HostStyleReactionApplication {
    _opaque: [u8; 0],
    _not_send_or_sync: std::marker::PhantomData<*const ()>,
}

/// The host's element held in a struct, which C++ sees as an opaque pointer: the GC plugin rejects a raw cell pointer
/// field, and a `GC::Ptr` is no C type.
#[derive(Clone, Copy)]
#[repr(transparent)]
pub struct HostElementPtr(pub *mut HostElement);

impl HostElementPtr {
    fn is_null(self) -> bool {
        self.0.is_null()
    }
}

/// The element a style node names, as the host holds it while it applies a reaction to it.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiReactionElement {
    /// The host's element, or null where no element has the style node.
    pub element: HostElementPtr,
    pub style_node: u32,
    /// Whether the element is connected to the document whose reactions are applied.
    pub connected: bool,
    pub has_style: bool,
    pub style_record: u64,
    /// Whether the element's style reads its custom-property environment through `var()` or `inherit()`.
    pub reads_environment: bool,
    /// Whether the style the element holds has display:none.
    pub display_none: bool,
}

/// How a targeted style update of an element reads its style.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiStyleUpdateMode {
    /// Every element the element inherits through computes the style it owes, under display:none ancestors too.
    Normal,
    /// Only an element without style, or one that owes a style input, computes its style.
    #[expect(
        dead_code,
        reason = "only the host asks for this mode, which the engine reads by elimination"
    )]
    OnlyIfNeeded,
    /// As `OnlyIfNeeded`, but a display:none ancestor stops the update: the element has no style to read.
    StopAtDisplayNone,
}

/// What a targeted style update answers a read of an element's style.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiTargetedStyleAnswer {
    /// The element has no style to read.
    Unstyled,
    /// The element has the style it reads.
    Styled,
    /// The element has the style it holds, if any.
    HeldStyle,
    /// The style the element reads needs a style update of its document first.
    NeedsStyleUpdate,
}

/// What a targeted update of an element in an inheritance chain did, for the elements below it.
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct FfiTargetedStyleReaction {
    /// The update moved what the element's descendants inherit or how their boxes are built.
    pub descendants_need_recompute: bool,
    /// The update asks for the element's descendants to compute their styles again.
    pub recompute_descendant_styles: bool,
}

/// What moving a pseudo-element from one record to another damages, where the engine answered it.
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct FfiPseudoRecordDamage {
    pub old_style_record: u64,
    pub new_style_record: u64,
    /// An `FfiStyleInvalidationField` word with `EngineComputed` set, or zero where the engine answered nothing.
    pub damage: u32,
}

/// A record the host installs on an element, as one the engine computed, with the pseudo-element records beside it.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiRecordInstallation {
    /// The element's row, holding the record and what it says about the installation.
    pub row: FfiStyleDelta,
    /// Whether the host acknowledges the record to the engine once it holds it.
    pub acknowledge: bool,
    /// What moving the element from the row's old record to its new one damages, where the engine answered it: an
    /// `FfiStyleInvalidationField` word with `EngineComputed` set, or zero.
    pub damage: u32,
    /// The synthetic pseudo-element kinds whose records install beside the element's, as a bit per kind; a present
    /// slot holding zero is a removal.
    pub pseudo_records_present: u16,
    pub pseudo_records: [u64; PSEUDO_RECORD_SLOTS],
    pub pseudo_damages: [FfiPseudoRecordDamage; PSEUDO_RECORD_SLOTS],
}

/// What the reaction passes of a style update did, for the host's counters.
#[derive(Clone, Copy, Default)]
#[repr(C)]
pub struct FfiStyleReactionCounts {
    /// Whether the update had reactions to apply at all.
    pub had_reactions: bool,
    pub batch_runs: u32,
    pub reaction_elements: u32,
    pub published_reactions: u32,
    pub materialized_gaps: u32,
    pub record_deltas_applied: u32,
    pub pass_guard_hits: u32,
    pub apply_microseconds: u64,
}

/// A style transaction the host took, whose reactions live until the host takes the next one.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiTakenStyleTransaction {
    pub reactions: *const FfiStyleDelta,
    pub count: usize,
    pub scoped: bool,
    /// The transaction planned nothing but the child reactions the engine derived from the reactions the host applied
    /// last: one more generation of the same style change, not a new one.
    pub only_derived_child_reactions: bool,
    /// The elements connected to the document as the transaction was taken.
    pub connected_element_count: u32,
}

unsafe extern "C" {
    fn web_css_republish_moved_environment(
        application: *mut HostStyleReactionApplication,
        node: u32,
        style_record: u64,
        replaced: u64,
        new_inheritable: *const std::ffi::c_void,
    );
    fn web_css_rebuild_custom_property_environment(
        application: *mut HostStyleReactionApplication,
        node: u32,
        new_inheritable: *const std::ffi::c_void,
    );
    fn web_css_style_inheritance_parent(
        application: *mut HostStyleReactionApplication,
        element: *mut HostElement,
    ) -> FfiReactionElement;
    fn web_css_apply_targeted_style_reaction(
        application: *mut HostStyleReactionApplication,
        element: *mut HostElement,
        installation: *const FfiRecordInstallation,
        descendant_style_recompute_needed: bool,
    ) -> FfiTargetedStyleReaction;
    fn web_css_take_style_transaction(
        application: *mut HostStyleReactionApplication,
        flown: bool,
    ) -> FfiTakenStyleTransaction;
    fn web_css_has_pending_style_transaction(application: *mut HostStyleReactionApplication) -> bool;
    fn web_css_note_style_update_has_reactions(application: *mut HostStyleReactionApplication);
    fn web_css_sample_animations_for_style_update(application: *mut HostStyleReactionApplication);
    fn web_css_begin_style_update_pass(application: *mut HostStyleReactionApplication, first: bool);
    fn web_css_style_reaction_element(application: *mut HostStyleReactionApplication, node: u32) -> FfiReactionElement;
    fn web_css_parent_style_has_animated_values(element: *mut HostElement) -> bool;
    fn web_css_engine_record_environment_is_installable(
        application: *mut HostStyleReactionApplication,
        element: *mut HostElement,
        style_record: u64,
    ) -> bool;
    fn web_css_record_derived_element_style_input(
        application: *mut HostStyleReactionApplication,
        node: u32,
        reaction: u8,
        groups: u8,
    );
    fn web_css_apply_style_reaction(
        application: *mut HostStyleReactionApplication,
        element: *mut HostElement,
        reaction: u8,
        installation: *const FfiRecordInstallation,
    );
}

/// The host's application of a batch of reactions to its document's elements, which the host names by pointer.
#[derive(Clone, Copy)]
struct Application<'a> {
    host: &'a DocumentHost,
    read: &'a BegunRead,
    host_application: *mut HostStyleReactionApplication,
}

impl Application<'_> {
    fn element(self, node: u32) -> FfiReactionElement {
        // SAFETY: The host lends its application for the call that applies the batch.
        unsafe { web_css_style_reaction_element(self.host_application, node) }
    }

    fn record_environment_is_installable(self, element: &FfiReactionElement, style_record: u64) -> bool {
        // SAFETY: As above, and the element is live while its reaction is applied.
        unsafe {
            web_css_engine_record_environment_is_installable(self.host_application, element.element.0, style_record)
        }
    }

    fn apply(self, element: &FfiReactionElement, reaction: u8, installation: Option<&FfiRecordInstallation>) {
        let installation = installation.map_or(std::ptr::null(), std::ptr::from_ref);
        // SAFETY: As above.
        unsafe { web_css_apply_style_reaction(self.host_application, element.element.0, reaction, installation) };
    }

    /// The engine's answer to a targeted demand for the element's record, where the host can install it. The engine
    /// resolved the record's environment over the parent's own; where the parent's inheritable environment differs,
    /// it takes back what the demand derived.
    fn answer_record_demand(self, element: &FfiReactionElement, node: StyleNodeID) -> Option<FfiEngineComputedRecord> {
        let answer = with_engine(self.read, self.host, |engine| {
            super::bridge::answer_record_demand(engine, node, RecordDemand::Element(FfiRecordDemand::TargetedElement))
        })
        .record;
        if answer.style_record == 0 {
            return None;
        }
        if !self.record_environment_is_installable(element, answer.style_record) {
            self.host
                .queue_change(ArenaChange::Style(StyleChange::AbandonDemandedRecords {
                    node: Some(node),
                }));
            return None;
        }
        Some(answer)
    }

    /// The engine declined the row. Nothing else computes styles: the element keeps the record it holds. The engine
    /// answers no demand below an ancestor that owes a style input, such as one whose custom-property animations
    /// published new values as an earlier row of the batch installed its record. That ancestor's style moves in the
    /// next transaction, so the row is owed to it too, which plans the row after the ancestor's.
    fn refuse(self, node: StyleNodeID, reaction: u8, inherited_style_groups: u8) {
        let ancestor_owes_style_input = with_engine(self.read, self.host, |engine| {
            std::iter::successors(engine.retained.tree.inheritance_parent(node), |&ancestor| {
                engine.retained.tree.inheritance_parent(ancestor)
            })
            .any(|ancestor| engine.has_deferred_element_style_input(ancestor))
        });
        if ancestor_owes_style_input {
            // SAFETY: The host lends its application for the call that applies the batch.
            unsafe {
                web_css_record_derived_element_style_input(
                    self.host_application,
                    node.raw(),
                    reaction,
                    inherited_style_groups,
                );
            }
            return;
        }
        self.host.queue_change(crate::render_state::ArenaChange::Style(
            super::boundary::StyleChange::ConsumeElementStyleInput { node: Some(node) },
        ));
    }

    /// A row the engine did not settle in its transaction, or a targeted update: the record the engine demands for
    /// the element now, which installs as one the engine computed, moving the element from the record it holds. A
    /// refused demand refuses the row.
    fn demand(
        self,
        element: &FfiReactionElement,
        node: StyleNodeID,
        reaction: u8,
        inherited_style_groups: u8,
    ) -> Option<FfiRecordInstallation> {
        let Some(answer) = self.answer_record_demand(element, node) else {
            self.refuse(node, reaction, inherited_style_groups);
            return None;
        };
        let mut row = unsettled_row(node.raw(), 0);
        row.old_style_record = element.style_record;
        let mut installation = FfiRecordInstallation::new(row, true);
        installation.settle(&answer);
        Some(installation)
    }
}

/// A row the engine left for the host to answer from the element's record demand.
fn unsettled_row(style_node: u32, reaction: u8) -> FfiStyleDelta {
    FfiStyleDelta {
        style_node,
        match_answer: 0,
        old_style_record: 0,
        new_style_record: 0,
        damage: FfiStyleDeltaDamage::None,
        reaction,
        inherited_style_groups: 0,
        pseudo_kind: u8::MAX,
        gap: FfiStyleDeltaGap::Materialize,
        uses_substitution: false,
        record_reads: 0,
        explicitly_inherited_groups: 0,
        record_damage: 0,
        owes_an_animation_plan: false,
        owes_a_transition_step: false,
        composed_by_the_host: false,
    }
}

impl FfiStyleDelta {
    /// Leaves the row to the host to answer from the element's record demand.
    fn unsettle(&mut self) {
        self.gap = FfiStyleDeltaGap::Materialize;
        self.new_style_record = 0;
        self.damage = FfiStyleDeltaDamage::None;
    }

    /// What moving to the row's record damages, where the engine answered it.
    fn answered_damage(&self) -> u32 {
        if self.record_damage & FfiStyleInvalidationField::EngineComputed as u32 != 0 {
            self.record_damage
        } else {
            0
        }
    }
}

impl FfiRecordInstallation {
    fn new(row: FfiStyleDelta, acknowledge: bool) -> Self {
        Self {
            damage: row.answered_damage(),
            row,
            acknowledge,
            pseudo_records_present: 0,
            pseudo_records: [0; PSEUDO_RECORD_SLOTS],
            pseudo_damages: [FfiPseudoRecordDamage::default(); PSEUDO_RECORD_SLOTS],
        }
    }

    /// Installs the record the engine settled for the row by a demand, with the pseudo-element records it settled
    /// beside it.
    fn settle(&mut self, record: &FfiEngineComputedRecord) {
        let row = &mut self.row;
        row.new_style_record = record.style_record;
        row.uses_substitution = record.uses_substitution;
        row.record_reads = record.record_reads;
        row.explicitly_inherited_groups = record.explicitly_inherited_groups;
        row.owes_an_animation_plan = record.owes_an_animation_plan;
        row.owes_a_transition_step = record.owes_a_transition_step;
        row.composed_by_the_host = record.composed_by_the_host;
        row.damage = FfiStyleDeltaDamage::Full;
        row.gap = FfiStyleDeltaGap::Computed;
        row.record_damage = 0;
        self.damage = 0;
        self.pseudo_records_present = record.pseudo_records_present;
        self.pseudo_records = record.pseudo_records;
    }

    /// Installs the record of a pseudo-element row the batch published beside the element's.
    fn add_pseudo_element_row(&mut self, row: &FfiStyleDelta) {
        let kind = usize::from(row.pseudo_kind);
        self.pseudo_records_present |= 1 << kind;
        self.pseudo_records[kind] = row.new_style_record;
        self.pseudo_damages[kind] = FfiPseudoRecordDamage {
            old_style_record: row.old_style_record,
            new_style_record: row.new_style_record,
            damage: row.answered_damage(),
        };
    }
}

/// Applies `reactions` in preorder, so every element's inheritance inputs are ready when it is applied. What an
/// applied element's change means for its (flat-tree) children is the engine's to derive: it reads each application
/// and plans the children as the next transaction of this style update.
fn apply_style_reactions(
    application: Application<'_>,
    reactions: &[FfiStyleDelta],
    counts: &mut FfiStyleReactionCounts,
) {
    let Application { host, read, .. } = application;
    let _noting = host.engine_memo().note_declaration_changes_during_apply();
    for (index, published) in reactions.iter().enumerate() {
        // A pseudo-element record installs with its element's, which leads it. An element the engine answered as
        // hidden needs no style until a read or its subtree's reveal asks for one.
        if published.pseudo_kind != u8::MAX || published.gap == FfiStyleDeltaGap::Hidden {
            continue;
        }
        let element = application.element(published.style_node);
        if element.element.is_null() {
            continue;
        }
        let node = StyleNodeID::from_raw(published.style_node).expect("a style reaction names an element");
        let mut row = *published;

        // A host that rewrote the element's declarations while an earlier row was applied (a form control restyling
        // its shadow tree as its own style moves) leaves the record the engine computed from the old ones: the row is
        // answered from its demand, over the declarations as they are now.
        if row.gap == FfiStyleDeltaGap::Computed && host.engine_memo().declarations_changed_during_apply(node) {
            row.unsettle();
            row.reaction |= STYLE_REACTION_RECOMPUTE_STYLE;
        }

        // NB: An earlier row's environment move can republish the record an element holds over the moved environment.
        //     A swap of its inherited groups planned over the record it held before would undo the move: the row is
        //     answered from its demand, over the record the element holds now.
        if row.gap == FfiStyleDeltaGap::None && element.has_style && row.old_style_record != element.style_record {
            row.unsettle();
        }

        // A reaction the engine derived for this element while applying an earlier one in this batch joins the
        // element's own reaction where it covers it, which a demand for the element's record always does.
        let absorbed = absorb_element_style_input(
            host,
            read,
            node,
            row.reaction,
            row.inherited_style_groups,
            row.gap == FfiStyleDeltaGap::Materialize,
        );
        if absorbed != 0 {
            row.reaction = absorbed as u8;
            row.inherited_style_groups = (absorbed >> 8) as u8;
        }

        // An engine-computed record installs on an element without style as a first record does, its style cleared on
        // entry to display:none included; other record deltas assume the style they move.
        // NB: An inheritance scheduling row carries no computation of its own. If an earlier display:none reaction
        //     cleared its style and no derived input reached it, leave it unstyled until a read or reveal.
        if !element.has_style
            && (row.gap == FfiStyleDeltaGap::None || (row.gap == FfiStyleDeltaGap::Materialize && row.reaction == 0))
        {
            continue;
        }

        let needs_regular_style_recompute = row.reaction
            & (STYLE_REACTION_PUBLISHED_STYLE
                | STYLE_REACTION_RECOMPUTE_STYLE
                | STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES
                | STYLE_REACTION_ANCESTOR_BECAME_VISIBLE)
            != 0;
        let needs_custom_property_recompute = row.reaction & STYLE_REACTION_INHERITED_CUSTOM_PROPERTIES != 0;
        let needs_inherited_style_recompute = row.reaction & STYLE_REACTION_INHERITED_STYLE != 0;
        // An element declaring custom properties of its own layers them over the environment it inherits, which its
        // cascade decides.
        let computes_style = needs_regular_style_recompute
            || needs_inherited_style_recompute
            || (needs_custom_property_recompute
                && (element.reads_environment
                    || with_engine(read, host, |engine| engine.node_declares_custom_properties(node))));

        // A row the engine did not settle in its transaction, and that computes the element's style, is answered from
        // its record demand, as a targeted update of the element is: the record installs as one the engine computed,
        // moving the element from the record it holds.
        let mut installation = None;
        if row.gap == FfiStyleDeltaGap::Materialize
            && computes_style
            && let Some(answer) = application.answer_record_demand(&element, node)
        {
            row.old_style_record = element.style_record;
            let mut demanded = FfiRecordInstallation::new(row, true);
            demanded.settle(&answer);
            row = demanded.row;
            installation = Some(demanded);
        }

        if row.reaction & STYLE_REACTION_PUBLISHED_STYLE != 0 {
            counts.published_reactions += 1;
        }
        if row.gap == FfiStyleDeltaGap::Materialize {
            if row.reaction & STYLE_REACTION_PUBLISHED_STYLE != 0 {
                counts.materialized_gaps += 1;
            }
            assert!(row.new_style_record == 0 && row.damage == FfiStyleDeltaDamage::None);
        } else {
            counts.record_deltas_applied += 1;
            // An engine-computed record is the engine's current answer for the element, which may have skipped a
            // delta the host never installed; it is applied against whatever the element holds now.
            assert!(row.gap == FfiStyleDeltaGap::Computed || row.old_style_record == element.style_record);
            assert!(row.new_style_record != 0 && row.damage == FfiStyleDeltaDamage::Full);
        }

        let installation = match row.gap {
            FfiStyleDeltaGap::None => {
                assert!(!needs_regular_style_recompute && needs_inherited_style_recompute);
                assert!(!needs_custom_property_recompute);
                // The engine swapped the element's inherited groups for its parent's: the record installs as an
                // engine record. The engine refuses the swap to an element that animates, declares transitions, or
                // inherits from an animating parent. A parent this batch installed may have taken animated values
                // from its own ancestors since the engine swapped, which the element inherits in place of the base
                // ones: the row is answered from its demand.
                // SAFETY: The element is live while its reaction is applied.
                if unsafe { web_css_parent_style_has_animated_values(element.element.0) } {
                    application.demand(&element, node, row.reaction, row.inherited_style_groups)
                } else {
                    Some(FfiRecordInstallation::new(row, false))
                }
            }
            FfiStyleDeltaGap::Computed => {
                // The engine computed the new record from this element's moved cascade winners, from its parent's
                // moved inherited style or display, or from its moved inherited custom-property environment.
                assert!(
                    needs_regular_style_recompute || needs_inherited_style_recompute || needs_custom_property_recompute
                );
                // A demand settled the pseudo-elements beside the element's record; the rows the batch published
                // beside the element's moved from a record the demand replaced.
                let installation = installation.unwrap_or_else(|| {
                    let mut installation = FfiRecordInstallation::new(row, true);
                    reactions[index + 1..]
                        .iter()
                        .take_while(|next| next.style_node == row.style_node && next.pseudo_kind != u8::MAX)
                        .for_each(|pseudo_row| installation.add_pseudo_element_row(pseudo_row));
                    installation
                });
                if application.record_environment_is_installable(&element, row.new_style_record) {
                    Some(installation)
                } else {
                    // The engine resolved the record's environment over the parent's own, which an earlier row of the
                    // batch moved: the row is answered from a fresh demand.
                    application.demand(&element, node, row.reaction, row.inherited_style_groups)
                }
            }
            // The engine declined the row's demand above.
            _ if computes_style => {
                application.refuse(node, row.reaction, row.inherited_style_groups);
                None
            }
            // A row that owes only a moved inherited environment on an element whose style reads none has nothing
            // left for the host: the engine moved the element's environment, and the record over it, when its
            // parent's moved.
            _ => None,
        };
        application.apply(&element, row.reaction, installation.as_ref());
    }
}

/// A style update applies at most this many passes of reactions that publish new style, beyond which it stops: a
/// style change that keeps feeding back into itself would never settle.
const MAX_STYLE_UPDATE_PASSES: u32 = 8;

impl Application<'_> {
    /// Takes the next style transaction of the update, the one that flew first where the update drains it, and
    /// answers the reactions naming an element into `reactions`, and whether the batch is dense enough to match
    /// broadly.
    fn take_transaction(self, flown: bool, reactions: &mut Vec<FfiStyleDelta>) -> (bool, bool) {
        // SAFETY: The host lends its application for the call that updates the style.
        let taken = unsafe { web_css_take_style_transaction(self.host_application, flown) };
        // SAFETY: The host's transaction lives until it takes the next one.
        let taken_reactions = unsafe { super::bridge::borrow(taken.reactions, taken.count) };
        reactions.clear();
        for reaction in taken_reactions {
            // The complete answer remains in the engine's transaction scratch under this node, which the reaction
            // names. An element removed beside the transaction that flew has no style to install: its removal is the
            // next transaction's.
            if self.element(reaction.style_node).element.is_null() {
                let node = StyleNodeID::from_raw(reaction.style_node).expect("a style reaction names an element");
                assert!(
                    self.host
                        .engine_memo()
                        .beside_flown_transaction
                        .borrow()
                        .contains(&node)
                );
                continue;
            }
            reactions.push(*reaction);
        }
        // A reaction batch covering more than one sixteenth of the connected elements is dense enough that packing
        // the scope once is cheaper than repeatedly reconstructing cold facts while matching the planned elements.
        let prefers_broad_matching_batch =
            !taken.scoped || reactions.len() * 16 > taken.connected_element_count as usize;
        (prefers_broad_matching_batch, taken.only_derived_child_reactions)
    }

    fn has_pending_transaction(self) -> bool {
        // SAFETY: As above.
        unsafe { web_css_has_pending_style_transaction(self.host_application) }
    }
}

/// Applies the reactions of the style transactions of one style update, from the first, which flew where the update
/// drains it, until the reactions they feed back settle. Each pass applies one transaction's reactions, closed over
/// the elements they inherit through, and the consequences produced while applying them become the next transaction.
fn update_style(
    application: Application<'_>,
    drains_flown_transaction: bool,
    root: Option<StyleNodeID>,
) -> FfiStyleReactionCounts {
    let Application { host, read, .. } = application;
    let mut counts = FfiStyleReactionCounts::default();
    let mut reactions = Vec::new();
    let (mut prefers_broad_matching_batch, mut only_derived_child_reactions) =
        application.take_transaction(drains_flown_transaction, &mut reactions);
    // SAFETY: The host lends its application for the call that updates the style.
    unsafe {
        if !reactions.is_empty() {
            web_css_note_style_update_has_reactions(application.host_application);
        }
        web_css_sample_animations_for_style_update(application.host_application);
    }
    if reactions.is_empty() && application.has_pending_transaction() {
        (prefers_broad_matching_batch, only_derived_child_reactions) =
            application.take_transaction(false, &mut reactions);
    }
    // SAFETY: As above.
    unsafe { web_css_begin_style_update_pass(application.host_application, true) };
    if reactions.is_empty() {
        return counts;
    }
    counts.had_reactions = true;

    let cold_matching_batch = root.map(|root| {
        host.queue_change(ArenaChange::Style(if prefers_broad_matching_batch {
            StyleChange::BeginColdMatchingBatch { root: Some(root) }
        } else {
            StyleChange::BeginAdaptiveColdMatchingBatch { root: Some(root) }
        }));
    });

    let mut closed = Vec::new();
    let mut batch = Vec::new();
    let mut style_update_passes = 0;
    let mut first_pass = true;
    while !reactions.is_empty() {
        let apply_started_at = std::time::Instant::now();
        // One more tree generation of the same style change is not a new pass of it.
        if !std::mem::take(&mut first_pass) && !only_derived_child_reactions {
            // SAFETY: As above.
            unsafe { web_css_begin_style_update_pass(application.host_application, false) };
        }
        let published_reactions = reactions
            .iter()
            .filter(|reaction| reaction.reaction & STYLE_REACTION_PUBLISHED_STYLE != 0)
            .count() as u32;
        if published_reactions > 0 && !only_derived_child_reactions {
            style_update_passes += 1;
            if style_update_passes > MAX_STYLE_UPDATE_PASSES {
                counts.pass_guard_hits += 1;
                counts.apply_microseconds += apply_started_at.elapsed().as_micros() as u64;
                break;
            }
        }

        // A reaction can name an element created by editing after its new inheritance parent was inserted, and an
        // element between a reaction and an ancestor that reacts too needs a row for the reactions the ancestor
        // derives to reach the reaction. The engine closes the batch over both, and orders it for application in
        // preorder: every parent is ready before its descendants, and a parent's derived reaction can merge into an
        // unconsumed child reaction in the same batch.
        let changed = {
            let beside_flown_transaction = host.engine_memo().beside_flown_transaction.borrow();
            let beside_flown_transaction = drains_flown_transaction.then_some(&*beside_flown_transaction);
            with_engine(read, host, |engine| {
                super::bridge::close_style_reactions_over_inheritance(
                    engine,
                    &reactions,
                    beside_flown_transaction,
                    &mut closed,
                )
            })
        };
        batch.clear();
        batch.extend(
            (if changed { &closed } else { &reactions })
                .iter()
                .filter(|reaction| application.element(reaction.style_node).connected),
        );
        reactions.clear();
        if !batch.is_empty() {
            if published_reactions > 0 {
                counts.batch_runs += 1;
                counts.reaction_elements += published_reactions;
            }
            apply_style_reactions(application, &batch, &mut counts);
        }
        counts.apply_microseconds += apply_started_at.elapsed().as_micros() as u64;

        // Exact consequences produced while recomputing become the next transaction in this stabilization epoch. Take
        // it only after consuming the current published answers, since a new transaction retires their scratch.
        if application.has_pending_transaction() {
            (_, only_derived_child_reactions) = application.take_transaction(false, &mut reactions);
        }
    }
    if cold_matching_batch.is_some() {
        host.queue_change(ArenaChange::Style(StyleChange::EndColdMatchingBatch));
    }
    counts
}

/// Applies the reactions of the style transactions of a style update of the host's document, from the one that flew
/// where `drains_flown_transaction`, through `host_application`, the host's state for applying them, and answers what
/// they did for its counters. `root` is the document element's style node, or zero.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `host_application` must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_update_style(
    host: &DocumentHost,
    read: &BegunRead,
    host_application: *mut HostStyleReactionApplication,
    drains_flown_transaction: bool,
    root: u32,
) -> FfiStyleReactionCounts {
    let application = Application {
        host,
        read,
        host_application,
    };
    update_style(application, drains_flown_transaction, StyleNodeID::from_raw(root))
}

/// What an inheritance chain holds, by depth from its first element.
#[derive(Default)]
struct InheritanceChainScan {
    /// The topmost element that has to compute its style: it has none, or it owes a style input.
    topmost_requiring_style: Option<usize>,
    topmost_display_none: Option<usize>,
    nearest_display_none: Option<usize>,
}

impl InheritanceChainScan {
    fn display_none_above_every_element_requiring_style(&self) -> bool {
        self.topmost_display_none.is_some_and(|display_none| {
            self.topmost_requiring_style
                .is_none_or(|requiring| display_none > requiring)
        })
    }
}

impl Application<'_> {
    /// The elements an element inherits its style through, from `first` up to the root.
    fn inheritance_chain(self, first: FfiReactionElement) -> impl Iterator<Item = FfiReactionElement> {
        let present = |element: FfiReactionElement| (!element.element.is_null()).then_some(element);
        std::iter::successors(present(first), move |element| {
            // SAFETY: The host lends its application for the call, and the element is live while it is read.
            present(unsafe { web_css_style_inheritance_parent(self.host_application, element.element.0) })
        })
    }

    /// Scans the chain an element inherits its style through, from `first`, where every element may owe a style
    /// input unless `may_owe_style_inputs` is false.
    fn scan_inheritance_chain(self, first: FfiReactionElement, may_owe_style_inputs: bool) -> InheritanceChainScan {
        let mut scan = InheritanceChainScan::default();
        for (depth, element) in self.inheritance_chain(first).enumerate() {
            if !element.has_style
                || (may_owe_style_inputs
                    && StyleNodeID::from_raw(element.style_node)
                        .is_some_and(|node| has_deferred_element_style_input(self.host, self.read, node)))
            {
                scan.topmost_requiring_style = Some(depth);
            }
            if element.display_none {
                scan.topmost_display_none = Some(depth);
                scan.nearest_display_none.get_or_insert(depth);
            }
        }
        scan
    }

    /// Answers a read of an element's style, whose chain of elements it inherits through starts at `first`, from the
    /// styles they hold, where neither its document nor an embedding one has style work pending.
    fn read_settled_inheritance_chain(
        self,
        first: FfiReactionElement,
        mode: FfiStyleUpdateMode,
    ) -> FfiTargetedStyleAnswer {
        let scan = self.scan_inheritance_chain(first, false);
        if mode == FfiStyleUpdateMode::StopAtDisplayNone && scan.display_none_above_every_element_requiring_style() {
            FfiTargetedStyleAnswer::Unstyled
        } else if scan.topmost_requiring_style.is_none() {
            FfiTargetedStyleAnswer::Styled
        } else {
            FfiTargetedStyleAnswer::NeedsStyleUpdate
        }
    }

    /// Updates the style of the chain of elements an element inherits through, from `first`: re-cascades from the
    /// rootmost element on the chain that has to compute its style back down to the element.
    fn update_style_for_inheritance_chain(
        self,
        first: FfiReactionElement,
        mode: FfiStyleUpdateMode,
        embedding_document_layout_was_stale: bool,
    ) -> FfiTargetedStyleAnswer {
        let scan = self.scan_inheritance_chain(first, true);
        if mode == FfiStyleUpdateMode::StopAtDisplayNone && scan.display_none_above_every_element_requiring_style() {
            return FfiTargetedStyleAnswer::Unstyled;
        }
        // Normal mode also re-cascades the target path under its display:none ancestors.
        let mut topmost_element_to_recompute = scan.topmost_requiring_style;
        if mode == FfiStyleUpdateMode::Normal && topmost_element_to_recompute.is_none() {
            topmost_element_to_recompute = scan.nearest_display_none.and_then(|depth| depth.checked_sub(1));
        }
        let topmost_element_to_recompute = match topmost_element_to_recompute {
            Some(depth) => depth,
            None if mode != FfiStyleUpdateMode::Normal && !embedding_document_layout_was_stale => {
                return FfiTargetedStyleAnswer::HeldStyle;
            }
            None => 0,
        };

        // The chain is named by style node: an element it names is looked up again as the walk reaches it.
        let chain: smallvec::SmallVec<[u32; 32]> = self
            .inheritance_chain(first)
            .take(topmost_element_to_recompute + 1)
            .map(|element| element.style_node)
            .collect();
        let mut descendant_style_recompute_needed = false;
        for &style_node in chain.iter().rev() {
            let element = self.element(style_node);
            let Some(node) = StyleNodeID::from_raw(style_node).filter(|_| !element.element.is_null()) else {
                return FfiTargetedStyleAnswer::HeldStyle;
            };
            let installation = self.demand(
                &element,
                node,
                STYLE_REACTION_PUBLISHED_STYLE | STYLE_REACTION_RECOMPUTE_STYLE,
                0,
            );
            let installation = installation.as_ref().map_or(std::ptr::null(), std::ptr::from_ref);
            // SAFETY: The host lends its application for the call, and the element is live while it is updated.
            let applied = unsafe {
                web_css_apply_targeted_style_reaction(
                    self.host_application,
                    element.element.0,
                    installation,
                    descendant_style_recompute_needed,
                )
            };
            descendant_style_recompute_needed |= applied.recompute_descendant_styles;

            // The engine can refuse a first style, which leaves the rest of the chain nothing to inherit from.
            let element = self.element(style_node);
            if !element.has_style {
                return FfiTargetedStyleAnswer::HeldStyle;
            }
            if element.display_none {
                if mode == FfiStyleUpdateMode::StopAtDisplayNone {
                    return FfiTargetedStyleAnswer::Unstyled;
                }
                descendant_style_recompute_needed = false;
            }
            descendant_style_recompute_needed |= applied.descendants_need_recompute;
        }
        FfiTargetedStyleAnswer::HeldStyle
    }
}

/// Answers a read of an element's style from the styles held by the chain of elements it inherits through, from
/// `first`, where neither its document nor an embedding one has style work pending, or that it needs a style update.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `host_application` and the element `first`
/// names must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_read_settled_inheritance_chain(
    host: &DocumentHost,
    read: &BegunRead,
    host_application: *mut HostStyleReactionApplication,
    first: FfiReactionElement,
    mode: FfiStyleUpdateMode,
) -> FfiTargetedStyleAnswer {
    let application = Application {
        host,
        read,
        host_application,
    };
    application.read_settled_inheritance_chain(first, mode)
}

/// Updates the style of the chain of elements an element inherits through, from `first`, for a read of the element's
/// style, and answers the read.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `host_application` and the element `first`
/// names must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_update_style_for_inheritance_chain(
    host: &DocumentHost,
    read: &BegunRead,
    host_application: *mut HostStyleReactionApplication,
    first: FfiReactionElement,
    mode: FfiStyleUpdateMode,
    embedding_document_layout_was_stale: bool,
) -> FfiTargetedStyleAnswer {
    let application = Application {
        host,
        read,
        host_application,
    };
    application.update_style_for_inheritance_chain(first, mode, embedding_document_layout_was_stale)
}

/// Moves the custom-property environments below `origin`, whose own moved as `moved` says, and acts on what the move
/// reached, in flat tree preorder, through `host_application`.
///
/// Every styled descendant holds the environment it inherits by identity, and the engine keeps what each holds: it
/// hands the moved environment to the descendants that hold the one the element handed down before, with their
/// records. What is left is the host's: installing those records, the environments of element-backed pseudo-elements,
/// which the engine does not keep, and the custom properties a descendant declares itself, built again over the moved
/// environment. A descendant whose style reads the environment computes again.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, `host_application` must be live for the call, the
/// stores `moved` names must be null or live raw `Arc` pointers, and `moved.new_inheritable_data` must be null or a
/// live `Web::CSS::CustomPropertyData`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn style_engine_move_custom_property_environment(
    host: &DocumentHost,
    read: &BegunRead,
    host_application: *mut HostStyleReactionApplication,
    origin: u32,
    moved: super::bridge::FfiEnvironmentMove,
) {
    use super::environment_move::{EnvironmentMove, EnvironmentMoveAction, NamedEnvironment};
    let Some(origin) = StyleNodeID::from_raw(origin) else {
        return;
    };
    let named = |environment: super::bridge::FfiNamedEnvironment| NamedEnvironment {
        identity: environment.identity,
        store: environment.store,
    };
    let environment_move = EnvironmentMove {
        old_inheritable: moved.old_inheritable,
        new_inheritable: named(moved.new_inheritable),
        new_inheritable_data: moved.new_inheritable_data,
        new_inheritable_declares: moved.new_inheritable_declares,
    };
    let (actions, moved_environments) = with_engine(read, host, |engine| {
        let mut actions = Vec::new();
        // SAFETY: Guaranteed by the caller.
        unsafe { engine.move_custom_property_environment(origin, &environment_move, |action| actions.push(action)) };
        // Each element the move left an environment takes the record republished over it.
        let mut moved_environments = Vec::new();
        for action in &actions {
            let EnvironmentMoveAction::Republish { node, .. } = *action else {
                continue;
            };
            moved_environments.push((
                node,
                None,
                engine.element_custom_property_data(node).expose_provenance(),
            ));
            moved_environments.extend(
                engine
                    .pseudo_element_custom_property_environments(node)
                    .map(|(pseudo, data)| (node, Some(pseudo), data.expose_provenance())),
            );
        }
        (actions, moved_environments)
    });
    host.engine_memo().held.borrow_mut().follow_moved(&moved_environments);

    for action in actions {
        // SAFETY: The host lends its application for the call, and the environment it moved to outlives it.
        unsafe {
            match action {
                EnvironmentMoveAction::Republish {
                    node,
                    style_record,
                    replaced,
                } => web_css_republish_moved_environment(
                    host_application,
                    node.raw(),
                    style_record,
                    replaced,
                    moved.new_inheritable_data,
                ),
                EnvironmentMoveAction::Rebuild(node) => {
                    web_css_rebuild_custom_property_environment(
                        host_application,
                        node.raw(),
                        moved.new_inheritable_data,
                    );
                }
                // An element that has to compute again is recorded with a recompute reaction alone: its descendants
                // are the move's, or that computation's, to reach. (The engine fans an inherited custom-properties
                // reaction out to every child of an applied reaction.)
                EnvironmentMoveAction::Recompute(node) => web_css_record_derived_element_style_input(
                    host_application,
                    node.raw(),
                    STYLE_REACTION_RECOMPUTE_STYLE,
                    0,
                ),
            }
        }
    }
}
