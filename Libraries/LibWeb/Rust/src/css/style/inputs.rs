/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

impl StyleEngine {
    pub(super) fn push_pending_region(&mut self, regions: &mut Vec<ImpactRegion>, region: ImpactRegion) {
        let before = regions.capacity();
        regions.push(region);
        let after = regions.capacity();
        self.memory.reserve_required(
            MemoryCategory::BatchScratch,
            ((after - before) * size_of::<ImpactRegion>()) as u64,
        );
    }

    #[must_use]
    pub fn new(device_class: DeviceClass) -> Self {
        Self::new_with_owners(
            device_class,
            DocumentAtoms::for_live_engine(),
            SelectorPrograms::for_live_engine(),
        )
    }

    pub(crate) fn new_for_replay(device_class: DeviceClass) -> Self {
        Self::new_with_owners(
            device_class,
            DocumentAtoms::for_replay(),
            SelectorPrograms::for_replay(),
        )
    }

    fn new_with_owners(device_class: DeviceClass, atoms: DocumentAtoms, programs: SelectorPrograms) -> Self {
        let mut memory = MemoryController::new(device_class);
        let tree = StyleNodeTree::new(&mut memory);
        Self {
            #[cfg(feature = "style-recording")]
            recording_id: None,
            memory,
            parsed_substitution_memory: MemoryLease::new(MemoryCategory::ParsedSubstitutionCache),
            counters: Counters::new(),
            tree,
            program: StyleSheetProgram::new(),
            journal: NormalizationJournal::new(),
            deferred_geometry_journal: NormalizationJournal::new(),
            flushing_deferred_geometry_journal: false,
            deferred_element_style_inputs: Vec::new(),
            deferred_element_style_input_memory: MemoryLease::new(MemoryCategory::NormalizationJournal),
            initial_tree_batch_applied: false,
            initial_tree_bulk_load_is_pending: false,
            tree_staging: TreeRelationStaging::default(),
            tree_staging_memory: MemoryLease::new(MemoryCategory::NormalizationJournal),
            program_staging: ProgramStaging::default(),
            sheets_excluded_from_routing: BitColumn::default(),
            routing_needs_detachment_sweep: false,
            sheet_rule_replacement: None,
            match_workspace: MatchEvaluationWorkspace::default(),
            exact_covered_scratch: Vec::new(),
            next_style_transaction_version: StyleTransactionVersion(1),
            document_style_computation_inputs: None,
            font_resolver: None,
            layer_topology_version: 0,
            sheet_order_version: 0,
            specified_values: SpecifiedValues::new(),
            winner_groups: WinnerGroups::new(),
            computed_group_sets: ComputedGroupSets::default(),
            computed_group_set_memory: MemoryLease::new(MemoryCategory::ComputedGroupSet),
            custom_property_environment_memory: MemoryLease::new(MemoryCategory::CustomPropertyEnvironment),
            computed_fixed_metadata_memory: MemoryLease::new(MemoryCategory::ComputedFixedMetadata),
            computed_reconstruction_metadata_memory: MemoryLease::new(MemoryCategory::ComputedReconstructionMetadata),
            style_record_memory: MemoryLease::new(MemoryCategory::StyleRecord),
            animation_overlay_memory: MemoryLease::new(MemoryCategory::AnimationOverlayRecord),
            computed_pseudo_assignment_memory: MemoryLease::new(MemoryCategory::ComputedPseudoAssignment),
            html_element_namespace: StyleAtomID::NONE,
            match_answers: MatchAnswerCatalog::default(),
            selector_truth_sets: SelectorTruthSetCatalog::default(),
            retained_match_answers: RetainedMatchAnswers::default(),
            retained_selector_incidences: RetainedSelectorIncidences::default(),
            selector_incidence_is_current: false,
            batch_matching_traversal: None,
            complete_answers_exactly: false,
            completion_exactness_exhausted: false,
            route_pruning_states: RefCell::new(RoutePruningStateCache::default()),
            prefix_caches: Rc::new(RefCell::new(PrefixCaches::default())),
            #[cfg(test)]
            force_bounded_prefix_completion: false,
            prepared_batch_matching_traversal: None,
            published_match_answers: PublishedMatchAnswers::default(),
            ffi_style_transaction_output: bridge::FfiStyleTransactionOutput::default(),
            ffi_style_transaction_output_memory: MemoryLease::new(MemoryCategory::BridgeBuffer),
            ffi_style_node_query: Vec::new(),
            ffi_style_node_query_memory: MemoryLease::new(MemoryCategory::BridgeBuffer),
            ffi_retained_cascade_assignments: Vec::new(),
            ffi_retained_cascade_assignments_memory: MemoryLease::new(MemoryCategory::BridgeBuffer),
            transaction_fact_view: None,
            facts: ElementFactStore::new(),
            programs,
            attribute_value_text_names: HashSet::default(),
            attribute_value_text_requirements_version: 0,
            selector_programs_need_sweep: false,
            routing: Rc::new(RoutingRegistry::new()),
            selector_truth_changes: SelectorTruthChanges::default(),
            already_planned_selector_truth: DeltaBatch::default(),
            selector_truth_changes_active: false,
            relational_witnesses: RefCell::new(RelationalWitnesses::default()),
            relational_witness_residency: MemoryLease::new(MemoryCategory::RetainedWitness),
            scope_roots: Column::default(),
            scope_by_root: SegmentedNodeColumn::default(),
            scope_programs: intern_table::InternTable::default(),
            vacant_scope_programs: Vec::new(),
            scope_dispatch_templates: HashMap::default(),
            scope_cascade_templates: HashMap::default(),
            ancestor_dispatch_templates: HashMap::default(),
            scope_program_by_scope: Column::default(),
            held_scope_program: None,
            atoms,
            reclaimed_style_atoms: Vec::new(),
            style_atoms_swept: false,
            replay_reclaimed_style_atoms: None,
            fold_id_and_class_name_case: false,
            #[cfg(test)]
            diagnostic_plan_capture: None,
        }
    }

    pub(crate) fn begin_recording(&mut self, device_class: DeviceClass) {
        #[cfg(feature = "style-recording")]
        {
            self.recording_id = record_replay::begin_recording_stream(device_class as u8);
            if self.recording_id.is_some() {
                self.memory.enable_recording_policy();
            }
        }
        #[cfg(not(feature = "style-recording"))]
        let _ = device_class;
    }

    pub(crate) fn end_recording(&mut self) {
        #[cfg(feature = "style-recording")]
        {
            record_replay::end_recording_stream(self.recording_id.take());
            self.memory.disable_recording_policy();
        }
    }

    pub(crate) fn record_boundary_call(
        &self,
        kind: record_replay::EventKind,
        write_payload: impl FnOnce(&mut record_replay::PayloadWriter),
    ) {
        #[cfg(not(feature = "style-recording"))]
        {
            let _ = kind;
            let _ = write_payload;
        }
        #[cfg(feature = "style-recording")]
        let Some(engine_id) = self.recording_id else {
            return;
        };
        #[cfg(feature = "style-recording")]
        record_replay::record_engine_event(engine_id, kind, write_payload);
    }

    pub(crate) fn recording_pointer_token(&self, pointer: usize) -> Option<u64> {
        #[cfg(feature = "style-recording")]
        return self
            .recording_id
            .map(|engine_id| record_replay::pointer_token(engine_id, pointer));
        #[cfg(not(feature = "style-recording"))]
        {
            let _ = pointer;
            None
        }
    }

    pub(crate) fn recording_atom_pointer_token(&self, pointer: usize) -> Option<u64> {
        #[cfg(feature = "style-recording")]
        return self.recording_id.map(|_| record_replay::atom_pointer_token(pointer));
        #[cfg(not(feature = "style-recording"))]
        {
            let _ = pointer;
            None
        }
    }

    pub(crate) fn recording_first_response(&self, category: u8, identity: u64) -> bool {
        #[cfg(feature = "style-recording")]
        return self
            .recording_id
            .is_some_and(|engine_id| record_replay::first_response(engine_id, category, identity));
        #[cfg(not(feature = "style-recording"))]
        {
            let _ = (category, identity);
            false
        }
    }

    pub(crate) fn recording_id(&self) -> Option<u64> {
        #[cfg(feature = "style-recording")]
        return self.recording_id;
        #[cfg(not(feature = "style-recording"))]
        None
    }

    pub(crate) fn forget_recording_atom_mappings(&self, atoms: impl IntoIterator<Item = u32>) {
        #[cfg(feature = "style-recording")]
        if let Some(engine_id) = self.recording_id {
            record_replay::forget_atom_mappings(engine_id, atoms);
        }
        #[cfg(not(feature = "style-recording"))]
        let _ = atoms;
    }

    pub(crate) fn recording_atom_mappings(&self) -> RecordedAtomMappings {
        #[cfg(not(feature = "style-recording"))]
        return RecordedAtomMappings {
            atoms: Vec::new(),
            qualified_atoms: Vec::new(),
        };
        #[cfg(feature = "style-recording")]
        {
            let engine_id = self
                .recording_id
                .expect("only a recording engine serializes atom mappings");
            let mut atoms = self
                .atoms
                .raw()
                .iter()
                .filter(|(_, atom)| record_replay::first_atom_mapping(engine_id, atom.0))
                .map(|(pointer, atom)| {
                    (
                        self.recording_atom_pointer_token(*pointer)
                            .expect("only a recording engine serializes atom mappings"),
                        atom.0,
                    )
                })
                .collect::<Vec<_>>();
            atoms.sort_unstable_by_key(|(_, atom)| *atom);
            let mut qualified_atoms = self
                .atoms
                .qualified()
                .iter()
                .filter(|(_, atom)| record_replay::first_atom_mapping(engine_id, atom.0))
                .map(|((namespace, name), atom)| (*namespace, *name, atom.0))
                .collect::<Vec<_>>();
            qualified_atoms.sort_unstable_by_key(|(_, _, atom)| *atom);
            RecordedAtomMappings { atoms, qualified_atoms }
        }
    }

