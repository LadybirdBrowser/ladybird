/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The rule edits a transaction stages, which the rule store takes at the transaction barrier.

use super::super::super::*;
use super::RuleDeclarations;
use crate::css::declaration_block::SharedDeclarationBlock;

#[derive(Clone)]
struct StagedFieldRow<V> {
    before: V,
    after: V,
    dirty: bool,
}

/// Sparse staging for one program field. The first write freezes `before`, later writes replace
/// `after`, and `take_dirty()` applies only the last write while retaining both sides for
/// `ProgramStaging::delta()`.
#[derive(Clone)]
struct StagedField<K, V> {
    rows: HashMap<K, StagedFieldRow<V>>,
    touched: Vec<K>,
    dirty_count: usize,
}

impl<K, V> Default for StagedField<K, V> {
    fn default() -> Self {
        Self {
            rows: HashMap::default(),
            touched: Vec::new(),
            dirty_count: 0,
        }
    }
}

impl<K: Copy + Eq + Hash + Ord, V: Clone> StagedField<K, V> {
    fn current(&self, key: K, committed: impl FnOnce() -> V) -> V {
        self.side(key, TransactionFactSide::After, committed)
    }

    fn after(&self, key: K) -> Option<&V> {
        self.rows.get(&key).map(|row| &row.after)
    }

    fn side(&self, key: K, side: TransactionFactSide, resident: impl FnOnce() -> V) -> V {
        let row = self.rows.get(&key);
        match (side, row) {
            (TransactionFactSide::Before, Some(row)) => row.before.clone(),
            (TransactionFactSide::After, Some(row)) => row.after.clone(),
            _ => resident(),
        }
    }

    fn stage(&mut self, key: K, before: impl FnOnce() -> V, after: V) {
        match self.rows.entry(key) {
            std::collections::hash_map::Entry::Occupied(mut entry) => {
                let row = entry.get_mut();
                row.after = after;
                if !row.dirty {
                    row.dirty = true;
                    self.dirty_count += 1;
                }
            }
            std::collections::hash_map::Entry::Vacant(entry) => {
                entry.insert(StagedFieldRow {
                    before: before(),
                    after,
                    dirty: true,
                });
                self.touched.push(key);
                self.dirty_count += 1;
            }
        }
    }

    fn take_dirty(&mut self) -> Vec<(K, V)> {
        if self.is_empty() {
            return Vec::new();
        }
        let mut dirty = Vec::with_capacity(self.dirty_count);
        for &key in &self.touched {
            let row = self.rows.get_mut(&key).unwrap();
            if row.dirty {
                dirty.push((key, row.after.clone()));
                row.dirty = false;
            }
        }
        self.dirty_count = 0;
        dirty.sort_unstable_by_key(|(key, _)| *key);
        dirty
    }

    fn is_empty(&self) -> bool {
        self.dirty_count == 0
    }

    pub(in crate::css::style) fn clear(&mut self) {
        self.rows.clear();
        self.touched.clear();
        // Keep small edit buffers warm, but do not retain stylesheet-loading capacity for the
        // lifetime of every document. These rows are only needed until transaction release.
        if self.rows.capacity() * size_of::<(K, StagedFieldRow<V>)>() > 4096 {
            self.rows = HashMap::default();
        }
        if self.touched.capacity() * size_of::<K>() > 4096 {
            self.touched = Vec::new();
        }
        self.dirty_count = 0;
    }

    fn pairs(&self) -> impl Iterator<Item = (K, &V, &V)> {
        self.touched.iter().map(|&key| {
            let row = self.rows.get(&key).unwrap();
            (key, &row.before, &row.after)
        })
    }
}

