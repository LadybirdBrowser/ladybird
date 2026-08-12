/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

impl StyleEngine {
    /// Put the qualified layer names in the order one tree scope declares them in.
    pub fn set_layer_order(&mut self, scope: TreeScopeID, layers: &[CascadeLayerID]) {
        let ranks = StyleSheetProgram::layer_ranks_in_order(layers);
        let pending_order = self.pending_layer_orders.get(&scope);
        if pending_order.map_or_else(
            || self.program.layer_order_matches(scope, layers),
            |pending| pending == &ranks,
        ) {
            return;
        }
        let initialized = pending_order.is_some() || self.program.layer_order_is_initialized(scope);
        self.pending_program_base_version.get_or_insert(self.program.version());
        self.invalidate_scope_program(scope);
        self.pending_layer_orders.insert(scope, ranks);
        if initialized {
            self.record_layer_topology_change(scope);
        }
    }

    /// The dense layer rank consumed by the C++ property cascade and non-style rule lookup.
    #[must_use]
    pub fn layer_index(&self, scope: TreeScopeID, layer: CascadeLayerID) -> u32 {
        self.pending_layer_orders.get(&scope).map_or_else(
            || self.program.layer_index(scope, layer),
            |ranks| {
                ranks
                    .get(&layer)
                    .copied()
                    .unwrap_or_else(|| u32::try_from(ranks.len()).expect("cascade layer count exhausted"))
            },
        )
    }

    pub fn set_rule_gated_by_container_query(&mut self, rule: RuleID) {
        self.stage_rule_gated_by_container_query(rule, true);
    }

    /// Record which longhand properties a rule declares, and which of them it marks important.
    #[cfg(test)]
    pub(super) fn set_rule_declared_properties(
        &mut self,
        rule: RuleID,
        declared: &[(u16, bool)],
        declarations_are_complete: bool,
    ) {
        let block = self
            .program
            .rule_version(rule)
            .declaration_block
            .map_or(u32::MAX, |block| block.0);
        let declared: Vec<DeclaredProperty> = declared
            .iter()
            .map(|&(property, important)| DeclaredProperty {
                property,
                important,
                operator: CascadeOperator::Declared,
                value: SpecifiedValueID((u64::from(block) << 32) ^ (u64::from(rule.0) << 16) ^ u64::from(property)),
            })
            .collect();
        self.record_rule_declaration_change(rule, &declared);
        self.stage_rule_declared_properties(rule, declared, declarations_are_complete);
    }

    /// Record the canonical specified value beside every declared longhand.
    #[cfg(test)]
    pub(super) fn set_rule_declared_properties_with_values(
        &mut self,
        rule: RuleID,
        declared: &[(u16, bool, SpecifiedValueID)],
        declarations_are_complete: bool,
    ) {
        let declared: Vec<DeclaredProperty> = declared
            .iter()
            .map(|&(property, important, value)| DeclaredProperty {
                property,
                important,
                operator: CascadeOperator::Declared,
                value,
            })
            .collect();
        self.record_rule_declaration_change(rule, &declared);
        self.stage_rule_declared_properties(rule, declared, declarations_are_complete);
    }

    /// Record already decoded cascade operators beside canonical specified values.
    pub fn set_rule_declared_properties_with_operators(
        &mut self,
        rule: RuleID,
        declared: &[DeclaredProperty],
        declarations_are_complete: bool,
    ) {
        self.record_rule_declaration_change(rule, declared);
        self.stage_rule_declared_properties(rule, declared.to_vec(), declarations_are_complete);
    }

    pub(super) fn stage_rule_declared_properties(
        &mut self,
        rule: RuleID,
        declared: Vec<DeclaredProperty>,
        complete: bool,
    ) {
        // A rule that did not exist in the committed program has no old declaration inventory to
        // preserve. Parser-time sheet construction is almost entirely this case, so install its
        // declarations directly instead of buffering every arriving rule twice.
        if rule.0 >= self.committed_rule_count {
            self.program.set_rule_declared_properties(rule, declared, complete);
            return;
        }
        self.pending_program_base_version.get_or_insert(self.program.version());
        self.invalidate_scope_programs_for_sheet(self.program.rule_sheet(rule));
        self.pending_rule_declarations
            .insert(rule, PendingRuleDeclarations { declared, complete });
    }

    pub(super) fn current_declared_properties_of(&self, rule: RuleID) -> &[DeclaredProperty] {
        if rule.0 >= self.committed_rule_count {
            return self.program.declared_properties_of(rule);
        }
        self.pending_rule_declarations
            .get(&rule)
            .map(|pending| pending.declared.as_slice())
            .unwrap_or_else(|| self.program.declared_properties_of(rule))
    }

    pub(super) fn current_declarations_are_complete_for(&self, rule: RuleID) -> bool {
        if rule.0 >= self.committed_rule_count {
            return self.program.declarations_are_complete_for(rule);
        }
        self.pending_rule_declarations
            .get(&rule)
            .map(|pending| pending.complete)
            .unwrap_or_else(|| self.program.declarations_are_complete_for(rule))
    }

