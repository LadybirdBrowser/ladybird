/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What every style engine of the process shares: the style atoms and the data built from the
//! user agent's sheets, which every document records alike.

use super::atoms::ProcessAtoms;
use super::kept_past_release::KeptPastRelease;
use super::pipeline::rule_store::{RuleDeclarations, RuleID, UserAgentRules};
use super::*;
use crate::css::declaration_block::SharedDeclarationBlock;

/// The style data of the process, which the render owner holds and hands each style engine it
/// makes.
pub(crate) struct ProcessStyle {
    /// The atoms every engine and document host of the process interns names in.
    pub(super) atoms: Arc<ProcessAtoms>,
    /// The serial the next selector arena of the process takes.
    selector_arena_serials: std::sync::atomic::AtomicU64,
    /// The sheets the process parsed, which a page of the same site finds as it parses them again.
    parsed_sheets: Mutex<KeptPastRelease<crate::css::parser::syntax_parser::ParsedStyleSheet>>,
    /// The newest table of the user agent's rules, which the process publishes once for each user
    /// agent sheet a document first attaches.
    user_agent_rules: Mutex<Arc<UserAgentRules>>,
    /// The user agent rule indexes and sheet segments of the process, which engines whose atoms are
    /// the process's share.
    pub(super) rule_indexes: pipeline::ProcessRuleIndexes,
    /// The fact lists of every document's projection rows, interned by content.
    pub(super) fact_lists: Mutex<pipeline::projection::FactListsTable>,
    /// The initial groups the documents of the process share, by the device pixel ratio lengths
    /// snap by, which every initial length computes against.
    initial_groups: Mutex<HashMap<u64, Vec<std::sync::Weak<SharedInitialGroups>>>>,
    /// The groups of default values every computation starting from nothing starts from.
    pub(super) default_groups: crate::css::computed_values::DefaultGroups,
    /// Whether what is read of an element's values is checked against what its groups build from
    /// them, which debug builds always do and LIBWEB_VERIFY_STYLE_CHUNKS asks release builds to.
    pub(super) verifies_reads: bool,
}

/// The initial groups the documents of the process share: each group of a document's initial
/// style but its font group, which holds the fonts of its document's font computer and is the
/// default one here.
pub(super) struct SharedInitialGroups(crate::css::computed_values::StyleGroups);

impl SharedInitialGroups {
    pub(super) fn groups(&self) -> &crate::css::computed_values::StyleGroups {
        &self.0
    }

    /// Whether each group of `groups` but the font group holds what this one does.
    fn hold_the_same_as(&self, groups: &crate::css::computed_values::StyleGroups) -> bool {
        use crate::css::computed_values::StyleGroupIndex;
        use crate::css::table_group_builder::group_index;
        StyleGroupIndex::all()
            .filter(|index| index.usize() != group_index::FONT)
            .all(|index| {
                // SAFETY: Both hold a reference to each of their groups.
                unsafe {
                    crate::css::computed_values::groups_hold_the_same(
                        index.usize(),
                        self.0[index.usize()].as_ptr(),
                        groups[index.usize()].as_ptr(),
                    )
                }
            })
    }
}

impl Default for ProcessStyle {
    fn default() -> Self {
        Self::with_atoms(Arc::<ProcessAtoms>::default())
    }
}

impl ProcessStyle {
    /// Style data of a process whose atoms are `atoms`.
    pub(super) fn with_atoms(atoms: Arc<ProcessAtoms>) -> Self {
        Self {
            atoms,
            selector_arena_serials: std::sync::atomic::AtomicU64::new(1),
            parsed_sheets: Default::default(),
            user_agent_rules: Default::default(),
            rule_indexes: Default::default(),
            fact_lists: Default::default(),
            initial_groups: Default::default(),
            default_groups: Default::default(),
            verifies_reads: cfg!(debug_assertions) || std::env::var_os("LIBWEB_VERIFY_STYLE_CHUNKS").is_some(),
        }
    }
}

