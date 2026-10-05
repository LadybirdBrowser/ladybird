/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::bridge::{
    FfiElementArrival, FfiElementDeclarationDelta, FfiElementStyleInput, FfiLocalFeatureDelta, FfiStateDelta,
    FfiTreeDelta,
};
use super::*;
use crate::css::declaration_block;

/// The boundary owns instrumentation separately so engine operations borrow an explicit sink.
pub struct StyleEngine {
    pub(super) counters: Counters,
    pub(super) state: StyleEngineState,
}

// The engine may move to whichever thread runs a style stage. Every host object it keeps a
// reference to says by its type whether it may be shared between threads, so one that cannot be
// fails this rather than passing as an opaque pointer.
const _: () = {
    const fn assert_send<T: Send>() {}
    assert_send::<StyleEngine>();
};

impl std::ops::Deref for StyleEngine {
    type Target = StyleEngineState;

    fn deref(&self) -> &Self::Target {
        &self.state
    }
}

impl std::ops::DerefMut for StyleEngine {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.state
    }
}

impl StyleEngine {
    #[must_use]
    pub fn new(device_class: DeviceClass) -> Self {
        Self {
            counters: Counters::new(),
            state: StyleEngineState::new(device_class),
        }
    }

    #[inline]
    pub(crate) fn intern_element_declared_properties(
        &mut self,
        declarations: &[declaration_block::DeclaredProperty],
    ) -> Vec<(DeclaredProperty, RetainedStyleValueData)> {
        self.state
            .intern_element_declared_properties(declarations, &mut self.counters)
    }

    #[must_use]
    pub fn counters(&self) -> &Counters {
        &self.counters
    }

    /// Apply one flat transaction. Tree deltas are staged in arrival order so derived neighbour
    /// rows follow the live tree step by step, while the journal normalizes for discovery.
    #[inline]
    pub fn apply_transaction_batch(
        &mut self,
        tree_deltas: &[FfiTreeDelta],
        arrival_columns: (&[FfiElementArrival], &[u32]),
        local_feature_deltas: &[FfiLocalFeatureDelta],
        state_deltas: &[FfiStateDelta],
        element_declaration_deltas: &[FfiElementDeclarationDelta],
        element_style_inputs: &[FfiElementStyleInput],
    ) {
        self.state.apply_transaction_batch(
            tree_deltas,
            arrival_columns,
            local_feature_deltas,
            state_deltas,
            element_declaration_deltas,
            element_style_inputs,
            &mut self.counters,
        );
    }

    /// Record what a custom property's name atom spells, and the fly string it is.
    ///
    /// # Safety
    /// `raw` must be zero or a live `AK::Utf16FlyString` raw representation.
    #[inline]
    pub unsafe fn note_custom_property_name(&mut self, name: StyleAtomID, raw: usize, text: &[u16]) {
        unsafe {
            self.state
                .note_custom_property_name(name, raw, text, &mut self.counters);
        }
    }

    /// Add a style rule that only applies inside a scope, naming the scope's root selectors.
    ///
    /// `before` places the rule immediately ahead of an existing one instead of at the end. A rule
    /// arriving in the middle of a sheet takes an order token between its neighbours: nothing else
    /// is renumbered, and no other rule's identity or compiled program is touched.
    #[inline]
    pub fn add_style_rule_in_scope(
        &mut self,
        sheet: SheetID,
        before: Option<RuleID>,
        selectors: &[&CompiledSelector],
        namespaces: NamespaceScope,
        scope: &ScopeChain<'_>,
    ) -> RuleID {
        self.state
            .add_style_rule_in_scope(sheet, before, selectors, namespaces, scope, &mut self.counters)
    }

    /// Add a style rule of a user-agent sheet, compiling its selectors only if no document in the
    /// process has compiled them with the same inputs before.
    #[inline]
    pub(super) fn add_user_agent_style_rule(
        &mut self,
        sheet: SheetID,
        before: Option<RuleID>,
        rule_identity: u64,
        selectors: &[&CompiledSelector],
        namespaces: Option<&super::rule_writes::NamespaceTexts>,
    ) -> RuleID {
        self.state
            .add_user_agent_style_rule(sheet, before, rule_identity, selectors, namespaces, &mut self.counters)
    }

    /// Record that a sheet declared or gave up a cascade layer.
    ///
    /// The declaration contributes no declarations and matches nothing: what it does is fix the order
    /// of the layers every rule referencing them sits in. A layer name belongs to the tree scope the
    /// sheet is attached to, so what moves is that scope's layer order - one input per scope the sheet
    /// decides in.
    #[inline]
    pub fn record_layer_statement(&mut self, sheet: SheetID) {
        self.state.record_layer_statement(sheet, &mut self.counters);
    }

    /// Add a `@keyframes` rule, which matches no element and is found by the name it declares.
    ///
    /// It has to be in the program at all for a change to it to be an input, and it has to carry its
    /// name for that input to reach the animations referencing it.
    #[inline]
    pub fn add_keyframes_rule(&mut self, sheet: SheetID, before: Option<RuleID>, name: StyleAtomID) -> RuleID {
        self.state.add_keyframes_rule(sheet, before, name, &mut self.counters)
    }