    /// Preserve the property range of a declaration edit until its block identity is journaled.
    pub(super) fn record_rule_declaration_change(&mut self, rule: RuleID, declared: &[DeclaredProperty]) {
        let replacement = self.replacement_rule(rule);
        if replacement.is_none() && self.current_rule_version(rule).declaration_block.is_none() {
            return;
        }
        let previous = replacement
            .map(|replacement| replacement.declared.as_slice())
            .unwrap_or_else(|| self.current_declared_properties_of(rule));
        let mut old_properties: Vec<u16> = previous.iter().map(|declared| declared.property).collect();
        old_properties.sort_unstable();
        old_properties.dedup();
        let mut new_properties: Vec<u16> = declared.iter().map(|declared| declared.property).collect();
        new_properties.sort_unstable();
        new_properties.dedup();
        if let Some(change) = self
            .pending_rule_declaration_changes
            .iter_mut()
            .find(|change| change.rule == rule)
        {
            change.new_properties = new_properties;
            return;
        }
        self.pending_rule_declaration_changes
            .push(PendingRuleDeclarationChange {
                rule,
                old_properties,
                new_properties,
            });
    }

    /// Intern one immutable specified value into the rule program's dense value table.
    ///
    /// # Safety
    /// `value` must point at live `StyleValueData`.
    pub unsafe fn intern_specified_value(&mut self, value: *const StyleValueData) -> SpecifiedValueID {
        debug_assert!(!value.is_null());
        let (id, lookup) = unsafe { self.specified_values.intern(value, &mut self.memory) };
        match lookup {
            Lookup::Known(()) => self.counters.bump(Counter::SpecifiedValuesReused),
            Lookup::KnownAbsent | Lookup::Missing(_) => {}
        }
        id
    }

    pub(super) unsafe fn intern_exact_specified_value(&mut self, value: *const StyleValueData) -> SpecifiedValueID {
        debug_assert!(!value.is_null());
        unsafe { self.specified_values.intern(value, &mut self.memory).0 }
    }

    /// Register an authored spelling as an alias of its context-free canonical value.
    ///
    /// # Safety
    /// `value` must point at live `StyleValueData`.
    pub unsafe fn alias_specified_value(&mut self, value: *const StyleValueData, id: SpecifiedValueID) {
        unsafe { self.specified_values.alias(value, id, &mut self.memory) };
    }

    /// Record which cascade layer a rule sits in.
    ///
    /// Where a rule sits among the layers is part of what the rule is, and it is what the cascade
    /// compares; whether it is in one at all is what a layer topology change routes by.
    pub fn set_rule_layer(&mut self, rule: RuleID, layer: CascadeLayerID) {
        let mut version = self.current_rule_version(rule);
        if version.layer == layer {
            return;
        }
        version.layer = layer;
        self.replace_rule_version(rule, version);
    }

    pub fn set_rule_in_a_layer(&mut self, rule: RuleID) {
        self.stage_rule_in_a_layer(rule, true);
    }

    pub fn set_rule_conditions_hold(&mut self, rule: RuleID, conditions_hold: bool) {
        let previous = self
            .pending_rule_conditions
            .current(rule, || self.program.rule_conditions_hold(rule));
        if previous == conditions_hold {
            return;
        }
        self.stage_program_activation();
        self.pending_rule_conditions.stage(rule, conditions_hold);
        // While the rule's sheet is still arriving, whether its condition holds is part of what
        // arrives rather than something that moved: routing the attachment reads the activation the
        // rule ends the transaction with, and skips it if it decides nothing.
        if self.rule_change_is_carried_by_its_sheet(rule) {
            return;
        }
        self.record_input(
            InputKey::RuleField(rule, RuleField::Activation),
            InputValue::Flag(previous),
            InputValue::Flag(conditions_hold),
        );
    }

    /// Record whether a sheet's conditions hold, as evaluating its media queries decides.
    pub fn set_sheet_conditions_hold(&mut self, sheet: SheetID, conditions_hold: bool) {
        let previous = self
            .pending_sheet_conditions
            .current(sheet, || self.program.sheet_conditions_hold(sheet));
        if previous == conditions_hold {
            return;
        }
        self.stage_program_activation();
        self.pending_sheet_conditions.stage(sheet, conditions_hold);
        self.record_input(
            InputKey::SheetActivation(sheet),
            InputValue::Flag(previous),
            InputValue::Flag(conditions_hold),
        );
    }

    #[cfg(test)]
    pub(super) fn set_sheet_enabled(&mut self, sheet: SheetID, enabled: bool) {
        let previous = self
            .pending_sheet_enabled
            .current(sheet, || self.program.sheet_is_enabled(sheet));
        if previous == enabled {
            return;
        }
        self.stage_program_activation();
        self.pending_sheet_enabled.stage(sheet, enabled);
        self.record_input(
            InputKey::SheetActivation(sheet),
            InputValue::Flag(previous),
            InputValue::Flag(enabled),
        );
    }

    pub(super) fn append_rule(&mut self, sheet: SheetID, parent: Option<RuleID>, kind: RuleKind) -> RuleID {
        let rule = self.program.reserve_rule(sheet, parent, kind);
        self.stage_rule_liveness(rule, true);
        self.record_rule_existence(rule, false, true);
        rule
    }

    /// Insert one rule between two neighbours. Existing rules are neither renumbered nor rematched.
    pub(super) fn insert_rule_before(&mut self, before: RuleID, kind: RuleKind) -> RuleID {
        let rule = self.program.reserve_rule_before(before, kind);
        self.stage_rule_liveness(rule, true);
        self.record_rule_existence(rule, false, true);
        rule
    }

    /// Delete a rule, taking its subtree with it. Every removed identity is journalled, because a
    /// deleted winning declaration has to be repaired wherever it was winning.
    pub(super) fn remove_rule(&mut self, rule: RuleID) -> Vec<RuleID> {
        if !self.current_rule_is_live(rule) {
            return Vec::new();
        }
        let removed: Vec<RuleID> = self
            .program
            .rule_subtree(rule)
            .into_iter()
            .filter(|&rule| self.current_rule_is_live(rule))
            .collect();
        self.selector_programs_need_sweep |= removed
            .iter()
            .any(|&removed| self.current_rule_version(removed).selector_program.is_some());
        for &id in &removed {
            self.stage_rule_liveness(id, false);
            self.record_rule_existence(id, true, false);
        }
        removed
    }

