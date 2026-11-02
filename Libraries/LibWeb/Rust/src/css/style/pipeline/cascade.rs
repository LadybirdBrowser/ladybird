/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The cascade of one element: its matched rules and its own declaration blocks, ordered by cascade
//! priority, with the first declaration of each longhand in that order winning.

use super::super::animations::DeclarationScope;
use super::super::*;
use super::matching::ElementMatch;
use super::rules::RuleIndex;
use crate::css::computed_longhand_table::LONGHAND_COUNT;
use crate::css::property_metadata::property_id::ANIMATION_NAME;
use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID};
use crate::css::selector::Specificity;
use smallvec::SmallVec;

const LONGHAND_WORDS: usize = LONGHAND_COUNT.div_ceil(64);

/// The own declarations of an element no rule reaches and no style node holds the declarations
/// of, such as one outside the document, with the facts its style adjusts by. Its cascade and
/// computation take them in place of those of the node they are keyed by.
pub(in crate::css::style) struct DeclaredOnly {
    pub(in crate::css::style) declarations: super::super::element_declarations::ElementDeclarationRow,
    pub(in crate::css::style) facts: u32,
}

/// The declaration that won one longhand.
#[derive(Clone, Copy, Eq, Hash, PartialEq)]
pub(in crate::css::style) struct Winner {
    pub(super) property: u16,
    pub(super) important: bool,
    /// The block holding the declaration.
    pub(super) source: WinnerSource,
    /// The declaration's index in the block.
    pub(super) index: usize,
}

/// What the cascade of the element could not decide yet.
#[derive(Default)]
pub(super) struct CascadeCounters {
    pub(super) winners: u64,
    pub(super) pending_substitutions: u64,
}

impl CascadeCounters {
    pub(super) fn add(&mut self, other: &Self) {
        self.winners += other.winners;
        self.pending_substitutions += other.pending_substitutions;
    }
}

/// How many blocks an element's cascade sorts into cascade order and walks, taking the first
/// declaration of each longhand. Beyond that many, it offers them in any order instead, keeping
/// the best declaration of each longhand, which saves sorting them all.
const ORDERED_BLOCKS: usize = 16;

/// The declarations of one block, with its custom property declarations and their written values.
type BlockDeclarations<'a> = (
    &'a [DeclarationEntry],
    Option<(&'a [CustomDeclaration], &'a [RetainedStyleValueData])>,
);

/// The best declaration of one longhand so far, in a cascade offered blocks in any order.
#[derive(Clone, Copy)]
struct Candidate {
    priority: CascadePriority,
    important: bool,
    source: WinnerSource,
    index: usize,
    /// The rule index entry the declaration's block is of, or `u32::MAX` for an element's own.
    entry: u32,
    /// The tree scope the block's rule decides from, as its match says.
    scope: TreeScopeID,
}

/// A declaration of `revert` or `revert-layer`, by the priority of its block.
type Reverting = (CascadePriority, CascadeOperator);

/// Whether one of the declarations `reverted` rolls back past a declaration at `priority`.
fn rolled_back(reverted: &[Reverting], priority: &CascadePriority) -> bool {
    reverted
        .iter()
        .any(|(reverting, operator)| reverting.reverts_past(*operator, priority))
}

/// Whether a declaration of `operator` rolls the cascade back.
fn is_revert(operator: CascadeOperator) -> bool {
    matches!(operator, CascadeOperator::Revert | CascadeOperator::RevertLayer)
}

/// The best declaration of one custom property so far, in a cascade offered blocks in any order.
struct CustomCandidate {
    priority: CascadePriority,
    declaration: CustomDeclaration,
    written: RetainedStyleValueData,
}