    /// Add an `@property` rule, which registers the custom property it names. Registering one changes
    /// how every element that declares or references it computes, which the custom-property index
    /// knows and selector matching cannot say.
    #[inline]
    pub fn add_property_rule(&mut self, sheet: SheetID, before: Option<RuleID>, name: StyleAtomID) -> RuleID {
        self.state.add_property_rule(sheet, before, name, &mut self.counters)
    }

    /// Add a rule that matches no element and is not found by name either, so that a change to it is
    /// an input at all. What it reaches is decided by its kind.
    #[inline]
    pub fn add_non_matching_rule(&mut self, sheet: SheetID, before: Option<RuleID>, kind: RuleKind) -> RuleID {
        self.state
            .add_non_matching_rule(sheet, before, kind, &mut self.counters)
    }

    /// Give an existing rule a new selector list, keeping its identity and its position.
    ///
    /// Editing `selectorText` is one rule changing, not the sheet being rebuilt. The rule keeps its
    /// order token, so nothing around it is renumbered, and the journal sees exactly one selector
    /// field move.
    #[inline]
    pub fn replace_style_rule_selectors(
        &mut self,
        rule: RuleID,
        selectors: &[&CompiledSelector],
        namespaces: NamespaceScope,
        scope: &ScopeChain<'_>,
    ) {
        self.state
            .replace_style_rule_selectors(rule, selectors, namespaces, scope, &mut self.counters);
    }

    /// Report that a rule's declarations moved, without touching anything else about it.
    ///
    /// The block's contents changed even where the CSSOM object did not, so what makes this a
    /// change is a version rather than the object's address. The rule keeps its identity, its
    /// position, and its selector program, so the journal sees one field move and routing reaches
    /// exactly the elements that rule matches.
    #[inline]
    pub fn record_rule_declarations_changed(&mut self, rule: RuleID, block_version: u32) {
        self.state
            .record_rule_declarations_changed(rule, block_version, &mut self.counters);
    }

    /// Make the identities the host minted live, ahead of anything the host records about their
    /// nodes.
    #[inline]
    pub fn mint_style_nodes(&mut self, nodes: &[u32]) {
        self.state.mint_style_nodes(nodes, &mut self.counters);
    }

    /// Mint `out.len()` element identities from the tree's own space, for a test.
    #[cfg(test)]
    pub fn allocate_style_nodes(&mut self, out: &mut [u32]) {
        for slot in out.iter_mut() {
            *slot = self.state.retained.tree.mint_test_identity(false).raw();
        }
        self.mint_style_nodes(out);
    }

    /// Mint `out.len()` text identities from the tree's own space, for a test.
    #[cfg(test)]
    pub fn allocate_text_style_nodes(&mut self, out: &mut [u32]) {
        for slot in out.iter_mut() {
            *slot = self.state.retained.tree.mint_test_identity(true).raw();
        }
        self.mint_style_nodes(out);
    }

    /// Stage a structural change. The normalized transaction installs the final relation rows at
    /// the next observation boundary.
    #[inline]
    pub fn record_tree_delta(&mut self, node: StyleNodeID, old: Option<TreeRelations>, new: Option<TreeRelations>) {
        self.state.record_tree_delta(node, old, new, &mut self.counters);
    }

    /// Record a change to style inputs which are properties of the document environment rather
    /// than of an element or stylesheet rule.
    #[inline]
    pub fn record_environment_change(&mut self) {
        self.state.record_environment_change(&mut self.counters);
    }

    /// Record a registration made through `CSS.registerProperty()`. Stylesheet registrations are
    /// already represented by their `@property` rule's program input.
    #[inline]
    pub fn record_custom_property_registration_change(&mut self, name: StyleAtomID) {
        self.state
            .record_custom_property_registration_change(name, &mut self.counters);
    }

    #[inline]
    pub fn record_input(&mut self, key: InputKey, old: InputValue, new: InputValue) {
        self.state.record_input(key, old, new, &mut self.counters);
    }

    /// Preserve the pending paint-only selector facts as the style change event established by a
    /// geometry read. Repeated reads advance the same boundary to the latest observed facts.
    /// Returning false means exact journalling coarsened while combining the facts, so the caller
    /// must settle style instead of reusing layout.
    #[inline]
    pub fn defer_pending_transaction_for_geometry_read(&mut self) -> bool {
        self.state
            .defer_pending_transaction_for_geometry_read(&mut self.counters)
    }

    /// Install final staged relation rows at the transaction barrier.
    #[inline]
    #[cfg(test)]
    pub(super) fn apply_staged_tree_deltas(&mut self) {
        self.state.apply_staged_tree_deltas(&mut self.counters);
    }