/// Program transaction staging. Every field preserves its first before value and last-writer
/// after value until release; `delta()` may read those pairs after the final values are applied.
#[derive(Clone, Default)]
pub(in crate::css::style) struct ProgramStaging {
    rule_conditions: StagedField<RuleID, bool>,
    sheet_conditions: StagedField<SheetID, bool>,
    sheet_enabled: StagedField<SheetID, bool>,
    rule_declarations: StagedField<RuleID, RuleDeclarations>,
    rule_versions: StagedField<RuleID, RuleVersion>,
    rule_in_a_layer: StagedField<RuleID, bool>,
    rule_gated_by_container_query: StagedField<RuleID, bool>,
    rule_liveness: StagedField<RuleID, bool>,
    layer_orders: StagedField<TreeScopeID, HashMap<CascadeLayerID, u32>>,
    scopes_using_document_sheets: StagedField<TreeScopeID, bool>,
    sheets_in_scope: StagedField<TreeScopeID, Vec<SheetID>>,
    sheet_rule_replacements: Column<Option<SheetRuleReplacement>>,
    /// Whether a registration made through `CSS.registerProperty()` waits for the next
    /// transaction, which says what moved of the registrations as it takes the registry in.
    registration_pending: bool,
    /// The names whose registration the registry the next transaction reads holds otherwise than
    /// the one the last transaction read.
    moved_registrations: Vec<ak::Utf16FlyString>,
}

impl ProgramStaging {
    pub(in crate::css::style) fn is_dirty(&self) -> bool {
        !self.rule_conditions.is_empty()
            || !self.sheet_conditions.is_empty()
            || !self.sheet_enabled.is_empty()
            || !self.rule_declarations.is_empty()
            || !self.rule_versions.is_empty()
            || !self.rule_in_a_layer.is_empty()
            || !self.rule_gated_by_container_query.is_empty()
            || !self.rule_liveness.is_empty()
            || !self.layer_orders.is_empty()
            || !self.scopes_using_document_sheets.is_empty()
            || !self.sheets_in_scope.is_empty()
            || self.sheet_rule_replacements.iter().any(Option::is_some)
            || self.registration_pending
            || !self.moved_registrations.is_empty()
    }

    /// Note a registration made through `CSS.registerProperty()`, which the registry the next
    /// transaction reads holds.
    pub(in crate::css::style) fn note_registration_change(&mut self) {
        self.registration_pending = true;
    }

    /// Note what moved between the registry the last transaction read, `previous`, and the one
    /// the next reads, `registry`.
    pub(in crate::css::style) fn note_registry_moved(
        &mut self,
        previous: &crate::css::custom_properties::CustomPropertyRegistry,
        registry: &crate::css::custom_properties::CustomPropertyRegistry,
    ) {
        for name in registry.names_registered_otherwise_than(previous) {
            if !self.moved_registrations.contains(&name) {
                self.moved_registrations.push(name);
            }
        }
    }

    pub(in crate::css::style) fn clear(&mut self) {
        self.rule_conditions.clear();
        self.sheet_conditions.clear();
        self.sheet_enabled.clear();
        self.rule_declarations.clear();
        self.rule_versions.clear();
        self.rule_in_a_layer.clear();
        self.rule_gated_by_container_query.clear();
        self.rule_liveness.clear();
        self.layer_orders.clear();
        self.scopes_using_document_sheets.clear();
        self.sheets_in_scope.clear();
        for index in 0..self.sheet_rule_replacements.len() {
            self.sheet_rule_replacements[index] = None;
        }
    }
}

#[derive(Clone)]
pub(in crate::css::style) struct ReplacedStyleRule {
    pub(in crate::css::style) rule: RuleID,
    pub(in crate::css::style) version: RuleVersion,
    pub(in crate::css::style) declared: Vec<DeclarationEntry>,
    pub(in crate::css::style) declarations_are_complete: bool,
    pub(in crate::css::style) gated_by_container_query: bool,
}