/// Reusable scratch for cascading one element after another.
pub(super) struct Cascade {
    /// Each block with the rule index entry it is of, or `u32::MAX` for an element's own, and the
    /// tree scope its rule decides from.
    blocks: Vec<(CascadePriority, WinnerSource, bool, u32, TreeScopeID)>,
    decided: [u64; LONGHAND_WORDS],
    /// The best declaration of each longhand offered, by longhand, where `decided` says one was:
    /// the others are never written, so a cascade touches only the room of the longhands offered.
    best: Box<[std::mem::MaybeUninit<Candidate>]>,
    /// The longhands with a declaration offered, in the order they were first.
    offered: Vec<u16>,
    custom_candidates: Vec<CustomCandidate>,
    /// The declaration that won each decided longhand, highest priority first.
    pub(super) winners: Vec<Winner>,
    /// The custom property declaration that won each name, highest priority first, with the
    /// value it was written with.
    pub(super) custom_winners: Vec<(CustomDeclaration, RetainedStyleValueData)>,
    decided_names: Vec<StyleAtomID>,
    /// Each declaration of `revert` or `revert-layer` the ordered cascade met for a custom
    /// property it had not decided, with the name.
    reverting_names: Vec<(StyleAtomID, Reverting)>,
    /// The pseudo-element kind an element backing one of its shadow host's pseudo-elements stands
    /// for, which takes from the host's rules only the properties that pseudo-element supports, or
    /// `u8::MAX`.
    backed_kind: u8,
    /// The pseudo-element kind the last cascade was for, which takes from the rules only the
    /// properties that pseudo-element supports, or `u8::MAX` for the element itself.
    pseudo_kind: u8,
    /// The scope the winning `animation-name` was declared in, where the `@keyframes` it names
    /// are looked for first.
    pub(super) animation_name_scope: DeclarationScope,
    pub(super) statistics: CascadeCounters,
    /// The declarations a cascade of an element's own declarations alone takes in place of its
    /// node's.
    declared_only: Option<Box<DeclaredOnly>>,
}

impl Cascade {
    /// The best declaration of the longhand in `slot` offered so far, which `decided` says there is.
    fn best(&self, slot: usize) -> &Candidate {
        assert!(
            self.decided[slot / 64] & (1 << (slot % 64)) != 0,
            "a longhand's best declaration is read once one was offered"
        );
        // SAFETY: A longhand is decided once its best declaration is written, and a cascade clears
        //         every decided longhand before it starts.
        unsafe { self.best[slot].assume_init_ref() }
    }

    pub(super) fn new() -> Self {
        Self {
            blocks: Vec::new(),
            decided: [0; LONGHAND_WORDS],
            best: Box::new_uninit_slice(LONGHAND_COUNT),
            offered: Vec::new(),
            custom_candidates: Vec::new(),
            winners: Vec::new(),
            custom_winners: Vec::new(),
            decided_names: Vec::new(),
            reverting_names: Vec::new(),
            backed_kind: u8::MAX,
            pseudo_kind: u8::MAX,
            animation_name_scope: DeclarationScope::Unscoped,
            statistics: CascadeCounters::default(),
            declared_only: None,
        }
    }

    /// The cascade of `declared` alone, the own declarations of an element no rule reaches, keyed
    /// by `node`, which holds no declarations of its own.
    pub(super) fn of_declared_only(
        state: &RetainedState,
        index: &RuleIndex,
        node: StyleNodeID,
        declared: DeclaredOnly,
    ) -> Self {
        let mut cascade = Self::new();
        cascade.declared_only = Some(Box::new(declared));
        cascade.cascade(state, index, node, &[], u8::MAX);
        cascade
    }

    /// The declarations the cascade takes in place of its node's, where it is of an element's own
    /// declarations alone.
    pub(super) fn declared_only(&self) -> Option<&DeclaredOnly> {
        self.declared_only.as_deref()
    }

    /// Cascade the element's non-pseudo-element matches and element declarations into `winners`.
    pub(super) fn cascade_element(
        &mut self,
        state: &RetainedState,
        index: &RuleIndex,
        node: StyleNodeID,
        matches: &[ElementMatch],
    ) {
        self.backed_kind = u8::MAX;
        self.cascade(state, index, node, matches, u8::MAX);
    }