    /// Drop every rule of a sheet.
    ///
    /// This is for `replace()`, which really does swap the whole rule list. Every other edit to a
    /// sheet moves one rule and is patched one rule at a time, because retiring identities that did
    /// not change turns a one-rule edit into a program change over the whole sheet.
    pub(super) fn clear_sheet_rules(&mut self, sheet: SheetID) {
        for rule in self.current_top_level_rules_in_sheet(sheet) {
            self.remove_rule(rule);
        }
        self.settle_program();
    }

    /// Begin rebuilding a sheet while retaining compatible style-rule identities by cascade
    /// position. The parsed CSSOM objects are new, but an unchanged semantic rule does not become a
    /// departure followed by an arrival merely because the whole sheet was reparsed.
    pub fn begin_sheet_rules_replacement(&mut self, sheet: SheetID) {
        assert!(
            self.sheet_rule_replacement.is_none(),
            "stylesheet replacements do not nest"
        );
        if let Some(slot) = self.pending_sheet_rule_replacements.get_mut(sheet.0 as usize)
            && let Some(mut replacement) = slot.take()
        {
            replacement.reused = 0;
            replacement.reuse_disabled = false;
            self.sheet_rule_replacement = Some(replacement);
            return;
        }
        let rules = self.current_rules_in_sheet(sheet);
        if rules
            .iter()
            .any(|&rule| self.current_rule_version(rule).kind != RuleKind::Style)
        {
            self.clear_sheet_rules(sheet);
            return;
        }
        let rules: Vec<ReplacedStyleRule> = rules
            .into_iter()
            .map(|rule| ReplacedStyleRule {
                rule,
                version: self.current_rule_version(rule),
                declared: self.current_declared_properties_of(rule).to_vec(),
                declarations_are_complete: self.current_declarations_are_complete_for(rule),
                gated_by_container_query: self.current_rule_is_gated_by_container_query(rule),
            })
            .collect();
        self.sheet_rule_replacement = Some(SheetRuleReplacement {
            sheet,
            rules,
            reused: 0,
            reuse_disabled: false,
        });
    }

    /// Finish a synchronous sheet rebuild, retiring unmatched old rules and publishing declaration
    /// changes on identities that were reused.
    pub fn finish_sheet_rules_replacement(&mut self, sheet: SheetID, declaration_block: u32) {
        let Some(replacement) = self.sheet_rule_replacement.take() else {
            return;
        };
        assert_eq!(replacement.sheet, sheet);
        for old in &replacement.rules[..replacement.reused] {
            let declarations_changed = !old.declarations_are_complete
                || !self.current_declarations_are_complete_for(old.rule)
                || self.current_declared_properties_of(old.rule) != old.declared
                || self.current_rule_is_gated_by_container_query(old.rule) != old.gated_by_container_query;
            if declarations_changed {
                let mut version = self.current_rule_version(old.rule);
                version.declaration_block = Some(DeclarationBlockID(declaration_block));
                self.replace_rule_version(old.rule, version);
            }
        }
        if replacement.reused < replacement.rules.len() {
            let index = sheet.0 as usize;
            self.pending_sheet_rule_replacements.insert(index, Some(replacement));
            self.settle_program();
            return;
        }
        self.settle_program();
    }

    pub(super) fn finalize_pending_sheet_rule_replacements(&mut self) {
        for index in 0..self.pending_sheet_rule_replacements.len() {
            let Some(replacement) = self.pending_sheet_rule_replacements[index].take() else {
                continue;
            };
            for old in &replacement.rules[replacement.reused..] {
                self.remove_rule(old.rule);
            }
        }
    }

    pub(super) fn replacement_rule(&self, rule: RuleID) -> Option<&ReplacedStyleRule> {
        let replacement = self.sheet_rule_replacement.as_ref()?;
        let old = replacement.rules.get(replacement.reused.checked_sub(1)?)?;
        (old.rule == rule).then_some(old)
    }

    pub(super) fn reuse_replaced_style_rule(&mut self, sheet: SheetID) -> Option<RuleID> {
        let replacement = self.sheet_rule_replacement.as_mut()?;
        if replacement.sheet != sheet || replacement.reuse_disabled {
            return None;
        }
        let old = replacement.rules.get(replacement.reused)?;
        if old.version.kind != RuleKind::Style {
            replacement.reuse_disabled = true;
            return None;
        }
        let rule = old.rule;
        let declaration_block = old.version.declaration_block;
        replacement.reused += 1;

        self.set_rule_conditions_hold(rule, true);
        self.stage_rule_declared_properties(rule, Vec::new(), false);
        self.stage_rule_in_a_layer(rule, false);
        self.stage_rule_gated_by_container_query(rule, false);
        let mut version = RuleVersion::new(rule, RuleKind::Style);
        version.declaration_block = declaration_block;
        self.replace_rule_version(rule, version);
        Some(rule)
    }

    /// Delete one rule and settle the program around it.
    pub fn remove_style_rule(&mut self, rule: RuleID) {
        self.remove_rule(rule);
        self.settle_program();
    }