/// What a transaction changed of the rules, read off the program as it stood before the transaction
/// and as the transaction leaves it.
#[derive(Default)]
pub(crate) struct RuleChanges {
    /// The rules whose contents, conditions, layering or existence moved. A rule of a sheet that was
    /// attached nowhere before the transaction is left out: what it decides arrives with the sheet.
    pub(crate) rules: Vec<RuleID>,
    /// Those of `rules` where more than their conditions moved, whose declarations and gates may
    /// not be the ones their entries were indexed with.
    pub(crate) rules_with_moved_contents: Vec<RuleID>,
    /// The sheets that were attached to or detached from a scope, or whose activation moved.
    pub(crate) sheets: Vec<SheetID>,
    /// The scopes whose sheet order or layer order moved, which reorders declarations that already
    /// matched without changing which elements they match.
    pub(crate) reordered_scopes: Vec<TreeScopeID>,
    /// The scopes whose layer order moved, which moves the layer ranks of their rules, whether or
    /// not it reorders any of them.
    pub(crate) layered_scopes: Vec<TreeScopeID>,
    /// The custom property names whose registration moved, through an `@property` rule or
    /// `CSS.registerProperty()`.
    pub(crate) registrations: Vec<ak::Utf16FlyString>,
}

impl RuleChanges {
    pub(crate) fn is_empty(&self) -> bool {
        self.rules.is_empty()
            && self.sheets.is_empty()
            && self.reordered_scopes.is_empty()
            && self.layered_scopes.is_empty()
            && self.registrations.is_empty()
    }
}

#[derive(Clone)]
pub(in crate::css::style) struct SheetRuleReplacement {
    pub(in crate::css::style) sheet: SheetID,
    pub(in crate::css::style) rules: Vec<ReplacedStyleRule>,
    pub(in crate::css::style) reused: usize,
    pub(in crate::css::style) reuse_disabled: bool,
}

impl StyleEngine {
    /// Record that a tree scope decides with the document's author sheets as well as its own.
    pub fn set_tree_scope_uses_document_sheets(&mut self, tree_scope: TreeScopeID) {
        let previous = self
            .host
            .program_staging
            .scopes_using_document_sheets
            .current(tree_scope, || {
                self.retained.rules.scope_uses_document_sheets(tree_scope)
            });
        if previous {
            return;
        }
        self.host.program_staging.scopes_using_document_sheets.stage(
            tree_scope,
            || self.retained.rules.scope_uses_document_sheets(tree_scope),
            true,
        );
    }

    /// Put the qualified layer names in the order one tree scope declares them in.
    pub fn set_layer_order(&mut self, scope: TreeScopeID, layers: &[CascadeLayerID]) {
        let ranks = RuleStore::layer_ranks_in_order(layers);
        let pending_order = self.host.program_staging.layer_orders.after(scope);
        if pending_order.map_or_else(
            || self.retained.rules.layer_ranks_match(scope, &ranks),
            |pending| pending == &ranks,
        ) {
            return;
        }
        self.host
            .program_staging
            .layer_orders
            .stage(scope, || self.retained.rules.layer_ranks(scope), ranks);
    }

    /// The dense layer rank consumed by the C++ property cascade and non-style rule lookup.
    #[must_use]
    pub fn layer_index(&self, scope: TreeScopeID, layer: CascadeLayerID) -> u32 {
        self.host.program_staging.layer_orders.after(scope).map_or_else(
            || self.retained.rules.layer_index(scope, layer),
            |ranks| {
                ranks
                    .get(&layer)
                    .copied()
                    .unwrap_or_else(|| u32::try_from(ranks.len()).expect("cascade layer count exhausted"))
            },
        )
    }

    /// The declaration block `rule` holds, as this transaction staged it.
    pub(in crate::css::style) fn current_rule_block(&self, rule: RuleID) -> Option<SharedDeclarationBlock> {
        match self.host.program_staging.rule_declarations.after(rule) {
            Some(staged) => staged.block.clone(),
            None => self.retained.rules.block_of(rule).cloned(),
        }
    }