    /// Says whether the document matches id and class selectors ASCII case-insensitively.
    ///
    /// A document only learns its mode while it is still empty - a parser reads it from the doctype
    /// before the first element arrives, and `document.open()` has already removed every element and
    /// every sheet by the time it resets it - so no rule compiled against the other folding and no
    /// fact published under it can outlive the change.
    pub fn set_fold_id_and_class_name_case(&mut self, fold: bool) {
        self.fold_id_and_class_name_case = fold;
    }

    /// The HTML namespace, for an HTML document, or none for any other kind. It is what decides
    /// whether an attribute name from the legacy list compares its value case-insensitively.
    pub fn set_html_element_namespace(&mut self, namespace: StyleAtomID) {
        self.html_element_namespace = namespace;
    }

    #[must_use]
    pub fn tree(&self) -> &StyleNodeTree {
        &self.tree
    }

    #[must_use]
    pub fn connected_element_count(&self) -> u32 {
        self.tree.connected_element_count()
    }

    #[must_use]
    #[cfg(test)]
    pub(super) fn program(&self) -> &StyleSheetProgram {
        &self.program
    }

    pub(super) fn add_routing_rule(&mut self, rule: RuleID, program: SelectorProgramID) {
        self.programs.settle_memory(&mut self.memory);
        // A detached sheet's routes were shed, and reattachment restores the current routes of
        // every live rule in the sheet, so routes added for a rule edited while its sheet is
        // detached would come back twice. The exclusion covers the edit until the sheet reattaches.
        if self
            .sheets_excluded_from_routing
            .contains(self.program.rule_sheet(rule).0 as usize)
        {
            return;
        }
        let routing = Rc::get_mut(&mut self.routing).expect("routing program is shared outside a planning epoch");
        routing.add_rule(rule, program, &self.programs);
    }

    /// The process-global atom for one interned name identity.
    ///
    /// Selector names and DOM facts intern through here and nowhere else. Two tables keyed by the
    /// same word but assigning their own sequences would compare unequal for the same name, which
    /// is a silent failure to match rather than a loud one.
    pub fn intern_atom(&mut self, raw: usize) -> StyleAtomID {
        self.atoms.intern_cpp_raw(raw)
    }

    /// The document-local atom for a name qualified by a namespace.
    ///
    /// `[ns|x]` names an attribute that `[x]` does not, and one element can carry both. So the
    /// qualified form is a name of its own: the attribute is published under it as well as under
    /// its local name, and the selector that names a namespace tests only this one.
    pub fn intern_qualified_atom(&mut self, namespace: StyleAtomID, name: StyleAtomID) -> StyleAtomID {
        self.atoms.intern_qualified(namespace, name)
    }

    /// Add a style rule that only applies inside a scope, naming the scope's root selectors.
    ///
    /// `before` places the rule immediately ahead of an existing one instead of at the end. A rule
    /// arriving in the middle of a sheet takes an order token between its neighbours: nothing else
    /// is renumbered, and no other rule's identity or compiled program is touched.
    pub fn add_style_rule_in_scope(
        &mut self,
        sheet: SheetID,
        before: Option<RuleID>,
        selectors: &[&CompiledSelector],
        namespaces: NamespaceScope,
        scope: &ScopeChain<'_>,
    ) -> RuleID {
        let rule = match before {
            Some(before) => self.insert_rule_before(before, RuleKind::Style),
            None => self
                .reuse_replaced_style_rule(sheet)
                .unwrap_or_else(|| self.append_rule(sheet, None, RuleKind::Style)),
        };
        let previous_program = self
            .replacement_rule(rule)
            .and_then(|replacement| replacement.version.selector_program);
        let program = self.compile_selectors(selectors, namespaces, scope, previous_program);
        if previous_program != Some(program) {
            self.add_routing_rule(rule, program);
        }

        let mut version = self.current_rule_version(rule);
        version.selector_program = Some(program);
        self.replace_rule_version(rule, version);
        self.counters.bump(Counter::StyleRulesCompiled);
        rule
    }

    pub(crate) fn selector_program_for_rule(&self, rule: RuleID) -> &SelectorProgram {
        let program = self
            .current_rule_version(rule)
            .selector_program
            .expect("a style rule must have a selector program");
        self.programs.get(program)
    }

    #[cfg(feature = "style-recording")]
    pub(crate) fn add_replayed_style_rule(
        &mut self,
        sheet: SheetID,
        before: Option<RuleID>,
        selector_program: SelectorProgram,
    ) -> RuleID {
        let rule = match before {
            Some(before) => self.insert_rule_before(before, RuleKind::Style),
            None => self
                .reuse_replaced_style_rule(sheet)
                .unwrap_or_else(|| self.append_rule(sheet, None, RuleKind::Style)),
        };
        let previous_program = self
            .replacement_rule(rule)
            .and_then(|replacement| replacement.version.selector_program);
        let (program, _) = self.programs.add_with_status(selector_program, &mut self.memory);
        self.selector_programs_need_sweep |= previous_program.is_some();
        self.programs.settle_memory(&mut self.memory);
        if previous_program != Some(program) {
            self.add_routing_rule(rule, program);
        }
        let mut version = self.current_rule_version(rule);
        version.selector_program = Some(program);
        self.replace_rule_version(rule, version);
        self.counters.bump(Counter::StyleRulesCompiled);
        rule
    }

    #[cfg(feature = "style-recording")]
    pub(crate) fn replace_replayed_style_rule_selectors(&mut self, rule: RuleID, selector_program: SelectorProgram) {
        let (program, _) = self.programs.add_with_status(selector_program, &mut self.memory);
        self.selector_programs_need_sweep = true;
        self.programs.settle_memory(&mut self.memory);
        self.add_routing_rule(rule, program);
        let mut version = self.current_rule_version(rule);
        version.selector_program = Some(program);
        self.replace_rule_version(rule, version);
        self.settle_program();
        self.counters.bump(Counter::StyleRulesCompiled);
    }

    /// Record that a sheet declared or gave up a cascade layer.
    ///
    /// The declaration contributes no declarations and matches nothing: what it does is fix the order
    /// of the layers every rule referencing them sits in. A layer name belongs to the tree scope the
    /// sheet is attached to, so what moves is that scope's layer order - one input per scope the sheet
    /// decides in.
    pub fn record_layer_statement(&mut self, sheet: SheetID) {
        for scope in self.program.sheet_scopes(sheet) {
            self.record_layer_topology_change(scope);
        }
    }

    /// Add a `@keyframes` rule, which matches no element and is found by the name it declares.
    ///
    /// It has to be in the program at all for a change to it to be an input, and it has to carry its
    /// name for that input to reach the animations referencing it.
    pub fn add_keyframes_rule(&mut self, sheet: SheetID, before: Option<RuleID>, name: StyleAtomID) -> RuleID {
        self.add_named_rule(sheet, before, RuleKind::Keyframes, name)
    }

    /// Add an `@property` rule, which registers the custom property it names. Registering one changes
    /// how every element that declares or references it computes, which the custom-property index
    /// knows and selector matching cannot say.
    pub fn add_property_rule(
        &mut self,
        sheet: SheetID,
        before: Option<RuleID>,
        name: StyleAtomID,
        has_initial_value: bool,
    ) -> RuleID {
        let rule = self.add_named_rule(sheet, before, RuleKind::Property, name);
        if has_initial_value {
            let mut version = self.current_rule_version(rule);
            version.registration_has_initial_value = true;
            self.replace_rule_version(rule, version);
        }
        rule
    }

    pub(super) fn add_named_rule(
        &mut self,
        sheet: SheetID,
        before: Option<RuleID>,
        kind: RuleKind,
        name: StyleAtomID,
    ) -> RuleID {
        let rule = match before {
            Some(before) => self.insert_rule_before(before, kind),
            None => self.append_rule(sheet, None, kind),
        };
        let mut version = self.current_rule_version(rule);
        version.declared_name = Some(name);
        self.replace_rule_version(rule, version);
        rule
    }

    /// Add a rule that matches no element and is not found by name either, so that a change to it is
    /// an input at all. What it reaches is decided by its kind.
    pub fn add_non_matching_rule(&mut self, sheet: SheetID, before: Option<RuleID>, kind: RuleKind) -> RuleID {
        match before {
            Some(before) => self.insert_rule_before(before, kind),
            None => self.append_rule(sheet, None, kind),
        }
    }

    /// Give an existing rule a new selector list, keeping its identity and its position.
    ///
    /// Editing `selectorText` is one rule changing, not the sheet being rebuilt. The rule keeps its
    /// order token, so nothing around it is renumbered, and the journal sees exactly one selector
    /// field move.
    pub fn replace_style_rule_selectors(
        &mut self,
        rule: RuleID,
        selectors: &[&CompiledSelector],
        namespaces: NamespaceScope,
        scope: &ScopeChain<'_>,
    ) {
        let program = self.compile_selectors(selectors, namespaces, scope, None);
        self.add_routing_rule(rule, program);

        let mut version = self.current_rule_version(rule);
        version.selector_program = Some(program);
        self.replace_rule_version(rule, version);
        self.settle_program();
        self.counters.bump(Counter::StyleRulesCompiled);
    }