impl ProcessStyle {
    /// The initial groups the documents of the process share that hold what `groups`, a
    /// document's initial groups computed under the device pixel ratio
    /// `device_pixels_per_css_pixel`, hold besides its font group: the ones another document
    /// computed, or these.
    pub(super) fn share_initial_groups(
        &self,
        device_pixels_per_css_pixel: f64,
        groups: &crate::css::computed_values::StyleGroups,
    ) -> Arc<SharedInitialGroups> {
        use crate::css::computed_values::StyleGroupIndex;
        use crate::css::table_group_builder::group_index;
        let mut initial_groups = self
            .initial_groups
            .lock()
            .expect("the initial groups lock is not poisoned");
        let shared = initial_groups.entry(device_pixels_per_css_pixel.to_bits()).or_default();
        shared.retain(|entry| entry.strong_count() != 0);
        if let Some(shared) = shared
            .iter()
            .filter_map(std::sync::Weak::upgrade)
            .find(|shared| shared.hold_the_same_as(groups))
        {
            return shared;
        }
        // NB: The font group is the default one, so that no document's fonts outlive it here.
        let mut own = groups.clone();
        own.share_group(self.default_groups.groups(), StyleGroupIndex::new(group_index::FONT));
        let own = Arc::new(SharedInitialGroups(own));
        shared.push(Arc::downgrade(&own));
        own
    }

    /// Keep `sheet`, a sheet the parse cache just published, which holds about `bytes`.
    pub(crate) fn keep_parsed_sheet(
        &self,
        sheet: &Arc<crate::css::parser::syntax_parser::ParsedStyleSheet>,
        bytes: usize,
    ) {
        let left = self
            .parsed_sheets
            .lock()
            .expect("the parsed sheets lock is not poisoned")
            .keep(sheet, bytes);
        drop(left);
    }

    /// Note that a document asked for the parsed sheet `sheet`.
    pub(crate) fn note_parsed_sheet_use(&self, sheet: &Arc<crate::css::parser::syntax_parser::ParsedStyleSheet>) {
        self.parsed_sheets
            .lock()
            .expect("the parsed sheets lock is not poisoned")
            .note_use(sheet);
    }

    /// Let go of what only the process keeps and nothing asked for lately.
    pub(crate) fn prune_kept_past_release(&self) {
        let left = self
            .parsed_sheets
            .lock()
            .expect("the parsed sheets lock is not poisoned")
            .prune();
        drop(left);
        self.rule_indexes.prune_kept_segments();
    }

    /// Let go of everything only the process keeps, as the process is short of memory.
    pub(crate) fn release_kept_past_release(&self) {
        let left = self
            .parsed_sheets
            .lock()
            .expect("the parsed sheets lock is not poisoned")
            .release_all();
        drop(left);
        self.rule_indexes.release_kept_segments();
    }

    /// The newest table of the user agent's rules, holding every sheet the process published.
    pub(super) fn user_agent_rules(&self) -> Arc<UserAgentRules> {
        Arc::clone(
            &self
                .user_agent_rules
                .lock()
                .expect("the user agent rules lock is not poisoned"),
        )
    }

    /// Whether the process published the user agent sheet compiled from the native sheet
    /// `source_identity`.
    pub(crate) fn has_user_agent_sheet(&self, source_identity: u64) -> bool {
        self.user_agent_rules().sheet_of_source(source_identity).is_some()
    }