    /// Cascade an element standing for its shadow host's pseudo-element of `kind`, such as the
    /// element a `::placeholder` is: the host's matches for that pseudo-element, of which it takes
    /// only the properties the pseudo-element supports, with the element's own declarations.
    pub(super) fn cascade_backing_element(
        &mut self,
        state: &RetainedState,
        index: &RuleIndex,
        node: StyleNodeID,
        host_matches: &[ElementMatch],
        kind: u8,
    ) {
        self.backed_kind = kind;
        self.cascade(state, index, node, host_matches, kind);
    }

    /// Whether a declaration of `property` by `source` takes part in the cascade: a rule's for a
    /// pseudo-element where the pseudo-element supports the property.
    fn takes(&self, source: WinnerSource, property: u16) -> bool {
        self.pseudo_kind == u8::MAX
            || !matches!(source, WinnerSource::Rule(_))
            || crate::css::property_metadata::pseudo_element_supports_property(self.pseudo_kind, property)
    }

    /// Cascade the element's matches for its pseudo-element of `kind` into `winners`, as
    /// [`Self::cascade_element`] does for the element: the element's own declarations style the
    /// element alone.
    pub(super) fn cascade_pseudo_element(
        &mut self,
        state: &RetainedState,
        index: &RuleIndex,
        node: StyleNodeID,
        matches: &[ElementMatch],
        kind: u8,
    ) {
        self.backed_kind = u8::MAX;
        self.cascade(state, index, node, matches, kind);
    }