    /// Report that a rule's declarations moved, without touching anything else about it.
    ///
    /// The block's contents changed even where the CSSOM object did not, so what makes this a
    /// change is a version rather than the object's address. The rule keeps its identity, its
    /// position, and its selector program, so the journal sees one field move and routing reaches
    /// exactly the elements that rule matches.
    pub fn record_rule_declarations_changed(&mut self, rule: RuleID, block_version: u32) {
        let mut version = self.current_rule_version(rule);
        version.declaration_block = Some(DeclarationBlockID(block_version));
        self.replace_rule_version(rule, version);
        self.settle_program();
    }

    pub(super) fn compile_selectors(
        &mut self,
        selectors: &[&CompiledSelector],
        namespaces: NamespaceScope,
        scope: &ScopeChain<'_>,
        reusable: Option<SelectorProgramID>,
    ) -> SelectorProgramID {
        let fold_id_and_class_name_case = self.fold_id_and_class_name_case;
        let html_element_namespace = self.html_element_namespace;
        let atoms = &mut self.atoms;
        let mut intern = |raw: usize, namespace: Option<StyleAtomID>| -> StyleAtomID {
            let local = atoms.intern_raw(raw);
            let Some(namespace) = namespace else {
                return local;
            };
            atoms.intern_qualified(namespace, local)
        };

        let mut compiler = SelectorCompiler::new(
            &mut intern,
            fold_id_and_class_name_case,
            html_element_namespace,
            namespaces,
        );
        for selector in selectors {
            let compiled = compiler.compile_in_scope(selector, scope);
            match compiled.marker {
                Some(marker) => {
                    if let Some(counter) = marker.counter() {
                        self.counters.bump(counter);
                    }
                    self.counters.bump(Counter::ExactSelectorEntries);
                }
                None => self.counters.bump(Counter::ExactSelectorEntries),
            }
        }
        let compiled = compiler.finish();
        let mut requirements_changed = false;
        for name in compiled.attribute_value_text_names() {
            requirements_changed |= self.attribute_value_text_names.insert(name);
        }
        if requirements_changed {
            self.attribute_value_text_requirements_version += 1;
        }
        if let Some(reusable) = reusable
            && self.programs.get(reusable) == &compiled
        {
            return reusable;
        }
        let (program, _) = self.programs.add_with_status(compiled, &mut self.memory);
        self.selector_programs_need_sweep |= reusable.is_some();
        self.programs.settle_memory(&mut self.memory);
        program
    }

    pub(crate) fn compile_selector_query(&mut self, selectors: &[&CompiledSelector]) -> SelectorProgram {
        let fold_id_and_class_name_case = self.fold_id_and_class_name_case;
        let html_element_namespace = self.html_element_namespace;
        let atoms = &mut self.atoms;
        let mut intern = |raw: usize, namespace: Option<StyleAtomID>| -> StyleAtomID {
            let local = atoms.intern_raw(raw);
            let Some(namespace) = namespace else {
                return local;
            };
            atoms.intern_qualified(namespace, local)
        };

        let mut compiler = SelectorCompiler::new(
            &mut intern,
            fold_id_and_class_name_case,
            html_element_namespace,
            NamespaceScope::default(),
        );
        for selector in selectors {
            compiler.compile_for_query(selector);
        }
        let program = compiler.finish();
        let mut requirements_changed = false;
        for name in program.attribute_value_text_names() {
            requirements_changed |= self.attribute_value_text_names.insert(name);
        }
        if requirements_changed {
            self.attribute_value_text_requirements_version += 1;
        }
        program
    }

    pub fn attribute_value_text_requirements_version(&self) -> u64 {
        self.attribute_value_text_requirements_version
    }

    #[must_use]
    pub fn attribute_name_requires_value_text(&self, name: StyleAtomID) -> bool {
        self.facts
            .attribute_name_keys(name)
            .any(|key| self.attribute_value_text_names.contains(&key))
    }

    #[must_use]
    pub fn counters(&self) -> &Counters {
        &self.counters
    }

    #[must_use]
    pub fn memory(&self) -> &MemoryController {
        &self.memory
    }

    pub(crate) fn resize_parsed_substitution_cache(&mut self, bytes: u64) -> bool {
        let category = MemoryCategory::ParsedSubstitutionCache;
        if self.memory.external_tier3_drop_is_pending(category) {
            if bytes != 0 {
                return false;
            }
            self.parsed_substitution_memory.reconcile_committed(&mut self.memory, 0);
            self.memory.complete_external_tier3_drop(category);
            return true;
        }
        self.parsed_substitution_memory
            .reconcile_committed(&mut self.memory, bytes);
        self.memory.finish_committed_acceleration_growth(category);
        true
    }

    /// Mint `out.len()` element identities in one call. Identity allocation is batched because a
    /// call per element is exactly the boundary shape this design rules out.
    pub fn allocate_style_nodes(&mut self, out: &mut [u32]) {
        for slot in out.iter_mut() {
            let node = self.tree.allocate_element(&mut self.memory);
            self.counters.bump(Counter::StyleNodesAllocated);
            *slot = node.raw();
        }
        self.publish_budget_inputs();
    }

    /// Stage a structural change. The normalized transaction installs the final relation rows at
    /// the next observation boundary.
    pub fn record_tree_delta(&mut self, node: StyleNodeID, old: Option<TreeRelations>, new: Option<TreeRelations>) {
        if old != new {
            self.stage_tree_delta(node, old, new);
        }
    }

    /// Record a change to style inputs which are properties of the document environment rather
    /// than of an element or stylesheet rule.
    pub fn record_environment_change(&mut self) {
        self.discard_prepared_batch_matching_traversal();
        self.journal
            .record_complete_scope_action(InputKind::Environment, &mut self.memory, &mut self.counters);
    }

    /// Whether the first tree batch can be installed directly.
    pub(crate) fn can_bulk_load_initial_tree(&self) -> bool {
        !self.initial_tree_batch_applied
    }

    /// Install a first batch of unique element arrivals without journalling one structural input
    /// per row. The document root is the transaction envelope, so planning will still choose the
    /// same exact whole-document result at first observation.
    pub(crate) fn bulk_load_initial_tree(
        &mut self,
        document_root: StyleNodeID,
        arrivals: &[(StyleNodeID, TreeRelations)],
    ) {
        debug_assert!(!self.initial_tree_batch_applied);
        debug_assert!(!arrivals.is_empty());
        debug_assert!(arrivals.iter().any(|&(node, _)| node == document_root));

        for &(node, relations) in arrivals {
            self.link(node, relations);
        }
        self.publish_budget_inputs();

        let root_relations = arrivals
            .iter()
            .find_map(|&(node, relations)| (node == document_root).then_some(relations))
            .unwrap();
        let recorded = self.record(
            InputKey::TreeRelations(document_root),
            InputValue::TreeRelations(None),
            InputValue::TreeRelations(Some(root_relations)),
        );
        debug_assert!(recorded);

        let folded_rows = arrivals.len() - 1;
        self.counters.add(Counter::RawMutationRecords, folded_rows as u64);
        self.counters.add(Counter::TreeDeltas, folded_rows as u64);
        self.counters.bump(Counter::InitialBulkLoads);
        self.counters.add(Counter::InitialBulkTreeRows, arrivals.len() as u64);
        self.initial_tree_batch_applied = true;
        self.initial_tree_bulk_load_is_pending = true;
    }

    pub fn record_input(&mut self, key: InputKey, old: InputValue, new: InputValue) {
        if self.record(key, old, new) {
            self.apply_to_facts_without_settling(key, new);
        }
    }

    #[must_use]
    pub fn has_pending_transaction(&self) -> bool {
        !self.journal.is_empty()
            || (!self.flushing_deferred_geometry_journal && !self.deferred_geometry_journal.is_empty())
            || !self.tree_staging.is_empty()
            || self.program_staging.is_dirty()
            || self.sheet_rule_replacement.is_some()
    }

    #[must_use]
    pub fn has_deferred_geometry_transaction(&self) -> bool {
        !self.flushing_deferred_geometry_journal && !self.deferred_geometry_journal.is_empty()
    }

    /// Whether any element style input is still deferred, waiting for the first transaction with a
    /// document root. A rootless flush drains the journal but preserves these — so an engine that
    /// reports no pending transaction can still owe an element its recomputation.
    #[must_use]
    pub fn has_deferred_element_style_inputs(&self) -> bool {
        !self.deferred_element_style_inputs.is_empty()
    }

    /// Whether one element still owes a deferred style input, asked per node the way the recorded
    /// batch is. The deferred inputs are kept sorted by key, so this is a binary search.
    #[must_use]
    pub fn has_deferred_element_style_input(&self, node: StyleNodeID) -> bool {
        self.deferred_element_style_inputs
            .binary_search_by_key(&InputKey::ElementStyleInput(node), |pending| pending.key)
            .is_ok()
    }