    /// Record the shadow parts an element exposes.
    ///
    /// A part is a fact about the element like a class is: posted so `::part()` rules can be
    /// enumerated from it, and journalled so a change to the `part` attribute routes. The plain
    /// name set is derived here from the name-to-host pairs exact matching needs, so the two views
    /// cannot disagree.
    #[inline]
    pub fn set_element_parts(&mut self, node: StyleNodeID, pairs: &[(StyleAtomID, StyleNodeID)]) {
        self.state.set_element_parts(node, pairs, &mut self.counters);
    }

    /// Report the outermost host a `::part()` rule can address this element from.
    ///
    /// What an `exportparts` change moves is which scopes can name an element, not which names it
    /// carries - the forwarded name is usually the one it already had. So the exposure is the fact,
    /// and an element whose reach did not move says nothing.
    #[inline]
    pub fn set_element_part_exposure(&mut self, node: StyleNodeID, exposure: StyleAtomID) {
        self.state.set_element_part_exposure(node, exposure, &mut self.counters);
    }

    #[inline]
    pub fn set_element_heading_level(&mut self, node: StyleNodeID, level: u8) {
        self.state.set_element_heading_level(node, level, &mut self.counters);
    }

    /// Record what a language atom spells, so `:lang()` can compare its ranges against the tag.
    #[inline]
    pub fn set_element_language_text(&mut self, language: StyleAtomID, text: &[u16]) {
        self.state.set_element_language_text(language, text, &mut self.counters);
    }

    #[inline]
    pub fn set_element_language(&mut self, node: StyleNodeID, language: StyleAtomID) {
        self.state.set_element_language(node, language, &mut self.counters);
    }

    /// Report the element's resolved directionality, which `:dir()` tests.
    #[inline]
    pub fn set_element_directionality(&mut self, node: StyleNodeID, directionality: StyleAtomID) {
        self.state
            .set_element_directionality(node, directionality, &mut self.counters);
    }

    /// Replace the custom states an element is in.
    ///
    /// A custom state is a named fact about one element, exactly like a class, so it is published as
    /// one: the names that arrived and the names that left are each a local feature moving, and
    /// `:state()` reaches its subjects through the same postings every other name does.
    #[inline]
    pub fn set_element_custom_states(&mut self, node: StyleNodeID, states: &[StyleAtomID]) {
        self.state.set_element_custom_states(node, states, &mut self.counters);
    }

    /// Attach a compiled program at the end of a scope's sheet order.
    #[inline]
    #[cfg(test)]
    pub(super) fn attach_sheet(&mut self, sheet: SheetID, tree_scope: TreeScopeID) {
        self.state.attach_sheet(sheet, tree_scope, &mut self.counters);
    }

    /// Attach a sheet immediately before another sheet in the same scope, or at the end when that
    /// sheet is not attached there. Order tokens stay inside the engine: callers name neighbours,
    /// never positions.
    #[inline]
    pub fn attach_sheet_before_sheet(&mut self, sheet: SheetID, before: Option<SheetID>, tree_scope: TreeScopeID) {
        self.state
            .attach_sheet_before_sheet(sheet, before, tree_scope, &mut self.counters);
    }

    #[inline]
    pub fn detach_sheet(&mut self, sheet: SheetID, tree_scope: TreeScopeID) {
        self.state.detach_sheet(sheet, tree_scope, &mut self.counters);
    }

    pub fn attach_sheet_occurrence(
        &mut self,
        sheet: SheetID,
        scope: TreeScopeID,
        identity: u64,
        before: u64,
        conditions_hold: bool,
    ) {
        self.state
            .attach_sheet_occurrence(sheet, scope, identity, before, conditions_hold, &mut self.counters);
    }

    pub fn detach_sheet_occurrence(&mut self, scope: TreeScopeID, identity: u64) {
        self.state.detach_sheet_occurrence(scope, identity, &mut self.counters);
    }

    pub fn set_sheet_occurrence_conditions(&mut self, scope: TreeScopeID, identity: u64, conditions_hold: bool) {
        self.state
            .set_sheet_occurrence_conditions(scope, identity, conditions_hold, &mut self.counters);
    }