    fn cascade(
        &mut self,
        state: &RetainedState,
        index: &RuleIndex,
        node: StyleNodeID,
        matches: &[ElementMatch],
        pseudo_kind: u8,
    ) {
        self.pseudo_kind = pseudo_kind;
        self.decided = [0; LONGHAND_WORDS];
        self.winners.clear();
        self.custom_winners.clear();
        self.decided_names.clear();
        self.reverting_names.clear();
        self.blocks.clear();
        self.animation_name_scope = DeclarationScope::Unscoped;

        // Matches are in index order, so a rule's entries for one target are adjacent among the
        // target's; its greatest specificity decides for the rule.
        let mut targeted = matches
            .iter()
            .filter(|matched| matched.pseudo_kind == pseudo_kind)
            .peekable();
        while let Some(first) = targeted.next() {
            let mut specificity: Specificity = first.specificity;
            let mut scope_proximity = first.scope_proximity;
            // A rule matched from several of the element's encapsulation contexts decides from each.
            while let Some(next) = targeted.next_if(|next| next.rule == first.rule && next.scope == first.scope) {
                specificity = specificity.max(next.specificity);
                scope_proximity = scope_proximity.min(next.scope_proximity);
            }
            // NB: The entry is read in place, from the segment holding it where it is a bound one.
            let entry = index.entries.view(first.entry as usize);
            let placement = index.entries.placement_of(entry);
            for important in [false, true] {
                if important && !entry.declares_important() {
                    continue;
                }
                let priority = CascadePriority::new(PriorityInputs {
                    origin: placement.origin,
                    important,
                    context_depth: first.context_depth,
                    element_attachment: ElementAttachment::Rule,
                    layer_rank: placement.layer_rank,
                    specificity,
                    scope_proximity,
                    sheet_rank: placement.sheet_rank,
                    rule_rank: entry.rule_rank(),
                });
                self.blocks.push((
                    priority,
                    WinnerSource::Rule(first.rule),
                    important,
                    first.entry,
                    first.scope,
                ));
            }
        }
        let own = self.declared_only.as_deref();
        let element_declares_custom_properties = custom_declarations_of(
            state,
            index,
            own,
            node,
            WinnerSource::Element(ElementDeclarationKind::InlineStyle),
            u32::MAX,
        )
        .is_some_and(|(declared, _)| !declared.is_empty());
        // An element's own declarations style the element alone, which is what an element backing
        // its host's pseudo-element is.
        for kind in ElementDeclarationKind::ALL
            .into_iter()
            .filter(|_| pseudo_kind == u8::MAX || self.backed_kind != u8::MAX)
        {
            let declares_custom_properties =
                kind == ElementDeclarationKind::InlineStyle && element_declares_custom_properties;
            if block_declarations(state, own, node, WinnerSource::Element(kind)).is_empty()
                && !declares_custom_properties
            {
                continue;
            }
            for important in [false, true] {
                let priority = state.element_cascade_priority(node, kind, important);
                self.blocks.push((
                    priority,
                    WinnerSource::Element(kind),
                    important,
                    u32::MAX,
                    TreeScopeID::DOCUMENT,
                ));
            }
        }
        if self.blocks.len() > ORDERED_BLOCKS {
            self.cascade_unordered(state, index, node);
            return;
        }
        self.blocks.sort_unstable_by_key(|block| std::cmp::Reverse(block.0));
        let entries = &index.entries;

        for &(priority, source, important, entry, scope) in &self.blocks {
            // https://drafts.csswg.org/css-variables-2/#defining-variables
            // A custom property cascades by its name, the way a longhand does.
            if let Some((declared, written)) =
                custom_declarations_of(state, index, self.declared_only.as_deref(), node, source, entry)
            {
                for (declaration, written) in declared.iter().zip(written) {
                    if declaration.important() != important
                        || self.decided_names.contains(&declaration.name)
                        || self.reverting_names.iter().any(|(name, (reverting, operator))| {
                            *name == declaration.name && reverting.reverts_past(*operator, &priority)
                        })
                    {
                        continue;
                    }
                    match declaration.operator() {
                        // https://drafts.csswg.org/css-cascade-5/#default
                        // `revert` and `revert-layer` roll the cascaded value back to what lower
                        // declarations they do not roll back past cascade to.
                        operator @ (CascadeOperator::Revert | CascadeOperator::RevertLayer) => {
                            self.reverting_names.push((declaration.name, (priority, operator)));
                        }
                        CascadeOperator::Declared => {
                            self.decided_names.push(declaration.name);
                            self.custom_winners.push((*declaration, written.clone()));
                        }
                        // A registered name's CSS-wide keywords take its registration's initial value, or
                        // the parent's value of one that does not inherit, which the store resolves
                        // from the keyword written.
                        CascadeOperator::Inherit | CascadeOperator::Unset | CascadeOperator::Initial
                            if state.custom_property_name_is_registered(declaration.name) =>
                        {
                            self.decided_names.push(declaration.name);
                            self.custom_winners.push((*declaration, written.clone()));
                        }
                        // https://drafts.csswg.org/css-variables-2/#defining-variables
                        // Custom properties are inherited, so these keep the inherited value.
                        CascadeOperator::Inherit | CascadeOperator::Unset => self.decided_names.push(declaration.name),
                        // The initial value of a custom property is the guaranteed-invalid value.
                        CascadeOperator::Initial => {
                            self.decided_names.push(declaration.name);
                            self.custom_winners.push((
                                *declaration,
                                RetainedStyleValueData::from_owned(
                                    crate::css::style_value::StyleValueData::GuaranteedInvalid,
                                ),
                            ));
                        }
                    }
                }
            }
            let declarations = block_declarations(state, self.declared_only.as_deref(), node, source);
            // https://drafts.csswg.org/css-cascade-5/#cascade-order
            // Within a block, the last declaration in document order wins. The winners keep that
            // order, which is how the computation pairs a logical longhand with its physical
            // counterpart: of the two, the one declared with higher priority decides both.
            for (index, declaration) in declarations.iter().enumerate().rev() {
                if declaration.important() != important {
                    continue;
                }
                // A shorthand declared whole waits on substitution, and its longhands are declared
                // beside it, each pending the shorthand's value.
                if !(FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&declaration.property()) {
                    continue;
                }
                if !self.takes(source, declaration.property()) {
                    continue;
                }
                let slot = usize::from(declaration.property() - FIRST_LONGHAND_PROPERTY_ID);
                let (word, bit) = (slot / 64, 1_u64 << (slot % 64));
                if self.decided[word] & bit != 0 {
                    continue;
                }
                self.decided[word] |= bit;
                if declaration.property() == ANIMATION_NAME {
                    self.animation_name_scope = declaration_scope(entries, entry, scope);
                }
                self.winners.push(Winner {
                    property: declaration.property(),
                    important: declaration.important(),
                    source,
                    index,
                });
            }
        }
        self.statistics.winners += self.winners.len() as u64;
    }