    /// Replace a rule's contents and journal only the fields that actually changed.
    pub(super) fn replace_rule_version(&mut self, rule: RuleID, contents: RuleVersion) {
        let previous = self.current_rule_version(rule);
        self.stage_rule_version(rule, contents);

        if self.rule_change_is_carried_by_its_sheet(rule) {
            self.settle_program();
            return;
        }

        if previous.selector_program != contents.selector_program {
            self.record_input(
                InputKey::RuleField(rule, RuleField::Selector),
                InputValue::SelectorProgram(previous.selector_program),
                InputValue::SelectorProgram(contents.selector_program),
            );
        }
        if previous.declaration_block != contents.declaration_block {
            self.record_input(
                InputKey::RuleField(rule, RuleField::Declarations),
                InputValue::RuleDeclarations(previous.declaration_block),
                InputValue::RuleDeclarations(contents.declaration_block),
            );
        }
        if previous.activation_predicate != contents.activation_predicate {
            self.record_input(
                InputKey::RuleField(rule, RuleField::Activation),
                InputValue::ActivationPredicate(previous.activation_predicate),
                InputValue::ActivationPredicate(contents.activation_predicate),
            );
        }
        if previous.layer != contents.layer {
            self.record_input(
                InputKey::RuleField(rule, RuleField::Layer),
                InputValue::Layer(previous.layer),
                InputValue::Layer(contents.layer),
            );
        }
        if previous.scope != contents.scope {
            self.record_input(
                InputKey::RuleField(rule, RuleField::Scope),
                InputValue::Scope(previous.scope),
                InputValue::Scope(contents.scope),
            );
        }
        self.settle_program();
    }

    pub(super) fn current_rule_version(&self, rule: RuleID) -> RuleVersion {
        if rule.0 >= self.committed_rule_count {
            return self.program.rule_version(rule);
        }
        self.pending_rule_versions
            .current(rule, || self.program.rule_version(rule))
    }

    pub(super) fn stage_rule_version(&mut self, rule: RuleID, contents: RuleVersion) {
        if rule.0 >= self.committed_rule_count {
            self.program.replace_reserved_rule_version(rule, contents);
            return;
        }
        self.pending_program_base_version.get_or_insert(self.program.version());
        self.invalidate_scope_programs_for_sheet(self.program.rule_sheet(rule));
        self.pending_rule_versions.stage(rule, contents);
    }

    pub(super) fn current_rule_is_gated_by_container_query(&self, rule: RuleID) -> bool {
        if rule.0 >= self.committed_rule_count {
            return self.program.rule_is_gated_by_container_query(rule);
        }
        self.pending_rule_gated_by_container_query
            .current(rule, || self.program.rule_is_gated_by_container_query(rule))
    }

    pub(super) fn stage_rule_gated_by_container_query(&mut self, rule: RuleID, gated: bool) {
        if rule.0 >= self.committed_rule_count {
            self.program.set_rule_gated_by_container_query(rule, gated);
            return;
        }
        self.pending_program_base_version.get_or_insert(self.program.version());
        self.invalidate_scope_programs_for_sheet(self.program.rule_sheet(rule));
        self.pending_rule_gated_by_container_query.stage(rule, gated);
    }

    pub(super) fn stage_rule_in_a_layer(&mut self, rule: RuleID, in_a_layer: bool) {
        if rule.0 >= self.committed_rule_count {
            self.program.set_rule_in_a_layer(rule, in_a_layer);
            return;
        }
        self.pending_program_base_version.get_or_insert(self.program.version());
        self.invalidate_scope_programs_for_sheet(self.program.rule_sheet(rule));
        self.pending_rule_in_a_layer.stage(rule, in_a_layer);
    }

    /// A structural program edit cannot be applied while an epoch is reading the program, and it
    /// cannot be half-applied either, so it is a phase-contract violation rather than something to
    /// queue. Script cannot reach here mid-evaluation; an input that legitimately becomes ready
    /// mid-evaluation - a sheet finishing its load - is queued as a whole operation by its caller
    /// and arrives in a later transaction.
    /// The stylesheet program, for a change to it.
    ///
    /// Every scope's rule dispatch is derived from the program, so a kept one cannot outlive a
    /// change to it. Going through here rather than at the field is what makes that true by
    /// construction: a mutation that forgets to drop the dispatch does not compile.
    /// The stylesheet program, for an activation-only change to it.
    ///
    /// Whether a rule or sheet's conditions hold is match-time state: every scope dispatch keeps
    /// its structurally live rules and candidates are filtered on activation as they are
    /// consumed, so the derived scope programs and the retained prefix transition caches stay
    /// valid across the flip. Memoized prefix answers were filtered through the old activation,
    /// so those are dropped here.
    pub(super) fn stage_program_activation(&mut self) {
        self.pending_program_base_version.get_or_insert(self.program.version());
        self.prefix_caches.borrow_mut().answers.release(&mut self.match_answers);
    }

    pub(super) fn apply_pending_program_activation(&mut self) {
        let rule_conditions = self.pending_rule_conditions.take();
        let sheet_conditions = self.pending_sheet_conditions.take();
        let sheet_enabled = self.pending_sheet_enabled.take();
        if rule_conditions.is_empty() && sheet_conditions.is_empty() && sheet_enabled.is_empty() {
            return;
        }
        for (rule, conditions_hold) in rule_conditions {
            self.program.set_rule_conditions_hold(rule, conditions_hold);
        }
        for (sheet, conditions_hold) in sheet_conditions {
            self.program.set_sheet_conditions_hold(sheet, conditions_hold);
        }
        for (sheet, enabled) in sheet_enabled {
            self.program.set_sheet_enabled(sheet, enabled);
        }
        self.settle_program();
    }