    pub(in crate::css::style) fn stage_rule_declarations(&mut self, rule: RuleID, declarations: RuleDeclarations) {
        // NB: As for its version, a rule this transaction reserved takes its declarations in place.
        if self.staged_rule_is_arriving(rule) && self.host.program_staging.rule_declarations.after(rule).is_none() {
            self.retained.rules.set_rule_declarations(rule, declarations);
            return;
        }
        self.host.program_staging.rule_declarations.stage(
            rule,
            || self.retained.rules.rule_declarations_of(rule),
            declarations,
        );
    }

    pub(in crate::css::style) fn current_declared_properties_of(&self, rule: RuleID) -> &[DeclarationEntry] {
        self.host
            .program_staging
            .rule_declarations
            .after(rule)
            .map(|pending| pending.declared_properties())
            .unwrap_or_else(|| self.retained.rules.declared_properties_of(rule))
    }

    pub(in crate::css::style) fn current_declarations_are_complete_for(&self, rule: RuleID) -> bool {
        self.host
            .program_staging
            .rule_declarations
            .after(rule)
            .map(|pending| pending.custom_declarations().is_empty())
            .unwrap_or_else(|| self.retained.rules.declarations_are_complete_for(rule))
    }

    pub fn set_rule_conditions_hold(&mut self, rule: RuleID, conditions_hold: bool) {
        let previous = self
            .host
            .program_staging
            .rule_conditions
            .current(rule, || self.retained.rules.rule_conditions_hold(rule));
        if previous == conditions_hold {
            return;
        }
        self.host.program_staging.rule_conditions.stage(
            rule,
            || self.retained.rules.rule_conditions_hold(rule),
            conditions_hold,
        );
    }

    /// Record whether a sheet's conditions hold, as evaluating its media queries decides.
    pub fn set_sheet_conditions_hold(&mut self, sheet: SheetID, conditions_hold: bool) {
        let previous = self
            .host
            .program_staging
            .sheet_conditions
            .current(sheet, || self.retained.rules.sheet_conditions_hold(sheet));
        if previous == conditions_hold {
            return;
        }
        self.host.program_staging.sheet_conditions.stage(
            sheet,
            || self.retained.rules.sheet_conditions_hold(sheet),
            conditions_hold,
        );
    }

    #[cfg(test)]
    pub(in crate::css::style) fn set_sheet_enabled(&mut self, sheet: SheetID, enabled: bool) {
        let previous = self
            .host
            .program_staging
            .sheet_enabled
            .current(sheet, || self.retained.rules.sheet_is_enabled(sheet));
        if previous == enabled {
            return;
        }
        self.host
            .program_staging
            .sheet_enabled
            .stage(sheet, || self.retained.rules.sheet_is_enabled(sheet), enabled);
    }

    /// Begin rebuilding a sheet while retaining compatible style-rule identities by cascade
    /// position. The parsed CSSOM objects are new, but an unchanged semantic rule does not become a
    /// departure followed by an arrival merely because the whole sheet was reparsed.
    pub fn begin_sheet_rules_replacement(&mut self, sheet: SheetID) {
        assert!(
            self.host.sheet_rule_replacement.is_none(),
            "stylesheet replacements do not nest"
        );
        if let Some(slot) = self
            .host
            .program_staging
            .sheet_rule_replacements
            .get_mut(sheet.0 as usize)
            && let Some(mut replacement) = slot.take()
        {
            replacement.reused = 0;
            replacement.reuse_disabled = false;
            self.host.sheet_rule_replacement = Some(replacement);
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
        self.host.sheet_rule_replacement = Some(SheetRuleReplacement {
            sheet,
            rules,
            reused: 0,
            reuse_disabled: false,
        });
    }

    /// Finish a synchronous sheet rebuild, retiring unmatched old rules and publishing declaration
    /// changes on identities that were reused.
    pub fn finish_sheet_rules_replacement(&mut self, sheet: SheetID, declaration_block: u32) {
        let Some(replacement) = self.host.sheet_rule_replacement.take() else {
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
                version.declaration_block = Some(DeclarationBlockID::new(declaration_block));
                self.replace_rule_version(old.rule, version);
            }
        }
        if replacement.reused < replacement.rules.len() {
            let index = sheet.0 as usize;
            self.host
                .program_staging
                .sheet_rule_replacements
                .insert(index, Some(replacement));
        }
    }