    /// Cascade the blocks gathered in any order, keeping the best declaration of each longhand and
    /// custom property name.
    fn cascade_unordered(&mut self, state: &RetainedState, index: &RuleIndex, node: StyleNodeID) {
        self.offered.clear();
        self.custom_candidates.clear();
        for block in 0..self.blocks.len() {
            let (priority, source, important, entry, scope) = self.blocks[block];
            let (declarations, custom): BlockDeclarations<'_> = (
                block_declarations(state, self.declared_only.as_deref(), node, source),
                custom_declarations_of(state, index, self.declared_only.as_deref(), node, source, entry),
            );
            for (index, declaration) in declarations.iter().enumerate() {
                if declaration.important() != important {
                    continue;
                }
                if !(FIRST_LONGHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&declaration.property())
                    || !self.takes(source, declaration.property())
                {
                    continue;
                }
                let slot = usize::from(declaration.property() - FIRST_LONGHAND_PROPERTY_ID);
                let (word, bit) = (slot / 64, 1_u64 << (slot % 64));
                let candidate = Candidate {
                    priority,
                    important,
                    source,
                    index,
                    entry,
                    scope,
                };
                if self.decided[word] & bit == 0 {
                    self.decided[word] |= bit;
                    self.best[slot].write(candidate);
                    self.offered.push(slot as u16);
                    continue;
                }
                let best = self.best(slot);
                if priority > best.priority
                    || (priority == best.priority && source == best.source && index > best.index)
                {
                    self.best[slot].write(candidate);
                }
            }
            let Some((declared, written)) = custom else {
                continue;
            };
            for (declaration, written) in declared.iter().zip(written) {
                if declaration.important() != important {
                    continue;
                }
                match self
                    .custom_candidates
                    .iter_mut()
                    .find(|candidate| candidate.declaration.name == declaration.name)
                {
                    Some(candidate) if priority > candidate.priority => {
                        candidate.priority = priority;
                        candidate.declaration = *declaration;
                        candidate.written = written.clone();
                    }
                    Some(_) => {}
                    None => self.custom_candidates.push(CustomCandidate {
                        priority,
                        declaration: *declaration,
                        written: written.clone(),
                    }),
                }
            }
        }

        // https://drafts.csswg.org/css-variables-2/#defining-variables
        // A custom property cascades by its name, the way a longhand does.
        self.custom_candidates
            .sort_unstable_by_key(|candidate| std::cmp::Reverse(candidate.priority));
        for mut candidate in std::mem::take(&mut self.custom_candidates) {
            if is_revert(candidate.declaration.operator()) {
                let mut reverted: SmallVec<[Reverting; 2]> =
                    smallvec::smallvec![(candidate.priority, candidate.declaration.operator())];
                match self.custom_property_below(state, index, node, candidate.declaration.name, &mut reverted) {
                    Some(below) => candidate = below,
                    // Nothing below declares the name, which then inherits.
                    None => continue,
                }
            }
            let declaration = candidate.declaration;
            match declaration.operator() {
                CascadeOperator::Declared => self.custom_winners.push((declaration, candidate.written)),
                // A registered name's CSS-wide keywords take its registration's initial value, or
                // the parent's value of one that does not inherit, which the store resolves from
                // the keyword written.
                CascadeOperator::Inherit | CascadeOperator::Unset | CascadeOperator::Initial
                    if state.custom_property_name_is_registered(declaration.name) =>
                {
                    self.custom_winners.push((declaration, candidate.written));
                }
                // https://drafts.csswg.org/css-variables-2/#defining-variables
                // Custom properties are inherited, so these keep the inherited value.
                CascadeOperator::Inherit | CascadeOperator::Unset => {}
                // The initial value of a custom property is the guaranteed-invalid value.
                CascadeOperator::Initial => self.custom_winners.push((
                    declaration,
                    RetainedStyleValueData::from_owned(crate::css::style_value::StyleValueData::GuaranteedInvalid),
                )),
                CascadeOperator::Revert | CascadeOperator::RevertLayer => {
                    unreachable!("a reverting custom declaration rolls back above")
                }
            }
        }