    pub(super) fn apply_pending_rule_declarations(&mut self) {
        let declarations = std::mem::take(&mut self.pending_rule_declarations);
        for (rule, pending) in declarations {
            self.program
                .set_rule_declared_properties(rule, pending.declared, pending.complete);
        }
    }

    pub(super) fn apply_pending_rule_versions(&mut self) {
        let mut versions: Vec<(RuleID, RuleVersion)> = self.pending_rule_versions.take().into_iter().collect();
        if versions.is_empty() {
            return;
        }
        versions.sort_unstable_by_key(|(rule, _)| *rule);
        for (rule, version) in versions {
            self.program.replace_rule_version(rule, version);
        }
        self.settle_program();
    }

    pub(super) fn apply_pending_rule_metadata(&mut self) {
        let in_a_layer = self.pending_rule_in_a_layer.take();
        let gated_by_container_query = self.pending_rule_gated_by_container_query.take();
        for (rule, value) in in_a_layer {
            self.program.set_rule_in_a_layer(rule, value);
        }
        for (rule, value) in gated_by_container_query {
            self.program.set_rule_gated_by_container_query(rule, value);
        }
    }

    pub(super) fn current_rule_is_live(&self, rule: RuleID) -> bool {
        self.pending_rule_liveness
            .current(rule, || self.program.rule_is_live(rule))
    }

    pub(super) fn current_top_level_rules_in_sheet(&self, sheet: SheetID) -> Vec<RuleID> {
        self.program
            .all_top_level_rules_in_sheet(sheet)
            .iter()
            .copied()
            .filter(|&rule| self.current_rule_is_live(rule))
            .collect()
    }

    pub(super) fn current_rules_in_sheet(&self, sheet: SheetID) -> Vec<RuleID> {
        self.program
            .all_rules_in_sheet(sheet)
            .into_iter()
            .filter(|&rule| self.current_rule_is_live(rule))
            .collect()
    }

    pub(super) fn stage_rule_liveness(&mut self, rule: RuleID, live: bool) {
        self.pending_program_base_version.get_or_insert(self.program.version());
        if !self.rule_change_is_carried_by_its_sheet(rule) {
            self.invalidate_scope_programs_for_sheet(self.program.rule_sheet(rule));
        }
        self.pending_rule_liveness.stage(rule, live);
    }

    pub(super) fn apply_pending_rule_liveness(&mut self) {
        let mut changes: Vec<(RuleID, bool)> = self.pending_rule_liveness.take().into_iter().collect();
        if changes.is_empty() {
            return;
        }
        changes.sort_unstable_by_key(|(rule, _)| *rule);
        self.program.set_rule_liveness(&changes);
        self.settle_program();
    }

    pub(super) fn apply_pending_scope_program_metadata(&mut self) {
        let mut layer_orders: Vec<(TreeScopeID, HashMap<CascadeLayerID, u32>)> =
            std::mem::take(&mut self.pending_layer_orders).into_iter().collect();
        let mut scopes: Vec<TreeScopeID> = std::mem::take(&mut self.pending_scopes_using_document_sheets)
            .into_iter()
            .collect();
        if layer_orders.is_empty() && scopes.is_empty() {
            return;
        }
        layer_orders.sort_unstable_by_key(|(scope, _)| *scope);
        for (scope, ranks) in layer_orders {
            self.program.set_layer_ranks(scope, ranks);
        }
        scopes.sort_unstable();
        for scope in scopes {
            self.program.set_scope_uses_document_sheets(scope);
        }
        self.settle_program();
    }

    pub(super) fn current_sheets_in_scope(&self, tree_scope: TreeScopeID) -> Vec<SheetID> {
        self.pending_sheets_in_scope
            .current(tree_scope, || self.program.sheets_in_scope(tree_scope))
    }

    pub(super) fn stage_sheets_in_scope(&mut self, tree_scope: TreeScopeID, sheets: Vec<SheetID>) {
        self.rule_change_is_carried_by_sheet.clear();
        self.pending_program_base_version.get_or_insert(self.program.version());
        if tree_scope == TreeScopeID::DOCUMENT {
            // Ordinary shadow roots add only the document's non-author sheets to their local sheet
            // set. Keep their ranked programs when a document author sheet changes, while scopes
            // that explicitly consume document author sheets still follow every document change.
            let non_author_changed = self
                .current_sheets_in_scope(tree_scope)
                .into_iter()
                .filter(|&sheet| self.program.sheet_origin(sheet) != CascadeOrigin::Author)
                .ne(sheets
                    .iter()
                    .copied()
                    .filter(|&sheet| self.program.sheet_origin(sheet) != CascadeOrigin::Author));
            let scopes: Vec<_> = self
                .scope_program_by_scope
                .iter()
                .enumerate()
                .filter_map(|(index, retained)| {
                    let (_, program) = retained.as_ref()?;
                    let invalidate = index == TreeScopeID::DOCUMENT.0 as usize
                        || self.scope_program(*program).key.document_sheet_mode == DocumentSheetMode::All
                        || (non_author_changed
                            && self.scope_program(*program).key.document_sheet_mode == DocumentSheetMode::NonAuthor);
                    invalidate.then_some(TreeScopeID(
                        u32::try_from(index).expect("tree scope identity space exhausted"),
                    ))
                })
                .collect();
            for scope in scopes {
                self.invalidate_concrete_scope_program(scope);
            }
        } else {
            self.invalidate_concrete_scope_program(tree_scope);
        }
        self.pending_sheets_in_scope.stage(tree_scope, sheets);
    }