    pub(in crate::css::style) fn finalize_staged_sheet_rule_replacements(&mut self) {
        for index in 0..self.host.program_staging.sheet_rule_replacements.len() {
            let Some(replacement) = self.host.program_staging.sheet_rule_replacements[index].take() else {
                continue;
            };
            for old in &replacement.rules[replacement.reused..] {
                self.remove_rule(old.rule);
            }
        }
    }

    pub(in crate::css::style) fn reuse_replaced_style_rule(&mut self, sheet: SheetID) -> Option<RuleID> {
        let replacement = self.host.sheet_rule_replacement.as_mut()?;
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
        self.stage_rule_declarations(rule, RuleDeclarations::default());
        self.stage_rule_in_a_layer(rule, false);
        self.stage_rule_gated_by_container_query(rule, false);
        let mut version = RuleVersion::new(RuleKind::Style);
        version.declaration_block = declaration_block;
        self.replace_rule_version(rule, version);
        Some(rule)
    }

    pub(in crate::css::style) fn current_rule_version(&self, rule: RuleID) -> RuleVersion {
        self.host
            .program_staging
            .rule_versions
            .current(rule, || self.retained.rules.rule_version(rule).clone())
    }

    pub(in crate::css::style) fn stage_rule_version(&mut self, rule: RuleID, contents: RuleVersion) {
        // NB: A rule this transaction reserved has no committed version anything reads, and its
        //     arrival already names it a changed rule. Its version is written in place rather than
        //     staged with a before side nothing asks for, which for a sheet arriving whole would
        //     hold two versions of every one of its rules until the transaction is released.
        if self.staged_rule_is_arriving(rule) && self.host.program_staging.rule_versions.after(rule).is_none() {
            self.retained.rules.replace_reserved_rule_version(rule, contents);
            return;
        }
        self.host.program_staging.rule_versions.stage(
            rule,
            || self.retained.rules.rule_version(rule).clone(),
            contents,
        );
    }

    pub(in crate::css::style) fn current_rule_is_gated_by_container_query(&self, rule: RuleID) -> bool {
        self.host
            .program_staging
            .rule_gated_by_container_query
            .current(rule, || self.retained.rules.rule_is_gated_by_container_query(rule))
    }

    pub(in crate::css::style) fn stage_rule_gated_by_container_query(&mut self, rule: RuleID, gated: bool) {
        self.host.program_staging.rule_gated_by_container_query.stage(
            rule,
            || self.retained.rules.rule_is_gated_by_container_query(rule),
            gated,
        );
    }

    pub(in crate::css::style) fn stage_rule_in_a_layer(&mut self, rule: RuleID, in_a_layer: bool) {
        self.host.program_staging.rule_in_a_layer.stage(
            rule,
            || self.retained.rules.rule_is_in_a_layer(rule),
            in_a_layer,
        );
    }