        let animation_name_slot = usize::from(ANIMATION_NAME - FIRST_LONGHAND_PROPERTY_ID);
        if self.offered.contains(&(animation_name_slot as u16)) {
            let best = self.best(animation_name_slot);
            self.animation_name_scope = declaration_scope(&index.entries, best.entry, best.scope);
        }
        let mut winners: SmallVec<[Candidate; 16]> =
            self.offered.iter().map(|&slot| *self.best(usize::from(slot))).collect();
        // Highest priority first, and a block's later declarations before its earlier ones, as the
        // ordered cascade has them.
        winners.sort_unstable_by(|a, b| b.priority.cmp(&a.priority).then(b.index.cmp(&a.index)));
        self.winners.extend(winners.iter().map(|candidate| {
            let property = match candidate.source {
                WinnerSource::Rule(rule) => state.rules.declared_properties_of(rule)[candidate.index].property(),
                WinnerSource::Element(_) => {
                    block_declarations(state, self.declared_only.as_deref(), node, candidate.source)[candidate.index]
                        .property()
                }
            };
            Winner {
                property,
                important: candidate.important,
                source: candidate.source,
                index: candidate.index,
            }
        }));
        self.statistics.winners += self.winners.len() as u64;
    }

    /// The declaration of the longhand `property` the cascade rolls back to below the
    /// declarations of `revert` and `revert-layer` in `reverted`: the highest one they do not roll
    /// back past, where one that reverts in turn joins them. None where nothing below declares
    /// the longhand.
    fn longhand_below(
        &self,
        state: &RetainedState,
        node: StyleNodeID,
        property: u16,
        reverted: &mut SmallVec<[Reverting; 2]>,
    ) -> Option<(CascadePriority, Winner)> {
        loop {
            let mut best: Option<(CascadePriority, Winner, CascadeOperator)> = None;
            for &(priority, source, important, _, _) in &self.blocks {
                if best.is_some_and(|(best, ..)| best >= priority)
                    || rolled_back(reverted, &priority)
                    || !self.takes(source, property)
                {
                    continue;
                }
                let declarations = block_declarations(state, self.declared_only.as_deref(), node, source);
                // As in the cascade, the last declaration of the longhand in a block wins in it.
                if let Some(at) = declarations
                    .iter()
                    .rposition(|declaration| declaration.property() == property && declaration.important() == important)
                {
                    best = Some((
                        priority,
                        Winner {
                            property,
                            important,
                            source,
                            index: at,
                        },
                        declarations[at].operator(),
                    ));
                }
            }
            let (priority, winner, operator) = best?;
            if !is_revert(operator) {
                return Some((priority, winner));
            }
            reverted.push((priority, operator));
        }
    }

    /// What [`Self::longhand_below`] says for the custom property `name`.
    fn custom_property_below(
        &self,
        state: &RetainedState,
        index: &RuleIndex,
        node: StyleNodeID,
        name: StyleAtomID,
        reverted: &mut SmallVec<[Reverting; 2]>,
    ) -> Option<CustomCandidate> {
        loop {
            let mut best: Option<CustomCandidate> = None;
            for &(priority, source, important, entry, _) in &self.blocks {
                if best.as_ref().is_some_and(|best| best.priority >= priority) || rolled_back(reverted, &priority) {
                    continue;
                }
                let Some((declared, written)) =
                    custom_declarations_of(state, index, self.declared_only.as_deref(), node, source, entry)
                else {
                    continue;
                };
                if let Some(at) = declared
                    .iter()
                    .rposition(|declaration| declaration.name == name && declaration.important() == important)
                {
                    best = Some(CustomCandidate {
                        priority,
                        declaration: declared[at],
                        written: written[at].clone(),
                    });
                }
            }
            let best = best?;
            if !is_revert(best.declaration.operator()) {
                return Some(best);
            }
            reverted.push((best.priority, best.declaration.operator()));
        }
    }
}