    pub(super) fn apply_pending_sheet_orders(&mut self) {
        let mut orders: Vec<(TreeScopeID, Vec<SheetID>)> = self.pending_sheets_in_scope.take().into_iter().collect();
        if orders.is_empty() {
            return;
        }
        orders.sort_unstable_by_key(|(scope, _)| *scope);
        for (scope, sheets) in orders {
            self.program.set_sheets_in_scope(scope, &sheets);
        }
        self.settle_program();
    }

    pub(super) fn record_attachment(&mut self, sheet: SheetID, tree_scope: TreeScopeID, previous: bool, current: bool) {
        let key = InputKey::SheetAttachment(sheet, tree_scope);
        // A sheet that leaves a scope and comes back to it inside one transaction is attached at both
        // ends, so its attachment says nothing - and it is exactly how a reorder is performed, since
        // assigning `adoptedStyleSheets` or moving a `style` element detaches and attaches again.
        // What moved is the sheet's place in the scope's order, which is an input of its own. Asking
        // the journal before recording is what distinguishes that from a sheet that arrived and left
        // again, whose cancellation really does mean nothing happened.
        let reattached = current && self.journal.pending_old(key) == Some(InputValue::Flag(true));
        self.record_input(key, InputValue::Flag(previous), InputValue::Flag(current));
        if reattached {
            self.record_sheet_order_change(tree_scope);
        }
        self.settle_program();
    }

    /// Make every concrete scope resolve its ranked program again. Programs whose sheet and layer
    /// dependencies are unchanged remain interned for one invalidation generation, preserving the
    /// dispatch and prefix work shared by unaffected scopes.
    pub(super) fn invalidate_scope_programs(&mut self) {
        let inactive: Vec<_> = self
            .scope_programs
            .iter()
            .enumerate()
            .filter_map(|(index, program)| {
                program
                    .as_ref()
                    .is_some_and(|program| program.scope_count == 0)
                    .then_some(ScopeProgramID(
                        u32::try_from(index).expect("scope program identity space exhausted"),
                    ))
            })
            .collect();
        for id in inactive {
            let mut caches = self.prefix_caches.borrow_mut();
            caches.states.remove(id);
            caches.answers.remove_program(&mut self.match_answers, id);
            drop(caches);
            let program = self.scope_programs[id].take().unwrap();
            self.scope_programs
                .remove_identity(intern_table::content_hash(&program.key), id);
            self.vacant_scope_programs.push(id);
        }
        for program in self.scope_programs.iter_mut().flatten() {
            program.scope_count = 0;
        }
        self.scope_program_by_scope.clear();
        self.held_scope_program = None;
    }

    /// Drop the ranked program for one tree scope while leaving equivalent scopes' shared program
    /// and prefix caches alive. Callers without the replacement document sheet order conservatively
    /// invalidate every shadow scope because they cannot distinguish author-only changes.
    pub(super) fn invalidate_scope_program(&mut self, tree_scope: TreeScopeID) {
        if tree_scope == TreeScopeID::DOCUMENT {
            self.invalidate_scope_programs();
            return;
        }
        self.invalidate_concrete_scope_program(tree_scope);
    }

    fn invalidate_concrete_scope_program(&mut self, tree_scope: TreeScopeID) {
        if self.held_scope_program.is_some_and(|(scope, _, _)| scope == tree_scope) {
            self.held_scope_program = None;
        }
        let Some((_, program)) = self
            .scope_program_by_scope
            .get_mut(tree_scope.0 as usize)
            .and_then(Option::take)
        else {
            return;
        };
        self.release_scope_program(program);
    }

    /// Drop only concrete scope programs that consume `sheet`. The key contains the effective
    /// document sheets as well as locally attached sheets, so this also reaches shadow scopes that
    /// inherit a changed document sheet without scanning the DOM's attachment topology again.
    pub(super) fn invalidate_scope_programs_for_sheet(&mut self, sheet: SheetID) {
        let scopes: Vec<_> = self
            .scope_program_by_scope
            .iter()
            .enumerate()
            .filter_map(|(index, retained)| {
                let (_, program) = retained.as_ref()?;
                self.scope_program(*program)
                    .key
                    .sheets
                    .iter()
                    .any(|&(candidate, _)| candidate == sheet)
                    .then_some(TreeScopeID(
                        u32::try_from(index).expect("tree scope identity space exhausted"),
                    ))
            })
            .collect();
        for scope in scopes {
            self.invalidate_concrete_scope_program(scope);
        }
    }

    pub(super) fn record_sheet_order_change(&mut self, tree_scope: TreeScopeID) {
        self.winner_groups.invalidate_priorities();
        let previous = self.sheet_order_version;
        self.sheet_order_version += 1;
        self.record_input(
            InputKey::CascadeTopology(TopologyAxis::SheetOrder(tree_scope)),
            InputValue::Topology(previous),
            InputValue::Topology(self.sheet_order_version),
        );
        self.settle_program();
    }