    /// What the staged program changes of the committed one, which has yet to take it.
    fn staged_rule_changes(&self) -> RuleChanges {
        fn moved<K: Copy, V: PartialEq>(pairs: impl Iterator<Item = (K, V, V)>) -> impl Iterator<Item = K> {
            pairs.filter_map(|(key, before, after)| (before != after).then_some(key))
        }
        let staging = &self.host.program_staging;
        let program = &self.retained.rules;
        let mut changes = RuleChanges::default();

        let attached = |rule: &RuleID| !program.sheet_scopes(program.rule_sheet(*rule)).is_empty();
        changes.rules_with_moved_contents = moved(staging.rule_declarations.pairs())
            .chain(moved(staging.rule_versions.pairs()))
            .chain(moved(staging.rule_in_a_layer.pairs()))
            .chain(moved(staging.rule_gated_by_container_query.pairs()))
            .chain(moved(staging.rule_liveness.pairs()))
            .filter(attached)
            .collect();
        changes.rules_with_moved_contents.sort_unstable();
        changes.rules_with_moved_contents.dedup();
        let mut rules: Vec<RuleID> = moved(staging.rule_conditions.pairs())
            .filter(attached)
            .chain(changes.rules_with_moved_contents.iter().copied())
            .collect();
        rules.sort_unstable();
        rules.dedup();
        changes.rules = rules;

        changes.sheets = moved(staging.sheet_conditions.pairs())
            .chain(moved(staging.sheet_enabled.pairs()))
            .collect();
        for (scope, before, after) in staging.sheets_in_scope.pairs() {
            let kept_before = before.iter().filter(|sheet| after.contains(sheet));
            let kept_after = after.iter().filter(|sheet| before.contains(sheet));
            if kept_before.ne(kept_after) {
                changes.reordered_scopes.push(scope);
            }
            changes.sheets.extend(
                before
                    .iter()
                    .filter(|sheet| !after.contains(sheet))
                    .chain(after.iter().filter(|sheet| !before.contains(sheet))),
            );
        }
        changes.sheets.sort_unstable();
        changes.sheets.dedup();

        changes.layered_scopes = moved(staging.layer_orders.pairs()).collect();
        // NB: A scope's first layer order places no layered rule that was there before it.
        changes
            .reordered_scopes
            .extend(moved(staging.layer_orders.pairs()).filter(|&scope| program.layer_order_is_initialized(scope)));
        changes.reordered_scopes.sort_unstable();
        changes.reordered_scopes.dedup();
        changes
    }

    pub(in crate::css::style) fn commit_staged_program(&mut self) -> RuleChanges {
        let mut changes = self.staged_rule_changes();
        self.host.program_staging.registration_pending = false;
        changes.registrations = std::mem::take(&mut self.host.program_staging.moved_registrations);
        self.retained
            .counters
            .add(Counter::CascadeTopologyDeltas, changes.reordered_scopes.len() as u64);
        let rule_conditions = self.host.program_staging.rule_conditions.take_dirty();
        let sheet_conditions = self.host.program_staging.sheet_conditions.take_dirty();
        let sheet_enabled = self.host.program_staging.sheet_enabled.take_dirty();
        if !rule_conditions.is_empty() || !sheet_conditions.is_empty() || !sheet_enabled.is_empty() {
            for (rule, conditions_hold) in rule_conditions {
                self.retained.rules.set_rule_conditions_hold(rule, conditions_hold);
            }
            for (sheet, conditions_hold) in sheet_conditions {
                self.retained.rules.set_sheet_conditions_hold(sheet, conditions_hold);
            }
            for (sheet, enabled) in sheet_enabled {
                self.retained.rules.set_sheet_enabled(sheet, enabled);
            }
        }

        let declarations = self.host.program_staging.rule_declarations.take_dirty();
        for (rule, declarations) in declarations {
            self.retained.rules.set_rule_declarations(rule, declarations);
        }

        let mut versions = self.host.program_staging.rule_versions.take_dirty();
        if !versions.is_empty() {
            versions.sort_unstable_by_key(|(rule, _)| *rule);
            for (rule, version) in versions {
                if self.staged_rule_is_arriving(rule) {
                    self.retained.rules.replace_reserved_rule_version(rule, version);
                } else {
                    self.retained.rules.replace_rule_version(rule, version);
                }
            }
        }

        let in_a_layer = self.host.program_staging.rule_in_a_layer.take_dirty();
        let gated_by_container_query = self.host.program_staging.rule_gated_by_container_query.take_dirty();
        for (rule, value) in in_a_layer {
            self.retained.rules.set_rule_in_a_layer(rule, value);
        }
        for (rule, value) in gated_by_container_query {
            self.retained.rules.set_rule_gated_by_container_query(rule, value);
        }

        let mut liveness_changes = self.host.program_staging.rule_liveness.take_dirty();
        if !liveness_changes.is_empty() {
            liveness_changes.sort_unstable_by_key(|(rule, _)| *rule);
            self.retained.rules.set_rule_liveness(&liveness_changes);
        }

        let layer_orders = self.host.program_staging.layer_orders.take_dirty();
        let scopes = self.host.program_staging.scopes_using_document_sheets.take_dirty();
        if !layer_orders.is_empty() || !scopes.is_empty() {
            for (scope, ranks) in layer_orders {
                self.retained.rules.set_layer_ranks(scope, ranks);
            }
            for (scope, uses_document_sheets) in scopes {
                if uses_document_sheets {
                    self.retained.rules.set_scope_uses_document_sheets(scope);
                }
            }
        }

        let mut sheet_orders = self.host.program_staging.sheets_in_scope.take_dirty();
        if !sheet_orders.is_empty() {
            sheet_orders.sort_unstable_by_key(|(scope, _)| *scope);
            for (scope, sheets) in sheet_orders {
                self.retained.rules.set_sheets_in_scope(scope, sheets);
            }
        }
        changes
    }