    /// Whether settling the pending selector inputs can change geometry derived from the committed
    /// layout. This is deliberately a proof of independence rather than a list of properties which
    /// usually avoid layout: anything not explicitly known to preserve geometry remains observable.
    #[must_use]
    pub fn pending_transaction_may_affect_layout_geometry(&self) -> bool {
        if self.journal.is_empty() {
            return !self.tree_staging.is_empty()
                || self.program_staging.is_dirty()
                || self.sheet_rule_replacement.is_some()
                || !self.deferred_element_style_inputs.is_empty()
                || self.initial_tree_bulk_load_is_pending;
        }
        if !self.journal.markers().is_empty()
            || !self.tree_staging.is_empty()
            || self.program_staging.is_dirty()
            || self.sheet_rule_replacement.is_some()
            || !self.deferred_element_style_inputs.is_empty()
            || self.initial_tree_bulk_load_is_pending
        {
            return true;
        }

        let route_liveness = self.routing.route_liveness(&self.program, &self.programs);
        self.journal.inputs().any(|input| {
            let mut keys = match input.key {
                InputKey::LocalFeature(_, LocalFeatureKey::PartExposure | LocalFeatureKey::ArrivingFacts) => {
                    return true;
                }
                InputKey::LocalFeature(_, LocalFeatureKey::Attribute(name)) => {
                    let mut keys = routing_keys_for_input(&input);
                    for other in self.facts.attribute_name_keys(name) {
                        if other != name {
                            keys.push(RoutingKey::AttributeName(other));
                        }
                    }
                    keys
                }
                InputKey::LocalFeature(..) | InputKey::State(..) => routing_keys_for_input(&input),
                _ => return true,
            };
            keys.sort_unstable();
            keys.dedup();
            keys.into_iter().any(|key| {
                self.routing.routes_for(key).iter().copied().any(|route| {
                    if !route_liveness.contains(route.index()) {
                        return false;
                    }
                    let rule = self.routing.rule_of(route);
                    !self.program.declarations_are_complete_for(rule)
                        || self.program.declared_properties_of(rule).iter().any(|declared| {
                            crate::css::property_metadata::property_may_affect_layout_geometry(declared.property)
                        })
                })
            })
        })
    }

    /// Preserve the pending paint-only selector facts as the style change event established by a
    /// geometry read. Repeated reads advance the same boundary to the latest observed facts.
    /// Returning false means exact journalling coarsened while combining the facts, so the caller
    /// must settle style instead of reusing layout.
    pub fn defer_pending_transaction_for_geometry_read(&mut self) -> bool {
        debug_assert!(!self.flushing_deferred_geometry_journal);
        debug_assert!(self.tree_staging.is_empty());
        debug_assert!(!self.program_staging.is_dirty());
        debug_assert!(self.sheet_rule_replacement.is_none());
        debug_assert!(self.deferred_element_style_inputs.is_empty());
        debug_assert!(!self.initial_tree_bulk_load_is_pending);
        debug_assert!(self.journal.markers().is_empty());
        debug_assert!(
            self.journal
                .inputs()
                .all(|input| matches!(input.key, InputKey::LocalFeature(..) | InputKey::State(..)))
        );

        if self.journal.is_empty() {
            return true;
        }
        if self.deferred_geometry_journal.is_empty() {
            std::mem::swap(&mut self.journal, &mut self.deferred_geometry_journal);
        } else {
            self.deferred_geometry_journal
                .absorb_newer(&mut self.journal, &mut self.memory, &mut self.counters);
        }
        self.deferred_geometry_journal.markers().is_empty()
    }

    /// Make the style transaction sealed by a geometry read current while preserving local facts
    /// recorded after it for the following style change event.
    pub fn begin_deferred_geometry_transaction_flush(&mut self) -> bool {
        debug_assert!(!self.flushing_deferred_geometry_journal);
        if self.deferred_geometry_journal.is_empty()
            || !self.journal.markers().is_empty()
            || !self.tree_staging.is_empty()
            || self.program_staging.is_dirty()
            || self.sheet_rule_replacement.is_some()
            || !self.deferred_element_style_inputs.is_empty()
            || self.initial_tree_bulk_load_is_pending
            || !self
                .journal
                .inputs()
                .all(|input| matches!(input.key, InputKey::LocalFeature(..) | InputKey::State(..)))
        {
            return false;
        }

        let later_inputs: Vec<NormalizedInput> = self.journal.inputs().collect();
        for input in &later_inputs {
            self.apply_to_facts_without_settling(input.key, input.old);
        }
        std::mem::swap(&mut self.journal, &mut self.deferred_geometry_journal);
        self.flushing_deferred_geometry_journal = true;
        true
    }

    /// Restore the local facts recorded after the geometry boundary once its transaction has been
    /// consumed.
    pub fn end_deferred_geometry_transaction_flush(&mut self) {
        assert!(self.flushing_deferred_geometry_journal);
        assert!(self.journal.is_empty());
        assert!(self.tree_staging.is_empty());
        assert!(!self.program_staging.is_dirty());
        assert!(self.sheet_rule_replacement.is_none());
        assert!(self.deferred_element_style_inputs.is_empty());

        std::mem::swap(&mut self.journal, &mut self.deferred_geometry_journal);
        let later_inputs: Vec<NormalizedInput> = self.journal.inputs().collect();
        for input in later_inputs {
            self.apply_to_facts_without_settling(input.key, input.new);
        }
        self.flushing_deferred_geometry_journal = false;
    }

    pub(super) fn merge_deferred_geometry_transaction(&mut self) {
        if self.flushing_deferred_geometry_journal || self.deferred_geometry_journal.is_empty() {
            return;
        }
        if self.journal.is_empty() {
            std::mem::swap(&mut self.journal, &mut self.deferred_geometry_journal);
            return;
        }
        self.deferred_geometry_journal
            .absorb_newer(&mut self.journal, &mut self.memory, &mut self.counters);
        std::mem::swap(&mut self.journal, &mut self.deferred_geometry_journal);
    }

    /// Record one member of a flat FFI batch without repeatedly settling the fact-store capacity.
    ///
    /// The bridge has already applied every tree delta before it publishes facts. It can therefore
    /// identify facts carried by a node's arrival once per node instead of probing and replacing
    /// the same journal key once per fact.
    pub(crate) fn record_batched_input(
        &mut self,
        key: InputKey,
        old: InputValue,
        new: InputValue,
        arriving_node: bool,
    ) {
        if arriving_node {
            debug_assert!(matches!(key, InputKey::LocalFeature(..) | InputKey::State(..)));
            self.counters.bump(Counter::ArrivingNodeFactsFolded);
            self.counters.bump(Counter::RawMutationRecords);
            self.counters.bump(Counter::LocalFeatureDeltas);
            self.apply_to_facts_without_settling(key, new);
        } else if self.record(key, old, new) {
            self.apply_to_facts_without_settling(key, new);
        }
    }

    /// Record a state publication against the settled value StyleEngine already owns.
    ///
    /// State invalidators may conservatively republish a related group of pseudo-classes when one
    /// member changes. The settled fact is therefore the old side; assuming every publication is
    /// a Boolean toggle would invent transitions for the unchanged members of that group.
    pub(crate) fn record_batched_state(
        &mut self,
        node: StyleNodeID,
        fact: StateFact,
        new_value: bool,
        arriving_node: bool,
    ) {
        let old_value = self.facts.states_of_node(node).contains(fact);
        self.record_batched_input(
            InputKey::State(node, fact),
            InputValue::State(old_value),
            InputValue::State(new_value),
            arriving_node,
        );
    }

    /// Install the fixed facts carried by one element arrival. The tree row already routes the
    /// arriving element, so facts which can change later update their columns without adding one
    /// journal entry apiece.
    pub(crate) fn record_element_arrival(
        &mut self,
        node: StyleNodeID,
        arrival: &super::bridge::FfiElementArrival,
        custom_states: &[StyleAtomID],
        arriving_node: bool,
    ) {
        debug_assert!(arriving_node);
        self.facts.set_namespace(node, StyleAtomID(arrival.namespace_atom));
        self.facts.set_is_slot(node, arrival.is_slot);
        let mut publish_feature = |feature, value| {
            self.record_batched_input(
                InputKey::LocalFeature(node, feature),
                InputValue::Feature(FeatureValue::Absent),
                InputValue::Feature(value),
                arriving_node,
            );
        };
        if arrival.language_atom != 0 {
            publish_feature(
                LocalFeatureKey::Language,
                FeatureValue::Atom(StyleAtomID(arrival.language_atom)),
            );
        }
        if arrival.directionality_atom != 0 {
            publish_feature(
                LocalFeatureKey::Directionality,
                FeatureValue::Atom(StyleAtomID(arrival.directionality_atom)),
            );
        }
        if arrival.heading_level != 0 {
            publish_feature(
                LocalFeatureKey::HeadingLevel,
                FeatureValue::Number(u32::from(arrival.heading_level)),
            );
        }
        for &state in custom_states {
            self.record_batched_input(
                InputKey::LocalFeature(node, LocalFeatureKey::CustomState(state)),
                InputValue::Feature(FeatureValue::Absent),
                InputValue::Feature(FeatureValue::Present),
                arriving_node,
            );
        }
        self.facts.set_custom_states(node, custom_states, &mut self.memory);
    }

    pub(crate) fn settle_batched_inputs(&mut self) {
        if !self.journal.contains_only_element_style_inputs() {
            self.discard_prepared_batch_matching_traversal();
        }
        self.discard_published_match_answers();
    }

    #[must_use]
    pub(crate) fn node_arrival_is_pending(&self, node: StyleNodeID) -> bool {
        self.journal.pending_old(InputKey::TreeRelations(node)) == Some(InputValue::TreeRelations(None))
    }