    /// Whether a change to this rule is already carried by what its sheet is doing.
    ///
    /// A sheet attaching routes every rule in it, so a rule's fields recorded while that attachment
    /// is still pending in the same transaction say nothing the attachment does not already say. A
    /// sheet attached nowhere is the same case from the other side: routing ignores its rules
    /// outright, and whatever attaches it later will route them all.
    ///
    /// This is what keeps a stylesheet arriving from journalling a field or two per rule. A page
    /// with a few thousand rules would otherwise fill the transaction's whole scratch budget on the
    /// way in, and a journal that has to coarsen loses which keys moved - which costs a restyle of
    /// the entire document, for rules whose arrival was already accounted for.
    #[must_use]
    pub(super) fn rule_change_is_carried_by_its_sheet(&mut self, rule: RuleID) -> bool {
        let sheet = self.program.rule_sheet(rule);
        if let Some(&carried) = self.rule_change_is_carried_by_sheet.get(&sheet) {
            return carried;
        }
        let mut scopes = self.program.sheet_scopes(sheet);
        scopes.extend(
            self.pending_sheets_in_scope
                .iter()
                .filter_map(|(&scope, sheets)| sheets.contains(&sheet).then_some(scope)),
        );
        scopes.sort_unstable();
        scopes.dedup();
        scopes.retain(|&scope| self.current_sheets_in_scope(scope).contains(&sheet));
        if scopes.is_empty() {
            self.rule_change_is_carried_by_sheet.insert(sheet, true);
            return true;
        }
        // Every scope it decides in has to be arriving. A sheet adopted somewhere long ago and
        // attached somewhere new this transaction still has to answer for the old scope.
        let carried = scopes.iter().all(|&tree_scope| {
            self.journal.pending_old(InputKey::SheetAttachment(sheet, tree_scope)) == Some(InputValue::Flag(false))
        });
        self.rule_change_is_carried_by_sheet.insert(sheet, carried);
        carried
    }

    pub(super) fn record_rule_existence(&mut self, rule: RuleID, previous: bool, current: bool) {
        if self.rule_change_is_carried_by_its_sheet(rule) {
            self.settle_program();
            return;
        }
        self.record_input(
            InputKey::RuleField(rule, RuleField::Existence),
            InputValue::Flag(previous),
            InputValue::Flag(current),
        );
        self.settle_program();
    }

    /// A layer topology change updates priority comparisons for declarations that already matched.
    /// It never invalidates selector truth.
    pub(super) fn record_layer_topology_change(&mut self, scope: TreeScopeID) {
        self.pending_program_base_version.get_or_insert(self.program.version());
        self.invalidate_scope_program(scope);
        self.winner_groups.invalidate_priorities();
        let previous = self.layer_topology_version;
        self.layer_topology_version += 1;
        self.record_input(
            InputKey::CascadeTopology(TopologyAxis::LayerOrder(scope)),
            InputValue::Topology(previous),
            InputValue::Topology(self.layer_topology_version),
        );
        self.settle_program();
    }

    pub(super) fn settle_program(&mut self) {
        self.program.settle_memory(&mut self.memory, &mut self.counters);
    }

    #[must_use]
    pub(super) fn fact_value_is_reconstructible(key: FeatureKey, value: FeatureValue) -> bool {
        match key {
            FeatureKey::Class(_) | FeatureKey::Part(_) | FeatureKey::CustomState(_) | FeatureKey::Emptiness => {
                matches!(value, FeatureValue::Absent | FeatureValue::Present)
            }
            FeatureKey::TagName
            | FeatureKey::FoldedTagName
            | FeatureKey::Id
            | FeatureKey::Attribute(_)
            | FeatureKey::PartExposure
            | FeatureKey::Language
            | FeatureKey::Directionality => matches!(value, FeatureValue::Absent | FeatureValue::Atom(_)),
            FeatureKey::HeadingLevel => matches!(value, FeatureValue::Number(_)),
            FeatureKey::ArrivingFacts => false,
        }
    }

    pub(super) fn input_routes_to_prefix(&self, input: &NormalizedInput) -> bool {
        let route_is_prefix = |key| {
            self.routing
                .routes_for(key)
                .iter()
                .any(|&route| self.routing.route(route).has_prefix_chain)
        };
        if routing_keys_for_input(input).into_iter().any(route_is_prefix) {
            return true;
        }
        let InputKey::LocalFeature(_, FeatureKey::Attribute(name)) = input.key else {
            return false;
        };
        self.facts
            .attribute_name_keys(name)
            .into_iter()
            .map(RoutingKey::AttributeName)
            .any(route_is_prefix)
    }