    pub(in crate::css::style) fn current_rule_is_live(&self, rule: RuleID) -> bool {
        self.host
            .program_staging
            .rule_liveness
            .current(rule, || self.retained.rules.rule_is_live(rule))
    }

    fn staged_rule_is_arriving(&self, rule: RuleID) -> bool {
        !self
            .host
            .program_staging
            .rule_liveness
            .side(rule, TransactionFactSide::Before, || {
                self.retained.rules.rule_is_live(rule)
            })
            && self.current_rule_is_live(rule)
    }

    pub(in crate::css::style) fn stage_rule_liveness(&mut self, rule: RuleID, live: bool) {
        self.host
            .program_staging
            .rule_liveness
            .stage(rule, || self.retained.rules.rule_is_live(rule), live);
    }

    pub(in crate::css::style) fn current_sheets_in_scope(&self, tree_scope: TreeScopeID) -> &[SheetID] {
        self.host
            .program_staging
            .sheets_in_scope
            .after(tree_scope)
            .map_or_else(|| self.retained.rules.sheets_in_scope(tree_scope), Vec::as_slice)
    }

    pub(in crate::css::style) fn stage_sheets_in_scope(&mut self, tree_scope: TreeScopeID, sheets: Vec<SheetID>) {
        self.host.program_staging.sheets_in_scope.stage(
            tree_scope,
            || self.retained.rules.sheets_in_scope(tree_scope).to_vec(),
            sheets,
        );
    }
}