    /// Publish a user agent sheet compiled from the native sheet `source_identity`, whose rules
    /// `write` writes to an engine of its own: rules of the sheet are written once per process
    /// through what any sheet's are, and nothing edits them after.
    pub(crate) fn publish_user_agent_sheet(
        self: &Arc<Self>,
        source_identity: u64,
        write: impl FnOnce(&mut StyleEngine, SheetID),
    ) {
        let mut newest = self
            .user_agent_rules
            .lock()
            .expect("the user agent rules lock is not poisoned");
        if newest.sheet_of_source(source_identity).is_some() {
            return;
        }
        let mut engine = StyleEngine::new();
        engine.share_process_style(Arc::clone(self));
        *engine.rules = newest.store().clone();
        let sheet = engine.add_sheet(CascadeOrigin::UserAgent);
        engine.attach_sheet(sheet, TreeScopeID::DOCUMENT);
        write(&mut engine, sheet);
        engine.commit_staged_program();
        let selectors = engine.selector_summary();
        // NB: The names the sheet's rules declare stay for as long as the engine's atoms are held.
        let table = newest.with_sheet(
            (*engine.rules).clone(),
            sheet,
            source_identity,
            selectors,
            engine.retained.atoms.clone(),
        );
        *newest = Arc::new(table);
    }

    /// Build the user agent index of the documents deciding with the sheet published from the native
    /// sheet `source_identity` alone, but for the native rules `failing`, on a thread of its own, where
    /// no document waits for it: one that needs the index meanwhile waits for the lock it builds under.
    pub(crate) fn index_user_agent_sheet_ahead(
        self: &Arc<Self>,
        source_identity: u64,
        failing: &[u64],
    ) -> Option<std::thread::JoinHandle<()>> {
        let rules = self.user_agent_rules();
        let variant = pipeline::rule_store::UserAgentVariant::of_published_sheet(&rules, source_identity, failing)?;
        let process = Arc::clone(self);
        let thread = std::thread::Builder::new()
            .name("Style UA index".into())
            .spawn(move || process.rule_indexes.index_ahead(rules.store(), &variant))
            .expect("the thread indexing the user agent's rules starts");
        Some(thread)
    }

    /// The atoms every engine and document host of the process interns names in.
    pub(crate) fn atoms(&self) -> &Arc<ProcessAtoms> {
        &self.atoms
    }

    /// The serial the next selector arena of the process takes.
    pub(crate) fn next_selector_arena_serial(&self) -> crate::css::selector_arena::SelectorArenaSerial {
        crate::css::selector_arena::SelectorArenaSerial::next(&self.selector_arena_serials)
    }
}

impl StyleEngine {
    /// Share the style data of `process` with the other engines of the process. Nothing is indexed
    /// yet.
    pub(crate) fn share_process_style(&mut self, process: Arc<ProcessStyle>) {
        debug_assert!(self.retained.live_style.is_none());
        self.retained.atoms.share_process_atoms(&process.atoms);
        self.retained.process_style = process;
    }

    /// Have `rule` declare `block`, making the block's cascade data where no engine of the
    /// process published the block before, and naming its custom properties in the engine's
    /// atoms.
    pub(super) fn publish_block_declarations(&mut self, rule: RuleID, block: &SharedDeclarationBlock) {
        let custom_names: Vec<_> = block
            .custom_properties()
            .iter()
            .map(|property| {
                let name = property.name.to_fly_string();
                let atom = self.atoms.intern_raw(name.raw());
                // SAFETY: The fly string is live for the call, and the engine takes a reference of its own.
                unsafe { self.note_custom_property_name(atom, name.raw()) };
                atom
            })
            .collect();
        let (declarations, made) = RuleDeclarations::of_block(block.clone(), &custom_names);
        self.retained.counters.bump(match made {
            true => Counter::SheetDeclarationsInterned,
            false => Counter::SheetDeclarationsShared,
        });
        self.stage_rule_declarations(rule, declarations);
    }
}

impl RetainedState {
    /// The user agent indexes the engine shares with the others of the process, or none where its
    /// atoms are its own, which no other engine's selectors name.
    pub(super) fn process_rule_indexes(&self) -> Option<&pipeline::ProcessRuleIndexes> {
        self.atoms
            .is_process_global()
            .then_some(&self.process_style.rule_indexes)
    }
}