    /// Apply an accepted input to the authoritative fact arrangement.
    ///
    /// A fact exists in the store exactly because a mutation published it, which is what lets the
    /// evaluator distinguish "this element has no such class" from "nothing ever told me about this
    /// element".
    pub(super) fn apply_to_facts_without_settling(&mut self, key: InputKey, new: InputValue) {
        match (key, new) {
            (InputKey::LocalFeature(node, feature), InputValue::Feature(value)) => match feature {
                LocalFeatureKey::TagName => {
                    if let FeatureValue::Atom(atom) = value {
                        self.facts.set_tag(node, atom, &mut self.memory);
                    }
                }
                LocalFeatureKey::PartExposure => self.facts.set_part_exposure(
                    node,
                    match value {
                        FeatureValue::Atom(atom) => atom,
                        _ => StyleAtomID::NONE,
                    },
                ),
                LocalFeatureKey::Language => self.facts.set_language(
                    node,
                    match value {
                        FeatureValue::Atom(atom) => atom,
                        _ => StyleAtomID::NONE,
                    },
                ),
                LocalFeatureKey::Directionality => self.facts.set_directionality(
                    node,
                    match value {
                        FeatureValue::Atom(atom) => atom,
                        _ => StyleAtomID::NONE,
                    },
                    &mut self.memory,
                ),
                LocalFeatureKey::HeadingLevel => self.facts.set_heading_level(
                    node,
                    match value {
                        FeatureValue::Number(level) => level as u8,
                        _ => 0,
                    },
                ),
                LocalFeatureKey::FoldedTagName => self.facts.set_folded_tag(
                    node,
                    match value {
                        FeatureValue::Atom(atom) => atom,
                        _ => StyleAtomID::NONE,
                    },
                    &mut self.memory,
                ),
                // Parts and custom states are published as complete sets after their individual
                // journal deltas have been recorded. Arrival is only a routing key.
                LocalFeatureKey::Part(_) | LocalFeatureKey::CustomState(_) | LocalFeatureKey::ArrivingFacts => {}
                // A text node is not a style node, so nothing in the tree can say it is there.
                // `Present` on this key means the element is empty.
                LocalFeatureKey::Emptiness => self.facts.set_has_text_content(node, !value.holds()),
                LocalFeatureKey::Id => self.facts.set_id(
                    node,
                    match value {
                        FeatureValue::Atom(atom) => atom,
                        _ => StyleAtomID::NONE,
                    },
                    &mut self.memory,
                ),
                LocalFeatureKey::Class(class) => self.facts.set_class(node, class, value.holds(), &mut self.memory),
                LocalFeatureKey::Attribute(name) => {
                    // The value's atom rides on the same delta. Presence is what routing reads; the
                    // value is what an exact test compares, and a cold pass has no DOM to ask.
                    let atom = match value {
                        FeatureValue::Atom(atom) => atom,
                        _ => StyleAtomID::NONE,
                    };
                    self.facts
                        .set_attribute(node, name, atom, value.holds(), &mut self.memory);
                }
            },
            (InputKey::State(node, fact), InputValue::State(value)) => self.facts.set_state(node, fact, value),
            _ => {}
        }
    }

    /// Returns whether the change joined the current transaction.
    pub(super) fn record(&mut self, key: InputKey, old: InputValue, new: InputValue) -> bool {
        self.discard_prepared_batch_matching_traversal();
        if let Some(node) = self.node_whose_arrival_carries(key) {
            // Every fact of an arriving element folds onto one key, so the journal holds one entry
            // per element rather than one per fact. Routing reads the facts back off the element.
            self.counters.bump(Counter::ArrivingNodeFactsFolded);
            self.journal.record(
                InputKey::LocalFeature(node, LocalFeatureKey::ArrivingFacts),
                InputValue::Feature(FeatureValue::Absent),
                InputValue::Feature(FeatureValue::Present),
                &mut self.memory,
                &mut self.counters,
            );
            return true;
        }
        self.journal.record(key, old, new, &mut self.memory, &mut self.counters);
        true
    }

    /// Whether a fact about an element is already carried by that element arriving.
    ///
    /// An element that connects in this transaction has its whole subtree put in the plan by its own
    /// tree delta, and the elements around it - the anchors of a relational query, the neighbours a
    /// sibling selector constrains, the parent whose emptiness moved - are reached from that delta
    /// too, not from anything the arriving element publishes about itself. So each of the facts it
    /// announces on the way in says nothing the arrival does not already say.
    ///
    /// What journalling them costs is the transaction's scratch budget: a page of a few thousand
    /// elements announces tens of thousands of them, and a journal that has to coarsen no longer
    /// knows which keys moved - which costs a restyle of the whole document, for elements whose
    /// arrival was already accounted for.
    ///
    /// The facts themselves are still applied: what is skipped is the journal entry, not the state.
    #[must_use]
    pub(super) fn node_whose_arrival_carries(&self, key: InputKey) -> Option<StyleNodeID> {
        let node = match key {
            InputKey::LocalFeature(node, feature) if feature != LocalFeatureKey::ArrivingFacts => node,
            InputKey::State(node, _) => node,
            _ => return None,
        };
        (self.journal.pending_old(InputKey::TreeRelations(node)) == Some(InputValue::TreeRelations(None)))
            .then_some(node)
    }

    /// The routing keys of every fact an arriving element announced, read back off the element.
    ///
    /// A fact is in the store by the time routing runs, so this is what the individual inputs would
    /// have published between them - a class each, the tag, the id, each attribute name.
    #[must_use]
    pub(super) fn routing_keys_of_arriving_facts(&self, node: StyleNodeID) -> Vec<RoutingKey> {
        let mut keys = Vec::new();

        let tag = self.facts.tag_of_node(node);
        if !tag.is_none() {
            keys.push(RoutingKey::TagName(tag));
        }
        let folded_tag = self.facts.folded_tag_of_node(node);
        if !folded_tag.is_none() && folded_tag != tag {
            keys.push(RoutingKey::TagName(folded_tag));
        }
        let id = self.facts.id_of_node(node);
        if !id.is_none() {
            keys.push(RoutingKey::Id(id));
        }
        for &class in self.facts.classes_of_node(node) {
            keys.push(RoutingKey::Class(class));
        }
        for attribute in self.facts.attributes_of_node(node) {
            for key in self.facts.attribute_name_keys(attribute) {
                keys.push(RoutingKey::AttributeName(key));
            }
        }
        if !self.facts.language_of(node).is_none() {
            keys.push(RoutingKey::Language);
        }
        let directionality = self.facts.directionality_of(node);
        if !directionality.is_none() {
            keys.push(RoutingKey::Directionality(directionality));
        }
        if let Some(row) = self.facts.primary().row_of(node) {
            for &part in self.facts.primary().parts_of(row) {
                keys.push(RoutingKey::Part(part));
            }
            for &state in self.facts.primary().custom_states_of(row) {
                keys.push(RoutingKey::CustomState(state));
            }
        }
        for fact in self.facts.states_of_node(node).facts() {
            keys.push(RoutingKey::State(fact));
        }
        // An element arrives empty or not, and either way that is a positional truth about it that
        // nothing else in this transaction says.
        keys.push(RoutingKey::Structural);

        keys
    }

    pub(super) fn settled_tree_relations(&self, node: StyleNodeID) -> TreeRelations {
        TreeRelations {
            parent: self.tree.parent(node),
            previous_element_sibling: self.tree.previous_element_sibling(node),
            next_element_sibling: self.tree.next_element_sibling(node),
            tree_scope: self.tree.tree_scope(node),
            assigned_slot: self.tree.assigned_slot_of(node),
        }
    }

    #[inline]
    pub(super) fn stage_tree_row(
        &mut self,
        node: StyleNodeID,
        old_if_unstaged: Option<TreeRelations>,
        new: Option<TreeRelations>,
    ) {
        let old = self.tree_staging.current_row(node, old_if_unstaged);
        if old == new {
            return;
        }
        self.record(
            InputKey::TreeRelations(node),
            InputValue::TreeRelations(old),
            InputValue::TreeRelations(new),
        );
        self.tree_staging.stage_row(node, old_if_unstaged, new);
    }

    pub(super) fn stage_connected_tree_row(&mut self, node: StyleNodeID, update: impl FnOnce(&mut TreeRelations)) {
        let old = self
            .tree_staging
            .current_row(node, Some(self.settled_tree_relations(node)));
        let mut new = old.expect("a pending neighbour must remain connected");
        update(&mut new);
        self.stage_tree_row(node, old, Some(new));
    }

    pub(super) fn stage_first_child(&mut self, parent: StyleNodeID, child: Option<StyleNodeID>) {
        self.tree_staging
            .stage_first_child(parent, self.tree.first_element_child(parent), child);
    }

    pub(super) fn settle_tree_staging_memory(&mut self) {
        let bytes = self.tree_staging.capacity_bytes();
        self.tree_staging_memory.resize_required_to(&mut self.memory, bytes);
    }

    /// Stage one structural delta and the neighbour rows it derives.
    pub(super) fn stage_tree_delta(
        &mut self,
        node: StyleNodeID,
        old: Option<TreeRelations>,
        new: Option<TreeRelations>,
    ) {
        if let Some(old) = old {
            if let Some(previous) = old.previous_element_sibling {
                self.stage_connected_tree_row(previous, |relations| {
                    relations.next_element_sibling = old.next_element_sibling;
                });
            } else if let Some(parent) = old.parent {
                self.stage_first_child(parent, old.next_element_sibling);
            }
            if let Some(next) = old.next_element_sibling {
                self.stage_connected_tree_row(next, |relations| {
                    relations.previous_element_sibling = old.previous_element_sibling;
                });
            }
        }
        if let Some(new) = new {
            if let Some(previous) = new.previous_element_sibling {
                self.stage_connected_tree_row(previous, |relations| {
                    relations.next_element_sibling = Some(node);
                });
            } else if let Some(parent) = new.parent {
                self.stage_first_child(parent, Some(node));
            }
            if let Some(next) = new.next_element_sibling {
                self.stage_connected_tree_row(next, |relations| {
                    relations.previous_element_sibling = Some(node);
                });
            }
        }
        self.stage_tree_row(node, old, new);
        self.settle_tree_staging_memory();
    }