/// The declarations of one of an element's cascade blocks.
fn block_declarations<'a>(
    state: &'a RetainedState,
    own: Option<&'a DeclaredOnly>,
    node: StyleNodeID,
    source: WinnerSource,
) -> &'a [DeclarationEntry] {
    match (source, own) {
        (WinnerSource::Rule(rule), _) => state.rules.declared_properties_of(rule),
        (WinnerSource::Element(kind), Some(own)) => own.declarations.declarations(kind).declared(),
        (WinnerSource::Element(kind), None) => state.element_declarations.declared_properties(node, kind),
    }
}

/// The custom property declarations of one of an element's cascade blocks, with the values they
/// were written with.
fn custom_declarations_of<'a>(
    state: &'a RetainedState,
    index: &'a RuleIndex,
    own: Option<&'a DeclaredOnly>,
    node: StyleNodeID,
    source: WinnerSource,
    entry: u32,
) -> Option<(&'a [CustomDeclaration], &'a [RetainedStyleValueData])> {
    match source {
        WinnerSource::Rule(_) if !index.entries.view(entry as usize).declares_custom_properties() => None,
        WinnerSource::Rule(rule) => Some((
            state.rules.custom_declarations_of(rule),
            state.rules.custom_written_values_of(rule),
        )),
        WinnerSource::Element(ElementDeclarationKind::InlineStyle) => match own {
            Some(own) => own.declarations.custom(),
            None => state.element_declarations.custom(node),
        }
        .map(|declarations| (declarations.declared(), declarations.written_values())),
        _ => None,
    }
}

/// The scope a declaration of the block of the rule index entry `entry`, deciding from the tree
/// scope `scope`, was declared in, or of an element's own block where `entry` is `u32::MAX`: an
/// author rule's is the tree scope of the sheet attachment it decides from, and no other
/// declaration has a scope of its own.
pub(super) fn declaration_scope(
    entries: &super::rules::IndexEntries,
    entry: u32,
    scope: TreeScopeID,
) -> DeclarationScope {
    let Some(view) = entries.try_view(entry as usize) else {
        return DeclarationScope::Unscoped;
    };
    match (entries.placement_of(view).origin, scope) {
        (CascadeOrigin::Author, scope) if scope != TreeScopeID::DOCUMENT => DeclarationScope::Shadow(scope),
        _ => DeclarationScope::Unscoped,
    }
}

/// The last cascade of one element, which a winner of `revert` or `revert-layer`, declared or
/// substituted, rolls back through.
pub(super) struct CascadeBelow<'a> {
    pub(super) cascade: &'a Cascade,
    pub(super) state: &'a RetainedState,
    pub(super) node: StyleNodeID,
}

impl super::compute::CascadeBelow for CascadeBelow<'_> {
    fn priority_of(&self, source: WinnerSource, important: bool) -> CascadePriority {
        self.cascade
            .blocks
            .iter()
            .find(|block| block.1 == source && block.2 == important)
            .expect("a winner's block is in its cascade")
            .0
    }

    fn winner_below(
        &self,
        property: u16,
        reverted: &[(CascadePriority, CascadeOperator)],
    ) -> Option<(CascadePriority, Winner)> {
        let mut reverted: SmallVec<[Reverting; 2]> = reverted.into();
        self.cascade
            .longhand_below(self.state, self.node, property, &mut reverted)
    }

    fn declared_only(&self) -> Option<&DeclaredOnly> {
        self.cascade.declared_only()
    }
}