    /// Record that a rule sits in a cascade layer.
    ///
    /// Said as the rule is compiled rather than as an input: which layer a rule is in is part of what
    /// the rule is, and a rule that moves between layers is recompiled.
    /// Record which longhand properties one of an element's own declarations covers.
    ///
    /// An element-attached declaration is a cascade component above layers: a style attribute beats
    /// every layered and unlayered rule in its context, whatever layer they are in.
    #[inline]
    pub fn set_element_declared_properties(
        &mut self,
        node: StyleNodeID,
        kind: ElementDeclarationKind,
        declarations: Vec<(DeclaredProperty, RetainedStyleValueData)>,
        custom_declarations: Vec<(CustomDeclaration, RetainedStyleValueData)>,
    ) {
        self.state
            .set_element_declared_properties(node, kind, declarations, custom_declarations, &mut self.counters);
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn intern_cascade_state(
        &mut self,
        winners: &[PropertyWinner],
        previous: Option<CascadeStateID>,
    ) -> CascadeStateID {
        self.state.intern_cascade_state(winners, previous, &mut self.counters)
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn matches_for_cascade(
        &mut self,
        all: Vec<RuleMatch>,
        can_have_scope_duplicates: bool,
        publish_winners_for: Option<StyleNodeID>,
    ) -> Vec<RuleMatch> {
        self.state.matches_for_cascade_immediately(
            all,
            can_have_scope_duplicates,
            publish_winners_for,
            &mut self.counters,
        )
    }

    /// Repair only the winner properties named by signed match changes.
    ///
    /// The retained exact answer supplies every contender, so deletion repair is a property-level
    /// upquery over rule identities rather than another selector match or whole-cascade reduction.
    #[inline]
    #[cfg(test)]
    pub(super) fn apply_cascade_winner_match_deltas(
        &mut self,
        node: StyleNodeID,
        matches: &[RuleMatch],
        deltas: &[SelectorTruthDelta],
        candidates: &mut Vec<OrderedCascadeCandidate>,
    ) -> bool {
        let mut effects = AnswerEffects::default();
        let result = self.state.apply_cascade_winner_match_deltas(
            &mut effects,
            node,
            matches,
            deltas,
            candidates,
            &mut self.counters,
        );
        self.state.install_answer_effects(effects);
        result
    }

    /// Exactly match every style node in the document scope against the attached program.
    ///
    /// This is the optimized batch path running on the real document. It packs the facts the store
    /// has published and evaluates the match programs its dispatch and prefix state reach. It
    /// reports how many concrete rule matches it found, or the node whose facts were missing - never
    /// a partial answer.
    #[inline]
    pub fn match_document(&mut self, root: StyleNodeID) -> Result<usize, Incomplete> {
        self.state.match_document(root, &mut self.counters)
    }

    /// Normalize and apply the staged inputs into one transaction. A required style observation
    /// drains here first, so normalization never combines changes across an observation boundary.
    #[inline]
    pub fn take_transaction(&mut self) -> StyleTransaction {
        self.state.take_transaction(&mut self.counters)
    }

    /// Settle inputs which cannot be planned while the document has no style root. Exact element
    /// style reactions are edge-triggered, so preserve them for the first transaction with a root.
    #[inline]
    pub(crate) fn flush_without_document_root(&mut self) {
        self.state.flush_without_document_root(&mut self.counters);
    }

    /// Leaves the atom sweep of the transactions taken until the next call to a later one, where `defers`.
    pub(crate) fn defer_atom_sweep(&mut self, defers: bool) {
        self.state.host.defers_atom_sweep = defers;
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn sweep_style_atoms(&mut self) {
        self.state.sweep_style_atoms(&mut self.counters);
    }

    #[inline]
    pub fn take_style_transaction(
        &mut self,
        root: StyleNodeID,
        emit: impl FnMut(StyleTransactionVersion, ProgramVersion, &[PublishedStyleDeltaRecord]),
    ) -> bool {
        self.state.take_style_transaction(root, emit, &mut self.counters)
    }

    #[inline]
    pub(crate) fn intern_declared_property(
        &mut self,
        declaration: &declaration_block::DeclaredProperty,
    ) -> DeclaredProperty {
        self.state.intern_declared_property(declaration, &mut self.counters)
    }

    /// Put the qualified layer names in the order one tree scope declares them in.
    #[inline]
    pub fn set_layer_order(&mut self, scope: TreeScopeID, layers: &[CascadeLayerID]) {
        self.state.set_layer_order(scope, layers, &mut self.counters);
    }

    /// Intern one immutable specified value into the rule program's dense value table.
    ///
    /// # Safety
    /// `value` must point at live `StyleValueData`.
    #[inline]
    pub unsafe fn intern_specified_value(&mut self, value: *const StyleValueData) -> SpecifiedValueID {
        unsafe { self.state.intern_specified_value(value, &mut self.counters) }
    }

    /// Record which cascade layer a rule sits in.
    ///
    /// Where a rule sits among the layers is part of what the rule is, and it is what the cascade
    /// compares; whether it is in one at all is what a layer topology change routes by.
    #[inline]
    pub fn set_rule_layer(&mut self, rule: RuleID, layer: CascadeLayerID) {
        self.state.set_rule_layer(rule, layer, &mut self.counters);
    }

    #[inline]
    pub fn set_rule_conditions_hold(&mut self, rule: RuleID, conditions_hold: bool) {
        self.state
            .set_rule_conditions_hold(rule, conditions_hold, &mut self.counters);
    }

    /// Record whether a sheet's conditions hold, as evaluating its media queries decides.
    #[inline]
    pub fn set_sheet_conditions_hold(&mut self, sheet: SheetID, conditions_hold: bool) {
        self.state
            .set_sheet_conditions_hold(sheet, conditions_hold, &mut self.counters);
    }

    #[cfg(test)]
    #[inline]
    pub(super) fn set_sheet_enabled(&mut self, sheet: SheetID, enabled: bool) {
        self.state.set_sheet_enabled(sheet, enabled, &mut self.counters);
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn append_rule(&mut self, sheet: SheetID, parent: Option<RuleID>, kind: RuleKind) -> RuleID {
        self.state.append_rule(sheet, parent, kind, &mut self.counters)
    }

    /// Delete a rule, taking its subtree with it. Every removed identity is journalled, because a
    /// deleted winning declaration has to be repaired wherever it was winning.
    #[inline]
    #[cfg(test)]
    pub(super) fn remove_rule(&mut self, rule: RuleID) -> Vec<RuleID> {
        self.state.remove_rule(rule, &mut self.counters)
    }

    /// Begin rebuilding a sheet while retaining compatible style-rule identities by cascade
    /// position. The parsed CSSOM objects are new, but an unchanged semantic rule does not become a
    /// departure followed by an arrival merely because the whole sheet was reparsed.
    #[inline]
    pub fn begin_sheet_rules_replacement(&mut self, sheet: SheetID) {
        self.state.begin_sheet_rules_replacement(sheet, &mut self.counters);
    }

    /// Finish a synchronous sheet rebuild, retiring unmatched old rules and publishing declaration
    /// changes on identities that were reused.
    #[inline]
    pub fn finish_sheet_rules_replacement(&mut self, sheet: SheetID, declaration_block: u32) {
        self.state
            .finish_sheet_rules_replacement(sheet, declaration_block, &mut self.counters);
    }

    /// Delete one rule and settle the program around it.
    #[inline]
    pub fn remove_style_rule(&mut self, rule: RuleID) {
        self.state.remove_style_rule(rule, &mut self.counters);
    }

    /// Replace a rule's contents and journal only the fields that actually changed.
    #[inline]
    #[cfg(test)]
    pub(super) fn replace_rule_version(&mut self, rule: RuleID, contents: RuleVersion) {
        self.state.replace_rule_version(rule, contents, &mut self.counters);
    }

    /// Take the pending transaction for the style consumer that immediately follows this call.
    ///
    /// Exact planning may already have packed the current facts and built document-order
    /// coordinates. Keep that Tier-4 workspace for matching instead of reconstructing it. The
    /// callback receives only accepted match-answer records after their complete payloads have been
    /// published. Versions are shared by the whole emitted batch rather than repeated per record.
    #[cfg(test)]
    #[inline]
    pub(super) fn take_style_transaction_nodes(&mut self, root: StyleNodeID, emit: impl FnMut(&[u32])) -> bool {
        self.state.take_style_transaction_nodes(root, emit, &mut self.counters)
    }

    #[inline]
    /// Ends the transaction the engine published last, and answers the identities its end released.
    pub(super) fn discard_style_transaction_outputs(&mut self) -> Vec<u32> {
        self.state.discard_style_transaction_outputs(&mut self.counters)
    }

    /// Route a possible relational witness through the anchors whose truth it can flip.
    ///
    /// The anchor set is bounded by the inverse of the query's axis - one element for a child or
    /// adjacent-sibling query, the ancestor path for a descendant one - and then filtered to the
    /// ones that actually carry the anchor compound's own feature. `.card:has(.error) .button`
    /// therefore reaches the `.button` elements under the `.card` ancestors of the changed
    /// `.error`, rather than every descendant of every ancestor.
    #[inline]
    #[cfg(test)]
    pub(super) fn route_from_anchors(
        &mut self,
        witness: StyleNodeID,
        program: SelectorProgramID,
        anchor: RelativeAnchor,
        site: &RoutingSite<'_>,
        regions: &mut ImpactRegions,
    ) {
        self.state
            .route_from_anchors(witness, program, anchor, site, regions, &mut self.counters);
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn add_prefix_convergence_regions(
        &mut self,
        pending: &mut PendingRoutes,
        regions: &mut ImpactRegions,
        retained_winners_are_current: bool,
        transaction: &StyleTransaction,
        sequences: &SequenceChanges,
        pending_prefix_producers: &[PendingPrefixProducer],
    ) -> PrefixConvergenceOutcome {
        self.state.add_prefix_convergence_regions(
            pending,
            regions,
            retained_winners_are_current,
            transaction,
            sequences,
            pending_prefix_producers,
            &mut self.counters,
        )
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn add_narrowed_region(
        &mut self,
        region: ImpactRegion,
        site: &RoutingSite<'_>,
        regions: &mut ImpactRegions,
    ) {
        self.state
            .add_narrowed_region(region, site, regions, &mut self.counters);
    }

    /// Resolve deferred exact-node routes after the plan's symbolic coverage is final.
    #[inline]
    #[cfg(test)]
    pub(super) fn resolve_already_planned_selector_truth(
        &mut self,
        regions: &ImpactRegions,
        coarse_cover: Option<&ImpactRegionBatch>,
    ) {
        self.state
            .resolve_already_planned_selector_truth(regions, coarse_cover, &mut self.counters);
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn materialize_cold_matching_batch(
        &mut self,
        root: StyleNodeID,
        topology: Option<&TransactionTopology>,
    ) -> Option<MatchingFactBatch> {
        self.state
            .materialize_cold_matching_batch(root, topology, &mut self.counters)
    }

    /// Share current facts and selector work while a scoped plan completes typed answer misses.
    ///
    /// Retained prefix caches describe the previous transaction, so this batch deliberately starts
    /// empty and is released before publication. Repeated local asks may still promote to one
    /// complete current fact batch after their accumulated reconstruction cost justifies it.
    #[inline]
    #[cfg(test)]
    pub(super) fn begin_published_match_answer_completion_batch(
        &mut self,
        root: StyleNodeID,
        prefer_complete_batch: bool,
    ) {
        self.state
            .begin_published_match_answer_completion_batch(root, prefer_complete_batch, &mut self.counters);
    }

    /// Materialize the document's facts once for a synchronous broad style traversal.
    ///
    /// The caller brackets one traversal explicitly, so engine mutations cannot leave a retained
    /// answer stale. If the batch exceeds the document's Tier-4 budget, drop it and let each
    /// element use the ordinary exact batch path.
    #[inline]
    pub fn begin_cold_matching_batch(&mut self, root: StyleNodeID) -> bool {
        self.state.begin_cold_matching_batch(root, &mut self.counters)
    }

    /// Begin a synchronous selective traversal without paying for broad facts up front.
    ///
    /// Local asks remain local instead of paying for broad facts up front.
    #[inline]
    pub fn begin_adaptive_cold_matching_batch(&mut self, root: StyleNodeID) {
        self.state.begin_adaptive_cold_matching_batch(root, &mut self.counters);
    }

    #[inline]
    pub fn end_cold_matching_batch(&mut self) {
        self.state.end_cold_matching_batch(&mut self.counters);
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn remember_retained_match_answer(&mut self, node: StyleNodeID, matches: &[RuleMatch]) {
        self.state
            .remember_retained_match_answer(node, matches, &mut self.counters);
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn remember_prepared_retained_match_answer_with_truth(
        &mut self,
        node: StyleNodeID,
        answer: Vec<RetainedRuleMatch>,
        selector_truth: Option<Vec<SelectorTruth>>,
    ) {
        self.state
            .remember_prepared_retained_match_answer_with_truth(node, answer, selector_truth, &mut self.counters);
    }

    /// Materialize selector incidence from current facts when no active retained answer names it.
    #[inline]
    #[cfg(test)]
    pub(super) fn materialize_current_selector_incidence(
        &mut self,
        program: SelectorProgramID,
    ) -> Option<Arc<[RetainedSelectorIncidence]>> {
        self.state
            .materialize_current_selector_incidence(program, &mut self.counters)
    }

    /// Apply a complete signed match delta directly to one retained exact answer.
    ///
    /// Entries with dynamic scope proximity still use the cold patch: their match row contains a
    /// value that the prefix match-set delta does not carry yet.
    #[inline]
    #[cfg(test)]
    pub(super) fn apply_retained_match_answer_deltas(
        &mut self,
        node: StyleNodeID,
        patch: &mut RetainedAnswerPatch,
        old_identity: MatchAnswerID,
        old_cascade_input: MatchAnswerID,
        deltas: &[SelectorTruthDelta],
    ) -> Option<RetainedAnswerPatchOutcome> {
        let mut effects = AnswerEffects::default();
        let result = self.state.apply_retained_match_answer_deltas(
            &mut effects,
            node,
            patch,
            old_identity,
            old_cascade_input,
            deltas,
            &mut self.counters,
        );
        self.state.install_answer_effects(effects);
        result
    }

    /// Patch one retained exact answer by re-evaluating only the rules reached by this transaction.
    ///
    /// `None` means the optional old side was absent or exact evaluation could not complete. The
    /// caller then retires the answer and publishes an ordinary exact reaction.
    #[inline]
    #[cfg(test)]
    pub(super) fn patch_retained_match_answer(
        &mut self,
        node: StyleNodeID,
        patch: &mut RetainedAnswerPatch,
        truth_patch: SelectorTruthPatch<'_>,
    ) -> Option<RetainedAnswerPatchOutcome> {
        let mut effects = AnswerEffects::default();
        let result = self
            .state
            .patch_retained_match_answer(&mut effects, node, patch, truth_patch, &mut self.counters);
        self.state.install_answer_effects(effects);
        result
    }

    /// Name one ask's answer so the cascade can share its expansion within this transaction.
    #[inline]
    #[cfg(test)]
    pub(super) fn remember_cascade_input(&mut self, node: StyleNodeID, matches: &[RuleMatch]) {
        self.state.remember_cascade_input(node, matches, &mut self.counters);
    }

    #[inline]
    pub fn match_element(&mut self, node: StyleNodeID) -> Result<Vec<RuleMatch>, Incomplete> {
        self.state.match_element(node, &mut self.counters)
    }

    /// Match one element and discard rules that cannot contribute to its cascade.
    #[inline]
    pub fn match_element_for_cascade(&mut self, node: StyleNodeID) -> Result<Vec<RuleMatch>, Incomplete> {
        self.state.match_element_for_cascade(node, &mut self.counters)
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn match_element_for_purpose(
        &mut self,
        node: StyleNodeID,
        compact_for_cascade: bool,
    ) -> Result<Vec<RuleMatch>, Incomplete> {
        self.state
            .match_element_for_purpose(node, compact_for_cascade, &mut self.counters)
    }

    #[cfg(test)]
    #[inline]
    pub(super) fn complete_published_match_answer(
        &mut self,
        node: StyleNodeID,
        retained_answer_dispatch: Option<&RuleDispatch>,
    ) -> Result<PublishedMatchAnswer, Incomplete> {
        self.state
            .complete_published_match_answer(node, retained_answer_dispatch, &mut self.counters)
    }

    /// Complete a node from a matching node's already compacted cascade input and winner rows.
    #[inline]
    #[cfg(test)]
    pub(super) fn complete_published_match_answer_from_cascade_input(
        &mut self,
        node: StyleNodeID,
        source: StyleNodeID,
        cascade_input: MatchAnswerID,
        cascade_winners_are_complete: bool,
    ) -> Option<PublishedMatchAnswer> {
        let mut effects = AnswerEffects::default();
        let result = self.state.complete_published_match_answer_from_cascade_input(
            &mut effects,
            node,
            source,
            cascade_input,
            cascade_winners_are_complete,
            &mut self.counters,
        );
        self.state.install_answer_effects(effects);
        result
    }

    /// Prove that an added-only transition between two compact answers cannot change any cascade
    /// winner, by checking every added rule's declared properties against the node's previously
    /// published winner rows: O(delta) instead of a cold re-derivation, so it holds even where
    /// the winner inventory is incomplete. Any gap answers false: a removal, a pseudo-targeted or
    /// scoped or container-gated added rule, an added rule whose declaration list is incomplete
    /// (custom properties), or provenance the previous state cannot answer for.
    #[inline]
    #[cfg(test)]
    pub(super) fn answer_transition_cannot_change_cascade(
        &mut self,
        node: StyleNodeID,
        previous_input: MatchAnswerID,
        current_input: MatchAnswerID,
    ) -> bool {
        self.state
            .answer_transition_cannot_change_cascade(node, previous_input, current_input, &mut self.counters)
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn retained_closure_cascade_input(&self, node: StyleNodeID) -> Option<MatchAnswerID> {
        let empty = AnswerEffects::default();
        let traversal = self.state.retained.batch_matching_traversal.as_ref();
        let effects = traversal.map_or(&empty, |traversal| &traversal.answer_effects);
        self.state.retained.retained_closure_cascade_input(effects, node)
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn verify_retained_cascade_input(&mut self, node: StyleNodeID, cascade_input: MatchAnswerID) {
        let traversal = self.state.retained.batch_matching_traversal.take();
        let empty = AnswerEffects::default();
        let effects = traversal.as_ref().map_or(&empty, |traversal| &traversal.answer_effects);
        self.state
            .verify_retained_cascade_input(effects, node, cascade_input, &mut self.counters);
        self.state.retained.batch_matching_traversal = traversal;
    }

    #[inline]
    pub fn complete_published_match_answers_for_closure(&mut self, nodes: &[StyleNodeID]) -> Result<(), Incomplete> {
        self.state
            .complete_published_match_answers_for_closure(nodes, &mut self.counters)
    }

    /// Consume the complete current answer which the immediately preceding style plan retained.
    ///
    /// A miss is not an incomplete selector answer. It means this transaction did not publish an
    /// answer for the node, so the caller may ask the ordinary exact matcher instead.
    #[inline]
    pub fn consume_published_match_answer(&mut self, node: StyleNodeID) -> Option<Vec<RuleMatch>> {
        self.state.consume_published_match_answer(node, &mut self.counters)
    }

    /// Read the shareable identity of one answer from the immediately preceding style transaction.
    ///
    /// A contextual answer has no identity and must still consume its complete payload. A shared
    /// identity lets a downstream cache answer before copying that payload across the bridge.
    #[inline]
    pub fn published_match_answer_signature(&mut self, node: StyleNodeID) -> Option<u32> {
        self.state.published_match_answer_signature(node, &mut self.counters)
    }

    /// Match one element from committed facts without consulting derived matching state.
    #[cfg(test)]
    #[inline]
    pub(super) fn match_element_with_exact_matcher(
        &mut self,
        node: StyleNodeID,
        compact_for_cascade: bool,
    ) -> Result<Vec<RuleMatch>, Incomplete> {
        self.state
            .match_element_with_exact_matcher(node, compact_for_cascade, &mut self.counters)
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn exact_match_answer_for_verification(
        &mut self,
        node: StyleNodeID,
    ) -> Result<Vec<RuleMatch>, Incomplete> {
        self.state.exact_match_answer_for_verification(node, &mut self.counters)
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn exact_cascade_answer_for_verification(
        &mut self,
        node: StyleNodeID,
    ) -> Result<(Vec<RuleMatch>, WinnerGroups), Incomplete> {
        self.state
            .exact_cascade_answer_for_verification(node, &mut self.counters)
    }

    #[inline]
    #[cfg(test)]
    pub(super) fn match_element_for_purpose_with_compact_answer(
        &mut self,
        node: StyleNodeID,
        compact_for_cascade: bool,
        completion_exactness: CompletionExactness,
        compact_answer: Option<&mut Option<MatchAnswerID>>,
        cascade_winners_are_complete: Option<&mut bool>,
    ) -> Result<Vec<RuleMatch>, Incomplete> {
        self.state.match_element_for_purpose_with_compact_answer(
            node,
            compact_for_cascade,
            completion_exactness,
            compact_answer,
            cascade_winners_are_complete,
            &mut self.counters,
        )
    }

    /// C++ installed the record the engine derived for `node`: the winner state it was computed
    /// from becomes the node's cascade state, and the answer counts as consumed.
    #[inline]
    pub(crate) fn acknowledge_engine_computed_record(&mut self, node: StyleNodeID) {
        self.state.acknowledge_engine_computed_record(node, &mut self.counters);
    }

    /// C++ computes `node` itself rather than install what a record demand derived for it.
    #[inline]
    pub(crate) fn abandon_demanded_records(&mut self, node: StyleNodeID) {
        self.state.abandon_demanded_records(node, &mut self.counters);
    }

    /// Answer a read of one element's style the host makes before the next style update.
    #[inline]
    pub(super) fn answer_record_demand(
        &mut self,
        node: StyleNodeID,
        demand: publication::RecordDemand,
    ) -> publication::Drive<publication::RecordDemandAnswer> {
        self.state.answer_record_demand(node, demand, &mut self.counters)
    }

    /// The record of an element no rule reaches, from its own declarations alone.
    #[inline]
    pub(super) fn declared_only_record(
        &mut self,
        subject: StyleNodeID,
        facts: u32,
        declarations: &[(ElementDeclarationKind, &crate::css::declaration_block::DeclaredProperty)],
        custom_declarations: &[(CustomDeclaration, RetainedStyleValueData)],
    ) -> publication::Drive<computed::FinalStyleRecordID> {
        self.state
            .declared_only_record(subject, facts, declarations, custom_declarations, &mut self.counters)
    }

    /// Settle the pseudo-element records of an element whose record C++ just installed.
    #[inline]
    pub(crate) fn settle_pseudo_records_after_host_record(
        &mut self,
        node: StyleNodeID,
        old_is_list_item: bool,
    ) -> bridge::FfiSettledPseudoRecords {
        self.state
            .settle_pseudo_records_after_host_record(node, old_is_list_item, &mut self.counters)
    }

    /// Publish the immutable computed-group payloads of one element's base style. This assigns
    /// dense identities to shared payloads and their ordered tuple, so an equal handle proves equal
    /// groups and downstream operators can consume one node handle.
    #[inline]
    pub(crate) fn publish_computed_groups(
        &mut self,
        target: computed::ComputedStyleTarget,
        payloads: &[crate::css::host_shared::SharedPayload],
        inherited_group_count: usize,
        custom_property_environment: u64,
        metadata_input: computed::ComputedMetadataInput<'_>,
    ) -> computed::ComputedGroupPublication {
        self.state.publish_computed_groups(
            target,
            payloads,
            inherited_group_count,
            custom_property_environment,
            metadata_input,
            &mut self.counters,
        )
    }

    /// Intern the immutable computed-group payloads of a style which has no live StyleEngine target.
    #[inline]
    pub(crate) fn intern_computed_groups(
        &mut self,
        payloads: &[crate::css::host_shared::SharedPayload],
        inherited_group_count: usize,
        custom_property_environment: u64,
        metadata_input: computed::ComputedMetadataInput<'_>,
    ) -> computed::ComputedGroupPublication {
        self.state.intern_computed_groups(
            payloads,
            inherited_group_count,
            custom_property_environment,
            metadata_input,
            &mut self.counters,
        )
    }

    #[inline]
    pub(crate) fn free_style_records_kept_for_leases(&mut self) {
        self.state.free_style_records_kept_for_leases(&mut self.counters);
    }

    #[inline]
    pub(crate) fn end_style_record_view_epoch(&mut self) {
        self.state.end_style_record_view_epoch(&mut self.counters);
    }

    #[inline]
    pub(super) fn publish_animation_overlay_impl(
        &mut self,
        target: computed::ComputedStyleTarget,
        source_identity: u64,
        animated_overlay: crate::css::host_shared::HostShared<crate::css::animated_overlay::AnimatedOverlay>,
        payloads: &[crate::css::host_shared::SharedPayload],
    ) -> Option<computed::AnimationOverlayUpdate> {
        self.state.publish_animation_overlay_impl(
            target,
            source_identity,
            animated_overlay,
            payloads,
            &mut self.counters,
        )
    }

    #[inline]
    pub(crate) fn remove_computed_pseudo(
        &mut self,
        node: StyleNodeID,
        pseudo_kind: u8,
    ) -> Option<computed::FinalStyleRecordID> {
        self.state.remove_computed_pseudo(node, pseudo_kind, &mut self.counters)
    }
}