impl StyleEngine {
    pub fn set_rule_gated_by_container_query(&mut self, rule: RuleID) {
        self.stage_rule_gated_by_container_query(rule, true);
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

    pub(in crate::css::style) fn append_rule(
        &mut self,
        sheet: SheetID,
        parent: Option<RuleID>,
        kind: RuleKind,
    ) -> RuleID {
        let rule = self.retained.rules.reserve_rule(sheet, parent, kind);
        self.stage_rule_liveness(rule, true);
        rule
    }

    /// Insert one rule between two neighbors. Existing rules are neither renumbered nor rematched.
    pub(in crate::css::style) fn insert_rule_before(&mut self, before: RuleID, kind: RuleKind) -> RuleID {
        let rule = self.retained.rules.reserve_rule_before(before, kind);
        self.stage_rule_liveness(rule, true);
        rule
    }

    /// Delete a rule, taking its subtree with it. Every removed identity is a rule change, because a
    /// deleted winning declaration has to be repaired wherever it was winning.
    pub(in crate::css::style) fn remove_rule(&mut self, rule: RuleID) -> Vec<RuleID> {
        if !self.current_rule_is_live(rule) {
            return Vec::new();
        }
        let removed: Vec<RuleID> = self
            .rules
            .rule_subtree(rule)
            .into_iter()
            .filter(|&rule| self.current_rule_is_live(rule))
            .collect();
        for &id in &removed {
            self.retained.rules.native.remove(id);
            self.stage_rule_liveness(id, false);
        }
        removed
    }

    /// Replace a rule's contents.
    pub(in crate::css::style) fn replace_rule_version(&mut self, rule: RuleID, contents: RuleVersion) {
        self.stage_rule_version(rule, contents);
    }

    pub(in crate::css::style) fn current_top_level_rules_in_sheet(&self, sheet: SheetID) -> Vec<RuleID> {
        self.retained
            .rules
            .all_top_level_rules_in_sheet(sheet)
            .iter()
            .copied()
            .filter(|&rule| self.current_rule_is_live(rule))
            .collect()
    }

    pub(in crate::css::style) fn current_rules_in_sheet(&self, sheet: SheetID) -> Vec<RuleID> {
        self.retained
            .rules
            .all_rules_in_sheet(sheet)
            .into_iter()
            .filter(|&rule| self.current_rule_is_live(rule))
            .collect()
    }

    /// Answers the identities the end of the transaction released, for the host to mint again.
    pub(in crate::css::style) fn discard_style_update_outputs(&mut self) -> Vec<u32> {
        self.retained.tree.release_retired_identities()
    }

    /// Releases the identities the end of the transaction released, which the job pays the host to mint again.
    pub(in crate::css::style) fn release_style_update_identities(&mut self) {
        let released = self.discard_style_update_outputs();
        self.host.released_style_nodes.extend(released);
    }

    pub(crate) fn take_released_style_nodes(&mut self) -> Vec<u32> {
        std::mem::take(&mut self.host.released_style_nodes)
    }
}

impl StyleEngine {
    /// Drop every rule of a sheet.
    ///
    /// This is for `replace()`, which really does swap the whole rule list. Every other edit to a
    /// sheet moves one rule and is patched one rule at a time, because retiring identities that did
    /// not change turns a one-rule edit into a program change over the whole sheet.
    pub(in crate::css::style) fn clear_sheet_rules(&mut self, sheet: SheetID) {
        for rule in self.current_top_level_rules_in_sheet(sheet) {
            self.remove_rule(rule);
        }
    }

    /// Delete one rule and settle the program around it.
    pub fn remove_style_rule(&mut self, rule: RuleID) {
        self.remove_rule(rule);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sparse_program_staging_freezes_before_until_release() {
        let rule = RuleID(7);
        let mut staged = StagedField::default();
        staged.stage(rule, || 10_u32, 20);
        staged.stage(rule, || panic!("the before value is already frozen"), 30);

        assert_eq!(staged.side(rule, TransactionFactSide::Before, || 0), 10);
        assert_eq!(staged.current(rule, || 0), 30);
        assert_eq!(staged.take_dirty(), [(rule, 30)]);
        assert!(staged.is_empty());
        assert_eq!(staged.side(rule, TransactionFactSide::Before, || 0), 10);
        assert_eq!(staged.current(rule, || 35), 30);

        staged.stage(rule, || panic!("the before value is already frozen"), 40);
        assert_eq!(staged.side(rule, TransactionFactSide::Before, || 0), 10);
        assert_eq!(staged.take_dirty(), [(rule, 40)]);
        assert_eq!(staged.current(rule, || 50), 40);

        staged.clear();
        assert_eq!(staged.current(rule, || 50), 50);
        assert!(staged.rows.is_empty());
        assert!(staged.touched.is_empty());
    }
}