    pub(super) fn depth_recompute_nodes(
        &self,
        staged_rows: &[(StyleNodeID, Option<TreeRelations>, Option<TreeRelations>)],
    ) -> HashSet<StyleNodeID> {
        staged_rows
            .iter()
            .filter_map(|&(node, before, relations)| {
                let relations = relations?;
                (before.is_none() || self.tree.parent(node) != relations.parent).then_some(node)
            })
            .collect()
    }

    /// Install final staged relation rows at the transaction barrier.
    pub(super) fn apply_staged_tree_deltas(&mut self) {
        if self.tree_staging.is_empty() || self.tree_staging.is_applied() {
            return;
        }
        let staged_rows = self.tree_staging.dirty_rows();
        let staged_first_children = self.tree_staging.dirty_first_children();
        // Depth changes only for arrivals and for nodes whose parent differs from the resident one,
        // read before installation: the frozen before-side parent misses a move that a mid-transaction
        // application already installed, and a sibling-only row must not count as a moved parent.
        // A moved parent's subtree walk covers its moved descendants, so those are skipped below.
        let depth_recompute_nodes = self.depth_recompute_nodes(&staged_rows);

        for &(node, _, relations) in &staged_rows {
            let Some(relations) = relations else {
                self.tree.set_parent_without_updating_depth(node, None);
                self.tree.set_next_element_sibling(node, None);
                self.tree.set_previous_element_sibling(node, None);
                self.tree.set_assigned_slot(node, None, &mut self.memory);
                continue;
            };
            self.tree.set_parent_without_updating_depth(node, relations.parent);
            self.tree.set_next_element_sibling(node, relations.next_element_sibling);
            self.tree
                .set_previous_element_sibling(node, relations.previous_element_sibling);
            if relations.tree_scope != TreeScopeID::DOCUMENT {
                self.tree.enable_tree_scopes(&mut self.memory);
            }
            if self.tree.has_tree_scopes() {
                self.tree.set_tree_scope(node, relations.tree_scope);
            }
            self.tree
                .set_assigned_slot(node, relations.assigned_slot, &mut self.memory);
        }
        for (parent, _, child) in &staged_first_children {
            self.tree.set_first_element_child(*parent, *child);
        }
        for &(node, _, _) in &staged_rows {
            if !depth_recompute_nodes.contains(&node) {
                continue;
            }
            let parent_is_recomputed = self
                .tree
                .parent(node)
                .is_some_and(|parent| depth_recompute_nodes.contains(&parent));
            if !parent_is_recomputed {
                self.tree.recompute_subtree_depth(node);
            }
        }
        for &(node, _, relations) in &staged_rows {
            if relations.is_some() || !self.tree.is_live(node) {
                continue;
            }
            self.winner_groups.remove(node);
            let live_animation_overlays_before = self.computed_group_sets.live_animation_overlay_records();
            self.computed_group_sets.remove(node);
            let live_animation_overlays_after = self.computed_group_sets.live_animation_overlay_records();
            self.settle_computed_memory();
            self.counters.add(
                Counter::AnimationOverlaySlotsReleased,
                (live_animation_overlays_before - live_animation_overlays_after) as u64,
            );
            self.counters.set(
                Counter::LiveAnimationOverlayRecords,
                live_animation_overlays_after as u64,
            );
            self.tree.retire_element(node, &mut self.memory);
        }
        self.tree_staging.mark_applied();
        self.publish_budget_inputs();
    }

    pub(super) fn link(&mut self, node: StyleNodeID, new: TreeRelations) {
        self.tree.set_parent(node, new.parent);
        self.tree.set_next_element_sibling(node, new.next_element_sibling);
        self.tree
            .set_previous_element_sibling(node, new.previous_element_sibling);
        if let Some(next) = new.next_element_sibling {
            self.tree.set_previous_element_sibling(next, Some(node));
        }
        match new.previous_element_sibling {
            Some(previous) => self.tree.set_next_element_sibling(previous, Some(node)),
            None => {
                if let Some(parent) = new.parent {
                    self.tree.set_first_element_child(parent, Some(node));
                }
            }
        }

        if new.tree_scope != TreeScopeID::DOCUMENT {
            self.tree.enable_tree_scopes(&mut self.memory);
        }
        if self.tree.has_tree_scopes() {
            self.tree.set_tree_scope(node, new.tree_scope);
        }

        // The slot a slottable is assigned to is its parent in the flat tree, and `::slotted()`
        // walks back along it. A slot name change reassigns it with no DOM mutation at all, so the
        // relation has to be carried by the delta rather than derived from the tree.
        self.tree.set_assigned_slot(node, new.assigned_slot, &mut self.memory);
    }

    /// Record the shadow parts an element exposes.
    ///
    /// A part is a fact about the element like a class is: posted so `::part()` rules can be
    /// enumerated from it, and journalled so a change to the `part` attribute routes. The plain
    /// name set is derived here from the name-to-host pairs exact matching needs, so the two views
    /// cannot disagree.
    pub fn set_element_parts(&mut self, node: StyleNodeID, pairs: &[(StyleAtomID, StyleNodeID)]) {
        let mut parts = Vec::new();
        for &(part, _) in pairs {
            if !parts.contains(&part) {
                parts.push(part);
            }
        }
        if self.facts.parts_of(node) != parts {
            let previous: Vec<StyleAtomID> = self.facts.parts_of(node).to_vec();
            for part in previous.iter().filter(|part| !parts.contains(part)) {
                self.record_input(
                    InputKey::LocalFeature(node, LocalFeatureKey::Part(*part)),
                    InputValue::Feature(FeatureValue::Present),
                    InputValue::Feature(FeatureValue::Absent),
                );
            }
            for part in parts.iter().filter(|part| !previous.contains(part)) {
                self.record_input(
                    InputKey::LocalFeature(node, LocalFeatureKey::Part(*part)),
                    InputValue::Feature(FeatureValue::Absent),
                    InputValue::Feature(FeatureValue::Present),
                );
            }
            self.facts.set_parts(node, &parts, &mut self.memory);
        }
        self.tree.set_part_hosts(node, pairs, &mut self.memory);
    }

    /// A fact whose value is an atom is absent when the atom is, so the two are one conversion.
    pub(super) fn atom_or_absent(atom: StyleAtomID) -> FeatureValue {
        match atom.is_none() {
            true => FeatureValue::Absent,
            false => FeatureValue::Atom(atom),
        }
    }

    /// Report the outermost host a `::part()` rule can address this element from.
    ///
    /// What an `exportparts` change moves is which scopes can name an element, not which names it
    /// carries - the forwarded name is usually the one it already had. So the exposure is the fact,
    /// and an element whose reach did not move says nothing.
    pub fn set_element_part_exposure(&mut self, node: StyleNodeID, exposure: StyleAtomID) {
        let previous = self.facts.part_exposure_of(node);
        if previous == exposure {
            return;
        }
        self.record_input(
            InputKey::LocalFeature(node, LocalFeatureKey::PartExposure),
            InputValue::Feature(Self::atom_or_absent(previous)),
            InputValue::Feature(Self::atom_or_absent(exposure)),
        );
    }

    /// Report the element's resolved language tag.
    /// Record the element's namespace.
    ///
    /// An element's namespace is fixed when it is created, so this is a fact the store holds rather
    /// than an input that moves: nothing routes from it, and no journal entry is needed.
    pub fn set_element_namespace(&mut self, node: StyleNodeID, namespace: StyleAtomID) {
        self.facts.set_namespace(node, namespace);
    }

    /// Record the element's heading level, or zero where it has none.
    /// Record that an element is a `<slot>`, which decides whether `::slotted()` can name it.
    pub fn set_element_is_slot(&mut self, node: StyleNodeID, is_slot: bool) {
        self.facts.set_is_slot(node, is_slot);
    }

    pub fn set_element_heading_level(&mut self, node: StyleNodeID, level: u8) {
        let previous = self.facts.heading_level_of(node);
        if previous == level {
            return;
        }
        self.record_input(
            InputKey::LocalFeature(node, LocalFeatureKey::HeadingLevel),
            InputValue::Feature(FeatureValue::Number(u32::from(previous))),
            InputValue::Feature(FeatureValue::Number(u32::from(level))),
        );
    }

    /// Record what an attribute-value atom spells, for the operators an atom cannot answer.
    pub fn set_attribute_value_text(&mut self, value: StyleAtomID, text: &[u16]) {
        self.facts.set_attribute_value_text(value, text);
    }

    #[must_use]
    pub fn has_attribute_value_text(&self, value: StyleAtomID) -> bool {
        self.facts.has_attribute_value_text(value)
    }

    /// Record what a language atom spells, so `:lang()` can compare its ranges against the tag.
    pub fn set_element_language_text(&mut self, language: StyleAtomID, text: &[u16]) {
        self.counters.bump(Counter::LanguageTextsPublished);
        self.facts.set_language_text(language, text);
    }

    pub fn set_element_language(&mut self, node: StyleNodeID, language: StyleAtomID) {
        let previous = self.facts.language_of(node);
        if previous == language {
            return;
        }
        self.record_input(
            InputKey::LocalFeature(node, LocalFeatureKey::Language),
            InputValue::Feature(Self::atom_or_absent(previous)),
            InputValue::Feature(Self::atom_or_absent(language)),
        );
    }

    /// Report the element's resolved directionality, which `:dir()` tests.
    pub fn set_element_directionality(&mut self, node: StyleNodeID, directionality: StyleAtomID) {
        let previous = self.facts.directionality_of(node);
        if previous == directionality {
            return;
        }
        self.record_input(
            InputKey::LocalFeature(node, LocalFeatureKey::Directionality),
            InputValue::Feature(Self::atom_or_absent(previous)),
            InputValue::Feature(Self::atom_or_absent(directionality)),
        );
    }