    /// Classify the local facts and relations available on both sides of this transaction.
    pub(super) fn classify_transaction_facts(
        &self,
        transaction: &StyleTransaction,
        arrival_regions: &ImpactRegions,
    ) -> TransactionFactClassification {
        let coarsened = transaction.has_coarsened_markers();
        let mut prefix_available = !coarsened;
        let retained_truth_available = !coarsened
            && transaction.inputs.iter().all(|input| {
                matches!(
                    input.key,
                    InputKey::LocalFeature(_, FeatureKey::ArrivingFacts)
                        | InputKey::LocalFeature(..)
                        | InputKey::State(..)
                        | InputKey::RuleField(_, RuleField::Activation | RuleField::Declarations | RuleField::Layer)
                        | InputKey::SheetAttachment(..)
                        | InputKey::SheetActivation(_)
                        | InputKey::CascadeTopology(_)
                        | InputKey::ElementDeclaration(..)
                        | InputKey::ElementStyleInput(..)
                ) && !matches!(input.key, InputKey::LocalFeature(_, FeatureKey::ArrivingFacts))
            });
        let mut roots = Vec::new();
        let mut departures = Vec::new();
        let mut sibling_frontier = Vec::new();
        let mut tree_relations_changed = false;
        let arrived = |node| arrival_regions.is_covered_by_subtree(ImpactRegion::Node(node), &self.tree);

        for input in &transaction.inputs {
            if prefix_available {
                match (input.key, input.old, input.new) {
                    (InputKey::TreeRelations(node), InputValue::TreeRelations(old), InputValue::TreeRelations(new)) => {
                        tree_relations_changed = true;
                        match (old, new) {
                            (None, Some(new)) => {
                                sibling_frontier.push(node);
                                sibling_frontier.extend(new.next_element_sibling);
                            }
                            (Some(old), None) => {
                                departures.push(node);
                                sibling_frontier.extend(old.next_element_sibling);
                            }
                            (None, None) => {}
                            (Some(old), Some(new)) if old.parent == new.parent && old.tree_scope == new.tree_scope => {
                                sibling_frontier.push(node);
                                sibling_frontier.extend(old.next_element_sibling);
                                sibling_frontier.extend(new.next_element_sibling);
                            }
                            _ => prefix_available = false,
                        }
                    }
                    (
                        InputKey::LocalFeature(_, FeatureKey::ArrivingFacts)
                        | InputKey::ElementDeclaration(..)
                        | InputKey::ElementStyleInput(..),
                        _,
                        _,
                    ) => {}
                    (
                        InputKey::LocalFeature(node, key),
                        InputValue::Feature(old_value),
                        InputValue::Feature(new_value),
                    ) => {
                        if !self.input_routes_to_prefix(input) {
                            continue;
                        }
                        if !Self::fact_value_is_reconstructible(key, old_value)
                            || !Self::fact_value_is_reconstructible(key, new_value)
                        {
                            prefix_available = false;
                        } else if self.tree.is_live(node)
                            && self.tree.tree_scope(node) == TreeScopeID::DOCUMENT
                            && !arrived(node)
                        {
                            roots.push(node);
                        }
                    }
                    (InputKey::State(node, _), InputValue::State(_), InputValue::State(_)) => {
                        if !self.input_routes_to_prefix(input) {
                            continue;
                        }
                        if self.tree.is_live(node)
                            && self.tree.tree_scope(node) == TreeScopeID::DOCUMENT
                            && !arrived(node)
                        {
                            roots.push(node);
                        }
                    }
                    _ => prefix_available = false,
                }
            }
        }

        let prefix = prefix_available.then(|| {
            departures.sort_unstable();
            departures.dedup();
            roots.sort_unstable();
            roots.dedup();
            sibling_frontier.retain(|&node| {
                self.tree.is_live(node)
                    && self.tree.tree_scope(node) == TreeScopeID::DOCUMENT
                    && departures.binary_search(&node).is_err()
            });
            sibling_frontier.sort_unstable();
            sibling_frontier.dedup();
            PrefixFactTransition {
                roots,
                departures,
                tree_relations_changed,
                sibling_frontier,
            }
        });
        TransactionFactClassification {
            prefix,
            retained_truth_available,
        }
    }

    pub(super) fn transaction_fact_view_for(
        &mut self,
        transaction: &mut StyleTransaction,
        root: StyleNodeID,
        arrival_regions: &ImpactRegions,
    ) -> TransactionFactView {
        let plan_before_commit = self.facts.has_pending_input();
        let classification = self.classify_transaction_facts(transaction, arrival_regions);
        let before_facts = transaction.take_before_facts(&mut self.memory);
        let local_facts_are_shared =
            !plan_before_commit && before_facts.is_none() && !transaction.has_coarsened_markers();
        let before = (!plan_before_commit && !local_facts_are_shared && !transaction.has_coarsened_markers())
            .then(|| before_facts.expect("a changed local fact transaction retained its old side"));
        let after = plan_before_commit.then(|| self.facts.pending_facts());
        TransactionFactView {
            root,
            moved_features: if !transaction.has_coarsened_markers() {
                self.feature_delta_for(transaction)
            } else {
                FeatureFluxColumn::default()
            },
            before,
            after,
            before_sibling_geometry: SiblingSequenceGeometry::default(),
            before_sibling_sequence_by_parent: Vec::new(),
            before_sibling_parents_by_sequence: Vec::new(),
            before_absent_nodes: Vec::new(),
            before_sibling_relations_available: false,
            prefix: classification.prefix,
            retained_truth_available: classification.retained_truth_available,
            resident_side: if plan_before_commit {
                TransactionFactSide::Before
            } else {
                TransactionFactSide::After
            },
            local_facts_are_shared,
            opposite_fully_materialized: false,
        }
    }

    /// The fact arrangement resident on the side from which this planning epoch started.
    pub(super) fn planning_facts(&self) -> &StyleNodeFacts {
        if self
            .transaction_fact_view
            .as_ref()
            .is_some_and(|view| view.resident_side == TransactionFactSide::Before)
        {
            self.facts.committed()
        } else {
            self.facts.primary()
        }
    }

    /// Take the pending transaction for the style consumer that immediately follows this call.
    ///
    /// Exact planning may already have packed the current facts and built document-order
    /// coordinates. Keep that Tier-4 workspace for matching instead of reconstructing it. The
    /// callback receives only accepted match-answer records after their complete payloads have been
    /// published. Versions are shared by the whole emitted batch rather than repeated per record.
    #[cfg(test)]
    pub(super) fn take_style_transaction_nodes(&mut self, root: StyleNodeID, mut emit: impl FnMut(&[u32])) -> bool {
        assert!(self.diagnostic_plan_capture.is_none());
        self.diagnostic_plan_capture = Some(DiagnosticPlanCapture {
            nodes: Vec::new(),
            scoped: true,
        });
        let _ = self.take_style_transaction(root, |_, _, _| {});
        let capture = self
            .diagnostic_plan_capture
            .take()
            .expect("the diagnostic plan capture is active during the transaction");
        self.discard_style_transaction_outputs();
        if !capture.nodes.is_empty() {
            emit(&capture.nodes);
        }
        capture.scoped
    }

    pub(super) fn discard_style_transaction_outputs(&mut self) {
        self.retain_prefix_states();
        self.discard_prepared_batch_matching_traversal();
        self.discard_published_match_answers();
    }
}