    /// Replace the custom states an element is in.
    ///
    /// A custom state is a named fact about one element, exactly like a class, so it is published as
    /// one: the names that arrived and the names that left are each a local feature moving, and
    /// `:state()` reaches its subjects through the same postings every other name does.
    pub fn set_element_custom_states(&mut self, node: StyleNodeID, states: &[StyleAtomID]) {
        if self.facts.custom_states_of(node) == states {
            return;
        }
        let previous: Vec<StyleAtomID> = self.facts.custom_states_of(node).to_vec();
        for state in previous.iter().filter(|state| !states.contains(state)) {
            self.record_input(
                InputKey::LocalFeature(node, LocalFeatureKey::CustomState(*state)),
                InputValue::Feature(FeatureValue::Present),
                InputValue::Feature(FeatureValue::Absent),
            );
        }
        for state in states.iter().filter(|state| !previous.contains(state)) {
            self.record_input(
                InputKey::LocalFeature(node, LocalFeatureKey::CustomState(*state)),
                InputValue::Feature(FeatureValue::Absent),
                InputValue::Feature(FeatureValue::Present),
            );
        }
        self.facts.set_custom_states(node, states, &mut self.memory);
    }

    /// See `ElementFactStore::note_attribute_name_forms`.
    pub fn note_attribute_name_forms(&mut self, name: StyleAtomID, forms: index::AttributeNameForms) {
        self.facts.note_attribute_name_forms(name, forms);
    }

    pub fn set_shadow_root(&mut self, host: StyleNodeID, shadow_root: StyleNodeID) {
        self.tree.set_shadow_root(host, shadow_root, &mut self.memory);
    }

    // -- Stylesheet program ------------------------------------------------------------------
    //
    // Every CSSOM mutation maps to a precise typed delta. None of them produces a generic document
    // invalidation, and the granularity is what makes that true: a declaration edit journals the
    // declaration field alone, so selector truth is untouched by an edit that cannot change it.

    pub fn add_sheet(&mut self, object: StyleSheetObjectID, origin: CascadeOrigin) -> SheetID {
        // Reserving an unattached sheet identity cannot change any scope's semantic program.
        let sheet = self.program.add_sheet(object, origin);
        self.settle_program();
        sheet
    }

    /// A sheet whose routes were shed while it was detached contributes routes again the moment
    /// it reattaches. The registry must be whole before the attachment's transaction plans, so
    /// this runs at recording time rather than waiting for the next sweep.
    fn restore_routing_for_reattached_sheet(&mut self, sheet: SheetID) {
        if !self.sheets_excluded_from_routing.set(sheet.0 as usize, false).0 {
            return;
        }
        let rules: Vec<(RuleID, SelectorProgramID)> = self
            .program
            .rules_in_sheet(sheet)
            .into_iter()
            .filter(|&rule| self.program.rule_is_live(rule))
            .filter_map(|rule| Some((rule, self.program.rule_version(rule).selector_program?)))
            .collect();
        let programs = &self.programs;
        let routing = Rc::get_mut(&mut self.routing).expect("routing program is shared outside a planning epoch");
        for (rule, program) in rules {
            routing.add_rule(rule, program, programs);
        }
        routing.settle_memory(&mut self.memory);
    }

    /// Attach a compiled program at the end of a scope's sheet order.
    pub(super) fn attach_sheet(&mut self, sheet: SheetID, tree_scope: TreeScopeID) {
        self.restore_routing_for_reattached_sheet(sheet);
        let mut sheets = self.current_sheets_in_scope(tree_scope);
        let previous_sheets = sheets.clone();
        let was_attached = sheets.contains(&sheet);
        sheets.retain(|&candidate| candidate != sheet);
        sheets.push(sheet);
        let order_changed = was_attached && sheets != previous_sheets;
        self.stage_sheets_in_scope(tree_scope, sheets);
        self.record_attachment(sheet, tree_scope, was_attached, true);
        if order_changed {
            self.record_sheet_order_change(tree_scope);
        }
    }

    /// Attach at a position established before the sheet finished loading. Network completion order
    /// does not determine cascade order.
    pub(super) fn attach_sheet_before(&mut self, sheet: SheetID, before: SheetID, tree_scope: TreeScopeID) {
        self.restore_routing_for_reattached_sheet(sheet);
        let mut sheets = self.current_sheets_in_scope(tree_scope);
        let previous_sheets = sheets.clone();
        let was_attached = sheets.contains(&sheet);
        sheets.retain(|&candidate| candidate != sheet);
        let position = sheets
            .iter()
            .position(|&candidate| candidate == before)
            .unwrap_or(sheets.len());
        sheets.insert(position, sheet);
        let order_changed = was_attached && sheets != previous_sheets;
        self.stage_sheets_in_scope(tree_scope, sheets);
        // A sheet arriving in the middle is not the scope's order changing. Order is kept as tokens
        // precisely so an insertion writes one label and renumbers nothing, so every sheet already
        // attached keeps the priority it had against every other. What is new is one more competitor
        // for the declarations it makes, and the elements that competition can reach are exactly the
        // ones its own rules match, which the attachment recorded above already names. A sheet that
        // was attached a moment ago and is arriving again can be a move, whose order delta is
        // recorded below.
        self.record_attachment(sheet, tree_scope, was_attached, true);
        if order_changed {
            self.record_sheet_order_change(tree_scope);
        }
    }

    /// Attach a sheet immediately before another sheet in the same scope, or at the end when that
    /// sheet is not attached there. Order tokens stay inside the engine: callers name neighbours,
    /// never positions.
    pub fn attach_sheet_before_sheet(&mut self, sheet: SheetID, before: Option<SheetID>, tree_scope: TreeScopeID) {
        let before = before.filter(|&before| self.current_sheets_in_scope(tree_scope).contains(&before));
        match before {
            Some(before) => self.attach_sheet_before(sheet, before, tree_scope),
            None => self.attach_sheet(sheet, tree_scope),
        }
    }

    pub fn detach_sheet(&mut self, sheet: SheetID, tree_scope: TreeScopeID) {
        let mut sheets = self.current_sheets_in_scope(tree_scope);
        let Some(position) = sheets.iter().position(|&candidate| candidate == sheet) else {
            return;
        };
        sheets.remove(position);
        self.stage_sheets_in_scope(tree_scope, sheets);
        self.record_attachment(sheet, tree_scope, true, false);
        self.routing_needs_detachment_sweep = true;
    }

    /// Record the animation names an element's computed style references.
    ///
    /// This is an index, not an input: an element whose `animation-name` changed was already
    /// recomputed by whatever changed it. What the index is for is the other direction - a
    /// `@keyframes` rule finding the elements running the animation it describes.
    pub fn set_element_animation_names(&mut self, node: StyleNodeID, names: &[StyleAtomID]) {
        self.facts.set_animation_names(node, names, &mut self.memory);
    }

    /// Record the custom properties an element declares or references. Also an index rather than an
    /// input, and for the same reason: it answers which elements an `@property` registration reaches.
    pub fn set_element_custom_property_names(
        &mut self,
        node: StyleNodeID,
        names: &[StyleAtomID],
        uses_unnamed: bool,
        uses_custom_functions: bool,
    ) {
        self.facts.set_custom_property_names(node, names, &mut self.memory);
        self.facts
            .set_uses_unnamed_custom_properties(node, uses_unnamed, &mut self.memory);
        self.facts
            .set_uses_custom_functions(node, uses_custom_functions, &mut self.memory);
    }

    /// Name the node a style scope belongs to. A shadow root is a scope and a subtree at once, which
    /// is what lets a sheet attached there be bounded by the tree it decides in.
    /// Record that a tree scope decides with the document's author sheets as well as its own.
    pub fn set_tree_scope_uses_document_sheets(&mut self, tree_scope: TreeScopeID) {
        let previous = self
            .program_staging
            .scopes_using_document_sheets
            .current(tree_scope, || self.program.scope_uses_document_sheets(tree_scope));
        if previous {
            return;
        }
        self.program_staging.scopes_using_document_sheets.stage(
            tree_scope,
            self.program.scope_uses_document_sheets(tree_scope),
            true,
        );
        self.program_staging.base_version.get_or_insert(self.program.version());
        self.invalidate_scope_program(tree_scope);
    }

    pub fn set_tree_scope_root(&mut self, tree_scope: TreeScopeID, root: StyleNodeID) {
        if tree_scope == TreeScopeID::DOCUMENT {
            return;
        }
        let index = tree_scope.0 as usize;
        if let Some(previous_root) = self.scope_roots.get(index).copied().flatten()
            && previous_root != root
        {
            self.scope_by_root.remove(previous_root);
        }
        self.scope_roots.insert(index, Some(root));
        self.scope_by_root.insert(root, tree_scope);
        // The root is a node selectors reach and nothing publishes features for, so this is where it
        // gets a row of its own.
        self.facts.ensure_row(root);
    }

    pub(super) fn scope_root(&self, tree_scope: TreeScopeID) -> Option<StyleNodeID> {
        self.scope_roots.get(tree_scope.0 as usize).copied().flatten()
    }

    /// Everything a sheet attached to these scopes can decide.
    ///
    /// A scope-local sheet reaches the tree it is attached to, and out of it only through `:host`,
    /// `::slotted()` and `::part()` - which reach the host and the nodes slotted into it. So a sheet
    /// in a shadow root can be bounded even when its rules dispatch on nothing enumerable, where the
    /// document's own scope has no bound narrower than the document.
    /// Where a sheet decides, taking in both the scopes it is attached to now and the ones it was
    /// attached to when the transaction began.
    pub(super) fn scopes_of_sheet(
        &self,
        sheet: SheetID,
        departed_sheet_scopes: &[(SheetID, TreeScopeID)],
    ) -> Vec<TreeScopeID> {
        let mut scopes = self.program.sheet_scopes(sheet);
        for &(departed_sheet, scope) in departed_sheet_scopes {
            if departed_sheet == sheet && !scopes.contains(&scope) {
                scopes.push(scope);
            }
        }
        scopes
    }

    pub(super) fn regions_reachable_from_scopes(
        &self,
        scopes: &[TreeScopeID],
        leaves_scope: bool,
        host_is_a_subject: bool,
    ) -> Option<Vec<ImpactRegion>> {
        if scopes.is_empty() {
            return None;
        }
        let mut regions = Vec::new();
        for &scope in scopes {
            let root = match self.scope_root(scope) {
                Some(root) => root,
                // The document's own scope records no root and has no bound narrower than itself,
                // which is a different thing from a shadow scope that has not named one yet.
                None if scope == TreeScopeID::DOCUMENT => return None,
                // A shadow scope whose root has never been named holds no element the engine knows
                // about: every element in a shadow tree takes its place in the style tree through
                // that root, so a scope that has not named one has nothing inside it for a sheet
                // attached there to decide. Attaching a sheet to a shadow root numbers its scope
                // without populating it, which is what `attachShadow` immediately followed by
                // `adoptedStyleSheets` does, and answering that with the document restyles the page
                // once per component.
                //
                // A program that leaves its scope reaches the host instead, so it needs the root to
                // find one, and a scope with no root names no host either.
                None if !leaves_scope => continue,
                None => continue,
            };
            match leaves_scope {
                // `:host` and `::slotted()` describe the host and what is slotted into it, which are
                // in the host's own tree and not in the one the sheet is attached to. A rule using
                // them says nothing about the shadow tree, so naming it would be the widest part of
                // the answer for the narrowest reason.
                // A root with no host is a shadow tree whose host has not taken its place in the
                // style tree, so the tree hangs off nothing the engine can reach and the rule
                // decides for no element that exists. The host computes its style from scratch when
                // it does arrive, which is what makes saying so safe rather than optimistic.
                true => match self.tree.host_of(root) {
                    Some(host) => {
                        if host_is_a_subject {
                            regions.push(ImpactRegion::Node(host));
                        }
                        regions.push(ImpactRegion::StrictSubtree(host));
                    }
                    None => continue,
                },
                false => regions.push(ImpactRegion::Subtree(root)),
            }
        }
        Some(regions)
    }

    /// Everything a named rule in these scopes can reach through its consumers.
    ///
    /// Unlike a selector program, a named rule has no one subject position: an `@keyframes` or
    /// `@property` rule can be referenced both inside its shadow tree and by a declaration matching
    /// `:host` or `::slotted()`. The consumer index identifies the exact nodes when it is complete;
    /// these regions bound that index, and are also the conservative answer when it is not.
    pub(super) fn regions_reachable_for_named_consumers(&self, scopes: &[TreeScopeID]) -> Option<Vec<ImpactRegion>> {
        let mut regions = self.regions_reachable_from_scopes(scopes, false, false)?;
        let outside_regions = self.regions_reachable_from_scopes(scopes, true, true)?;
        regions.extend(outside_regions);
        Some(regions)
    }

    /// Where an entry with no dispatch key can have subjects, when its own shape still says.
    ///
    /// Three things narrow an entry the dispatch buckets gave up on. `:root` matches the document's
    /// root element and nothing else, whatever else its compound tests. A child combinator names
    /// what the subject's parent must be, which does have postings to enumerate - one lookup per
    /// parent candidate reaches every subject the entry can have, where the alternative is every
    /// element in the document. And a descendant combinator says the same thing more weakly: the
    /// subject is somewhere under a named ancestor rather than directly beneath a named parent.
    ///
    /// A relative query the subject has to satisfy is the fourth: `:has(.error)` names no feature of
    /// its own, but it can only match an anchor of that query, and the witness compound does have a
    /// posting to enumerate.
    ///
    /// `None` means the entry says nothing, and the caller falls back to the scope. Every answer here
    /// is a superset of the true subject set, which is what a plan is allowed to be.
    pub(super) fn regions_from_subject_position(
        &self,
        compiled: &SelectorProgram,
        entry: usize,
        document_root: StyleNodeID,
    ) -> Option<Vec<ImpactRegion>> {
        if compiled.subject_is_only_the_root(entry) {
            return Some(vec![ImpactRegion::Node(document_root)]);
        }

        // The sets below are disjunctions: the constrained relative is reachable by at least one of
        // the keys, so the union over all of them covers it. An empty set is no constraint at all.
        //
        // A parent is the tightest, so it is asked for first; an ancestor is consulted when there is
        // no child combinator, and a relative query only when the subject's own position says nothing.
        let (keys, region): (_, fn(StyleNodeID) -> ImpactRegion) = match compiled.subject_parent_dispatch(entry) {
            keys if !keys.is_empty() => (keys, ImpactRegion::Children),
            _ => match compiled.subject_ancestor_dispatch(entry) {
                keys if !keys.is_empty() => (keys, ImpactRegion::StrictSubtree),
                _ => match compiled.subject_relative_anchor(entry) {
                    Some((axis, keys)) if !keys.is_empty() => (keys, anchor_region_for(axis)?),
                    _ => return None,
                },
            },
        };
        if keys.is_empty() {
            return None;
        }
        let mut regions = Vec::new();
        for key in keys {
            if !key.has_selector_posting() {
                return None;
            }
            match self.facts.postings().lookup(key) {
                Lookup::Known(posting) => {
                    for relative in posting.candidates() {
                        regions.push(region(relative));
                    }
                }
                Lookup::KnownAbsent => {}
                Lookup::Missing(_) => return None,
            }
        }
        Some(regions)
    }

    /// Record that a rule sits in a cascade layer.
    ///
    /// Said as the rule is compiled rather than as an input: which layer a rule is in is part of what
    /// the rule is, and a rule that moves between layers is recompiled.
    /// Record which longhand properties one of an element's own declarations covers.
    ///
    /// An element-attached declaration is a cascade component above layers: a style attribute beats
    /// every layered and unlayered rule in its context, whatever layer they are in.
    pub fn set_element_declared_properties(
        &mut self,
        node: StyleNodeID,
        kind: ElementDeclarationKind,
        declared: &[DeclaredProperty],
        declarations_are_complete: bool,
    ) {
        if matches!(
            kind,
            ElementDeclarationKind::PresentationalHint | ElementDeclarationKind::SvgPresentationAttribute
        ) {
            verify_cascade_winners(self, |_| {
                let mut properties: Vec<u16> = declared.iter().map(|declared| declared.property).collect();
                properties.sort_unstable();
                assert!(
                    properties.windows(2).all(|pair| pair[0] != pair[1]),
                    "element-attached declarations repeat a property"
                );
            });
        }
        let (current_declared, current_declarations_are_complete) = self.facts.element_declared_properties(node, kind);
        if current_declarations_are_complete == declarations_are_complete && current_declared == declared {
            return;
        }
        let repair_inputs = declarations_are_complete
            .then(|| {
                let previous = self
                    .winner_groups
                    .token_for(WinnerGroupKey::current(node, self.program.version()))
                    .sparse()
                    .ok()
                    .map(|(_, state)| state)?;
                let retained = Rc::clone(self.retained_match_answer(node).sparse().ok()?);
                Some((previous, retained))
            })
            .flatten();
        let previous_declared = repair_inputs.as_ref().and_then(|_| {
            let (declared, complete) = self.facts.element_declared_properties(node, kind);
            complete.then(|| declared.to_vec())
        });
        self.facts
            .set_element_declared_properties(node, kind, declared.to_vec(), declarations_are_complete);
        let Some(((previous, retained), previous_declared)) = repair_inputs.zip(previous_declared) else {
            return;
        };
        let mut changed_properties: Vec<u16> = previous_declared
            .iter()
            .chain(declared)
            .map(|declared| declared.property)
            .collect();
        changed_properties.sort_unstable();
        changed_properties.dedup();
        changed_properties.retain(|&property| {
            previous_declared.iter().find(|declared| declared.property == property)
                != declared.iter().find(|declared| declared.property == property)
        });
        if !changed_properties.is_empty() {
            self.apply_element_declaration_winner_updates(node, previous, retained, &changed_properties);
        }
    }

    /// Repair the exact properties whose element-attached declaration inventory changed.
    ///
    /// Presentational hints are published while the legacy cascade builds its input, after the
    /// transaction has already reused the selector answer. Re-reducing only their changed
    /// properties here keeps the retained top-1 relation current without matching the element.
    pub(super) fn apply_element_declaration_winner_updates(
        &mut self,
        node: StyleNodeID,
        previous: CascadeStateID,
        retained: Rc<[RetainedRuleMatch]>,
        properties: &[u16],
    ) {
        let matches = retained
            .iter()
            .copied()
            .map(|entry| entry.materialize(node, &self.programs, 0))
            .collect::<Option<Vec<_>>>();
        let Some(matches) = matches else {
            return;
        };
        let Some(updates) = self.exact_cascade_winner_updates_for_properties(node, &matches, None, properties) else {
            return;
        };
        let (state, _) =
            self.with_cascade_interning_counters(|groups| groups.apply_property_updates(previous, &updates));
        let published = self.winner_groups.set(node, state, self.program.version());
        self.winner_groups.settle_memory(&mut self.memory);
        if published {
            self.counters.bump(Counter::CascadeNodeHandlesPublished);
        }
    }
}
