/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

const MIN_SHARED_CASCADE_COMPLETION_BATCH: usize = 16;

impl StyleEngine {
    pub fn take_style_transaction(
        &mut self,
        root: StyleNodeID,
        mut emit: impl FnMut(StyleTransactionVersion, ProgramVersion, &[PublishedStyleDeltaRecord]),
    ) -> bool {
        self.reclaim_computed_memory_if_needed();
        self.sync_tier3_benefit_observations();
        let tier3_evictions = self.memory.finish_tier3_quota_period();
        for &category in &TIER3_REFUSAL_CATEGORIES {
            if tier3_evictions[category as usize] && self.evict_tier3_category(category) {
                self.counters.bump(Counter::Tier3BenefitEvictions);
            }
        }
        self.memory.begin_tier3_quota_period();
        self.winner_groups.begin_quota_period();
        self.relational_witnesses.borrow_mut().set_admitting(true);
        self.route_pruning_states.borrow_mut().clear();
        #[cfg(test)]
        if let Some(capture) = &mut self.diagnostic_plan_capture {
            capture.nodes.clear();
            capture.scoped = true;
        }
        self.discard_prepared_batch_matching_traversal();
        self.discard_published_match_answers();
        for input in std::mem::take(&mut self.deferred_element_style_inputs) {
            self.record_input(input.key, input.old, input.new);
        }
        self.deferred_element_style_input_memory
            .resize_required_to(&mut self.memory, 0);
        let document_root_arrival_is_pending =
            self.journal.pending_old(InputKey::TreeRelations(root)) == Some(InputValue::TreeRelations(None));
        let initial_tree_was_bulk_loaded = self.initial_tree_bulk_load_is_pending && document_root_arrival_is_pending;
        let publish_document_root_arrival = document_root_arrival_is_pending;

        // Whatever the last flush's late exact asks left in the flush-local workspace was measured
        // in the previous topology. Every exact evaluation below reads current-side sibling
        // geometry from it, so it starts empty here, before this transaction's tree is applied.
        let stale_match_workspace_bytes = self.match_workspace.capacity_bytes();
        self.match_workspace = MatchEvaluationWorkspace::default();
        self.memory
            .release(MemoryCategory::BatchScratch, stale_match_workspace_bytes);
        let mut transaction = self.drain_transaction();
        self.apply_staged_transaction(&mut transaction);
        if transaction.is_empty() {
            self.release_transaction_and_sweep_atoms(transaction);
            return true;
        }
        let preserves_selector_incidence = !transaction.has_coarsened_markers()
            && transaction.inputs.iter().all(|input| {
                matches!(
                    input.key,
                    InputKey::RuleField(_, RuleField::Activation | RuleField::Declarations | RuleField::Layer)
                        | InputKey::SheetActivation(_)
                        | InputKey::CascadeTopology(_)
                        | InputKey::ElementDeclaration(..)
                        | InputKey::ElementStyleInput(_)
                )
            });
        if !preserves_selector_incidence {
            self.retained_selector_incidences.clear();
        }
        self.selector_incidence_is_current = preserves_selector_incidence;
        let transaction_version = self.next_style_transaction_version;
        self.next_style_transaction_version = StyleTransactionVersion(
            self.next_style_transaction_version
                .0
                .checked_add(1)
                .expect("style transaction version space exhausted"),
        );
        let program_version = self.program.version();
        self.selector_truth_changes = SelectorTruthChanges::default();
        self.already_planned_selector_truth = DeltaBatch::default();
        self.selector_truth_changes_active = false;
        // Declaration values do not participate in selector matching. Preserve that fact across
        // the transaction/reaction boundary so the consumer can use each element's retained exact
        // answer and compact it against the new declaration inventories.
        //
        // Local fact transactions have exact routed-rule identities, and activation transactions
        // preserve each affected rule's selector program. Their plans can patch the old answer and
        // retain it across the same boundary. Other selector-affecting transactions retire the
        // answers they name: a planned element may not be recomputed by the traversal that
        // immediately follows, and stale state must not survive into a later reuse.
        let transaction_reaches_no_selector = !transaction.has_coarsened_markers()
            && transaction.inputs.iter().all(|input| {
                matches!(
                    input.key,
                    InputKey::ElementDeclaration(..)
                        | InputKey::ElementStyleInput(..)
                        | InputKey::RuleField(_, RuleField::Declarations)
                )
            });
        let match_identity_is_complete_output = !transaction.has_coarsened_markers()
            && transaction.inputs.iter().all(|input| {
                !matches!(
                    input.key,
                    InputKey::ElementDeclaration(..)
                        | InputKey::ElementStyleInput(..)
                        | InputKey::RuleField(_, RuleField::Declarations | RuleField::Layer)
                        | InputKey::CascadeTopology(_)
                )
            });
        let retained_answer_patch_selection = self.rules_for_retained_answer_patch(&transaction);
        let transaction_supports_global_exact_cascade_stops =
            retained_answer_patch_selection.as_ref().is_some_and(|selection| {
                let changed_nodes_have_selector_only_identity_inputs = transaction.inputs.iter().all(|input| {
                    matches!(
                        input.key,
                        InputKey::LocalFeature(
                            _,
                            LocalFeatureKey::Id
                                | LocalFeatureKey::Class(_)
                                | LocalFeatureKey::CustomState(_)
                                | LocalFeatureKey::Attribute(_)
                        )
                    )
                }) && transaction.inputs.iter().all(|input| {
                    let InputKey::LocalFeature(node, LocalFeatureKey::Attribute(_)) = input.key else {
                        return true;
                    };
                    transaction.inputs.iter().any(|candidate| {
                        matches!(
                            candidate.key,
                            InputKey::LocalFeature(candidate_node, LocalFeatureKey::Id | LocalFeatureKey::Class(_))
                                if candidate_node == node
                        )
                    })
                });
                transaction.markers.is_empty()
                    && changed_nodes_have_selector_only_identity_inputs
                    && selection
                        .affected
                        .iter()
                        .all(|affected| self.program.declarations_are_complete_for(affected.rule))
                    && selection.affected.iter().all(|affected| {
                        self.program
                            .rule_version(affected.rule)
                            .selector_program
                            .is_some_and(|program| !self.programs.get(program).contains_relational_selector())
                    })
            });
        // Node-local inputs can route both exact selector changes and conservative direct work in
        // one transaction. An exact confirmation can suppress a node whose cold cascade still
        // equals its computed cascade without forcing unrelated nodes through materialization.
        // Direct actions, broad plans, relational selectors, and stylesheet edits still require
        // their consumer reactions.
        let transaction_inputs_support_retained_cascade_stops = transaction.inputs.iter().all(|input| {
            matches!(
                input.key,
                InputKey::LocalFeature(..)
                    | InputKey::TreeRelations(_)
                    | InputKey::State(..)
                    | InputKey::ElementDeclaration(..)
                    | InputKey::ElementStyleInput(_)
            )
        });
        let transaction_supports_retained_cascade_stops =
            retained_answer_patch_selection.as_ref().is_some_and(|selection| {
                transaction.markers.is_empty()
                    && transaction_inputs_support_retained_cascade_stops
                    && !selection.orders_shifted
                    && selection.cascade_update_properties.is_empty()
            });
        self.selector_truth_changes_active = retained_answer_patch_selection.is_some();
        let reuse_retained_match_answers = transaction_reaches_no_selector || retained_answer_patch_selection.is_some();
        if reuse_retained_match_answers {
            let mut prepared = PreparedBatchMatchingTraversal::new(root);
            prepared.reuse_retained_match_answers = true;
            self.prepared_batch_matching_traversal = Some(prepared);
        }

        // A diagnostic plan represents the document root's arrival as one whole-document result.
        // No selector, program or relation input in the same transaction can widen that answer.
        if !publish_document_root_arrival
            && transaction.inputs.iter().any(|input| {
                matches!(
                    (input.key, input.old, input.new),
                    (
                        InputKey::TreeRelations(node),
                        InputValue::TreeRelations(None),
                        InputValue::TreeRelations(Some(_))
                    ) if node == root
                )
            })
        {
            self.discard_retained_prefix_caches();
            self.retained_match_answers.evict(&mut self.match_answers);
            self.release_transaction_and_sweep_atoms(transaction);
            return false;
        }

        // A normalized transaction this wide will ask enough tree-shaped questions to amortize one
        // preorder pass. Keep the coordinates in Tier-4 scratch and let smaller updates continue to
        // walk the mandatory relation columns directly.
        const TRANSACTION_TOPOLOGY_MINIMUM_INPUTS: usize = 32;
        let mut regions = if transaction.inputs.len() >= TRANSACTION_TOPOLOGY_MINIMUM_INPUTS {
            let regions = ImpactRegions::with_topology(&self.tree, root);
            let bytes = regions.topology_capacity_bytes();
            self.memory.reserve_required(MemoryCategory::BatchScratch, bytes);
            regions
        } else {
            ImpactRegions::new()
        };
        let topology_bytes = regions.topology_capacity_bytes();
        let mut arriving_nodes: Vec<StyleNodeID> = if !transaction.has_coarsened_markers() {
            transaction
                .inputs
                .iter()
                .filter_map(|input| match (input.key, input.old, input.new) {
                    (
                        InputKey::TreeRelations(node),
                        InputValue::TreeRelations(None),
                        InputValue::TreeRelations(Some(_)),
                    ) => Some(node),
                    _ => None,
                })
                .collect()
        } else {
            Vec::new()
        };
        arriving_nodes.sort_unstable();
        arriving_nodes.dedup();
        let (outer_arrivals, nested_arrivals) =
            regions.partition_subtree_roots(&mut arriving_nodes).unwrap_or_else(|| {
                let nested: Vec<StyleNodeID> = arriving_nodes
                    .iter()
                    .copied()
                    .filter(|&node| {
                        self.tree
                            .ancestors(node)
                            .any(|ancestor| arriving_nodes.binary_search(&ancestor).is_ok())
                    })
                    .collect();
                let outer = arriving_nodes
                    .iter()
                    .copied()
                    .filter(|node| nested.binary_search(node).is_err())
                    .collect();
                (outer, nested)
            });
        // An outer arrival's whole subtree is dirty regardless of which selectors are attached.
        // Establish that envelope before preparing prefix transitions, so wide transactions can
        // classify local changes through the same preorder intervals used by routing.
        for &arrival in &outer_arrivals {
            regions.add(ImpactRegion::Subtree(arrival), &mut self.counters);
        }
        // An arriving subtree's own retained answers date from before it detached, and facts can
        // have moved off the journal's books while it was out; they must match cold rather than
        // be patched. A subtree that crossed parents within one flush is not an arrival, its
        // relations fold to two connected sides, but its ancestor context moved just the same
        // and nothing else re-proves its answers: the DOM side restyles it directly and the
        // traversal would otherwise reuse the answers from its old place.
        if retained_answer_patch_selection.is_some() {
            for &arrival in &outer_arrivals {
                for node in self.tree.preorder(arrival) {
                    self.retained_match_answers.forget(&mut self.match_answers, node);
                }
            }
            for input in &transaction.inputs {
                if let (
                    InputKey::TreeRelations(mover),
                    InputValue::TreeRelations(Some(old)),
                    InputValue::TreeRelations(Some(new)),
                ) = (input.key, input.old, input.new)
                    && old.parent != new.parent
                {
                    for node in self.tree.preorder(mover) {
                        self.retained_match_answers.forget_answer(&mut self.match_answers, node);
                    }
                }
            }
        }

        // The parents whose child sequences this transaction touched. A sibling-observing answer
        // for a child of an untouched parent cannot have gone positionally stale.
        let sequence_truth_is_coarse = transaction.has_coarsened_markers();
        let mut sequence_touched_parents: Vec<StyleNodeID> = Vec::new();
        if !sequence_truth_is_coarse {
            for input in &transaction.inputs {
                if let InputKey::TreeRelations(node) = input.key {
                    for value in [input.old, input.new] {
                        if let InputValue::TreeRelations(Some(relations)) = value
                            && let Some(parent) = relations.parent
                        {
                            sequence_touched_parents.push(parent);
                        }
                    }
                    if let Some(parent) = self.tree.parent(node) {
                        sequence_touched_parents.push(parent);
                    }
                }
            }
            sequence_touched_parents.sort_unstable();
            sequence_touched_parents.dedup();
        }
        let sequence_touched_parent_bytes = (sequence_touched_parents.capacity() * size_of::<StyleNodeID>()) as u64;
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, sequence_touched_parent_bytes);
        let connected_element_count = self.tree.connected_element_count() as usize;
        let departing_nodes = transaction
            .inputs
            .iter()
            .filter(|input| {
                matches!(
                    (input.key, input.new),
                    (InputKey::TreeRelations(_), InputValue::TreeRelations(None))
                )
            })
            .count();
        let use_exact_tree_routing = !transaction.has_coarsened_markers()
            && exact_tree_routing_is_selective(arriving_nodes.len() + departing_nodes, connected_element_count);
        let mut transaction_fact_view = self.transaction_fact_view_for(&mut transaction, root, &regions);
        if use_exact_tree_routing {
            let _ = self.install_before_sibling_geometry(&mut transaction_fact_view);
        }
        let has_before_sibling_relations = transaction_fact_view.before_sibling_relations_available;
        self.prefix_caches.borrow_mut().states.mark_previous();
        self.transaction_fact_view = Some(transaction_fact_view);
        let fact_view_bytes = self
            .transaction_fact_view
            .as_ref()
            .map_or(0, TransactionFactView::capacity_bytes);
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, fact_view_bytes);
        let mut sequences = SequenceChanges::new();
        if !transaction.has_coarsened_markers() {
            // Routing reads the program as it stands now, but the inputs happened before it did.
            // A sheet that went away in this same transaction was still deciding when the mutations
            // ahead of it in the journal were recorded, so its rules cannot be skipped as inactive.
            let program_delta = self.program_staging.delta();
            let retained_winners_are_current = transaction
                .inputs
                .iter()
                .all(|input| matches!(input.key, InputKey::LocalFeature(..) | InputKey::State(..)));
            if self.selector_incidence_is_current {
                let programs: Vec<_> = transaction
                    .inputs
                    .iter()
                    .filter(|input| {
                        matches!(
                            (input.key, input.old, input.new),
                            (
                                InputKey::RuleField(_, RuleField::Activation),
                                InputValue::Flag(true),
                                InputValue::Flag(false)
                            )
                        )
                    })
                    .flat_map(|input| transaction.program_joins_for(input.key))
                    .filter_map(|delta| delta.before_program)
                    .collect();
                self.retain_selector_incidences(&programs, root);
            }
            // Only program routing joins against the resident nodes, and a transaction of pure
            // DOM inputs has no program joins at all. Walking every element of the document to
            // enumerate them for such a transaction would be the flush's single largest cost.
            let program_routing_needs_resident_nodes = !outer_arrivals.is_empty()
                && transaction
                    .inputs
                    .iter()
                    .any(|input| !transaction.program_joins_for(input.key).is_empty());
            let mut resident_nodes: Vec<StyleNodeID> = if !program_routing_needs_resident_nodes {
                Vec::new()
            } else if regions.topology_node_count() == connected_element_count {
                regions
                    .nodes_outside_covered_subtrees()
                    .expect("a non-empty topology has preorder nodes")
            } else {
                self.elements_under_excluding_subtrees(root, &outer_arrivals)
            };
            resident_nodes.sort_unstable();
            let arrival_scratch_bytes = ((arriving_nodes.capacity()
                + nested_arrivals.capacity()
                + outer_arrivals.capacity()
                + resident_nodes.capacity())
                * size_of::<StyleNodeID>()) as u64;
            self.memory
                .reserve_required(MemoryCategory::BatchScratch, arrival_scratch_bytes);
            let tree_routing = TreeRoutingMode {
                use_exact: use_exact_tree_routing,
                has_before_sibling_relations,
                transaction_inputs: &transaction.inputs,
            };
            let routing_for_siblings = Rc::clone(&self.routing);
            let sibling_entries = routing_for_siblings.live_sibling_entries(&self.program, &self.programs);
            let mut sibling_candidates = routing_for_siblings.live_sibling_workspace(&self.program, &self.programs);
            let mut pending_routes = PendingRoutes::new();
            let mut pending_sibling_routes = PendingSiblingRoutes::new();
            let mut pending_prefix_producers = Vec::new();
            let collect_pending_prefix_producers = self.prefix_caches.borrow().states.is_retained();
            let mut prefix_producer_cache = PrefixProducerCache::default();
            let mut prefix_producer_seen = Vec::new();

            // Program and cascade-topology inputs can establish the transaction's outer envelope
            // without routing any DOM input. Do those first: once one proves that the exact plan is
            // the document, no local feature, tree relation, or sequence route can add anything to
            // it. Initial style is the important ordinary case, since a universal rule and all of
            // the document's arriving facts usually share one transaction.
            let mut attachment_groups: Vec<(usize, Vec<TreeScopeID>)> = Vec::new();
            let mut attachment_group_indices: HashMap<(SheetID, bool, bool), usize> = HashMap::default();
            for (input_index, input) in transaction.inputs.iter().enumerate() {
                let InputKey::SheetAttachment(sheet, scope) = input.key else {
                    continue;
                };
                let (InputValue::Flag(previous), InputValue::Flag(current)) = (input.old, input.new) else {
                    unreachable!("a sheet attachment input must carry flags");
                };
                let group_key = (sheet, previous, current);
                let group_index = match attachment_group_indices.get(&group_key) {
                    Some(&group_index) => group_index,
                    None => {
                        let group_index = attachment_groups.len();
                        attachment_groups.push((input_index, Vec::new()));
                        attachment_group_indices.insert(group_key, group_index);
                        group_index
                    }
                };
                attachment_groups[group_index].1.push(scope);
            }
            for (_, scopes) in &mut attachment_groups {
                scopes.sort_unstable();
                scopes.dedup();
            }
            let mut removed_rules_requiring_refresh = Vec::new();
            for input in transaction
                .inputs
                .iter()
                .filter(|input| !matches!(input.key, InputKey::SheetAttachment(..)))
            {
                self.route_program_input(
                    input,
                    transaction.program_joins_for(input.key),
                    &program_delta.arriving_rules,
                    &program_delta.departed_scopes,
                    ProgramRoutingContext {
                        resident_nodes: program_routing_needs_resident_nodes.then_some(resident_nodes.as_slice()),
                        winner_program_version: transaction.program_base_version,
                        document_root: root,
                        attachment_scopes: None,
                        removed_rules_requiring_refresh: &mut removed_rules_requiring_refresh,
                    },
                    &mut regions,
                );
                if regions.covers_document() {
                    break;
                }
            }
            if !regions.covers_document() {
                for (input_index, scopes) in &attachment_groups {
                    let input = &transaction.inputs[*input_index];
                    self.route_program_input(
                        input,
                        transaction.program_joins_for(input.key),
                        &program_delta.arriving_rules,
                        &program_delta.departed_scopes,
                        ProgramRoutingContext {
                            resident_nodes: program_routing_needs_resident_nodes.then_some(resident_nodes.as_slice()),
                            winner_program_version: transaction.program_base_version,
                            document_root: root,
                            attachment_scopes: Some(scopes),
                            removed_rules_requiring_refresh: &mut removed_rules_requiring_refresh,
                        },
                        &mut regions,
                    );
                    if regions.covers_document() {
                        break;
                    }
                }
            }
            if self.selector_truth_changes_active && !removed_rules_requiring_refresh.is_empty() {
                removed_rules_requiring_refresh.sort_unstable();
                removed_rules_requiring_refresh.dedup();
                let refreshes = &mut self.selector_truth_changes.refreshes;
                self.retained_match_answers.for_each_answer_containing_any_rule(
                    &self.match_answers,
                    &removed_rules_requiring_refresh,
                    |node| refreshes.push(SelectorTruthRefresh { node, rule: None }),
                );
            }
            let prefix_producer_admission = if !regions.covers_document() && collect_pending_prefix_producers {
                Some(self.ranked_scope_program(TreeScopeID::DOCUMENT))
            } else {
                None
            };
            if !regions.covers_document() {
                for input in &transaction.inputs {
                    // An arriving ancestor's subtree region already contains every arriving node
                    // below it. Sequence changes inside that new subtree can affect only other new
                    // nodes; facts that can affect an existing relational anchor are routed
                    // separately through each node's arriving-facts key.
                    let nested_tree_arrival = matches!(
                        (input.key, input.old, input.new),
                        (
                            InputKey::TreeRelations(node),
                            InputValue::TreeRelations(None),
                            InputValue::TreeRelations(Some(_))
                        ) if nested_arrivals.binary_search(&node).is_ok()
                    );
                    // A nested arrival's siblings and descendants are inside the outer subtree
                    // already in the plan. Its facts can escape that envelope only by serving as
                    // the witness of a relational selector whose anchor is outside it.
                    let nested_fact_arrival_without_relational_selectors = matches!(
                        input.key,
                        InputKey::LocalFeature(node, LocalFeatureKey::ArrivingFacts)
                            if nested_arrivals.binary_search(&node).is_ok()
                                && self.routing.relational_routes().is_empty()
                    );
                    if nested_tree_arrival || nested_fact_arrival_without_relational_selectors {
                        continue;
                    }
                    self.route_input(
                        input,
                        &program_delta.sheets,
                        &program_delta.selector_programs,
                        tree_routing,
                        &sibling_entries,
                        &mut sibling_candidates,
                        &mut pending_sibling_routes,
                        &mut pending_routes,
                        &mut pending_prefix_producers,
                        prefix_producer_admission.as_ref(),
                        &mut prefix_producer_cache,
                        &mut prefix_producer_seen,
                        &mut sequences,
                        &mut regions,
                    );
                    if regions.covers_document() {
                        break;
                    }
                }
            }
            pending_routes.finish();
            pending_sibling_routes.finish();
            let pending_table_scratch_bytes = (pending_routes.capacity_bytes()
                + pending_sibling_routes.capacity_bytes()
                + pending_prefix_producers.capacity() * size_of::<PendingPrefixProducer>()
                + prefix_producer_cache.capacity_bytes()
                + prefix_producer_seen.capacity() * size_of::<u32>())
                as u64;
            self.memory
                .reserve_required(MemoryCategory::BatchScratch, pending_table_scratch_bytes);
            sequences.finish();
            let sequence_scratch_bytes = sequences.capacity_bytes();
            self.memory
                .reserve_required(MemoryCategory::BatchScratch, sequence_scratch_bytes);
            let mut planning_workspace = ImpactPlanningWorkspace::default();
            let mut deferred_sequence_routes = DeferredSequenceRoutes::default();
            if !regions.covers_document() {
                deferred_sequence_routes = self.route_sequence_changes(
                    &sequences,
                    &program_delta.sheets,
                    tree_routing,
                    &mut regions,
                    &mut planning_workspace,
                );
            }
            if !regions.covers_document() {
                self.route_relational_sequence_changes(&sequences, &mut regions);
            }
            let mut prefix_convergence = PrefixConvergenceOutcome::default();
            if !regions.covers_document() {
                prefix_convergence = self.flush_pending_routes(
                    &mut pending_routes,
                    &mut regions,
                    &mut planning_workspace,
                    retained_winners_are_current,
                    &transaction,
                    &sequences,
                    &pending_prefix_producers,
                );
            }
            self.flush_deferred_sequence_routes(
                deferred_sequence_routes,
                prefix_convergence.positional_routes_are_covered,
                &sequences,
                &mut regions,
                &mut planning_workspace,
            );
            if !regions.covers_document() {
                self.flush_pending_sibling_routes(
                    &mut pending_sibling_routes,
                    &sibling_entries,
                    &mut regions,
                    &mut planning_workspace,
                    prefix_convergence.sibling_routes_are_covered,
                );
            }
            drop(planning_workspace);
            let pending_route_scratch_bytes = pending_routes
                .values()
                .map(|routes| (routes.capacity() * size_of::<ImpactRegion>()) as u64)
                .sum();
            self.memory
                .release(MemoryCategory::BatchScratch, pending_route_scratch_bytes);
            let pending_sibling_scratch_bytes = pending_sibling_routes
                .values()
                .map(|routes| (routes.capacity() * size_of::<ImpactRegion>()) as u64)
                .sum();
            self.memory
                .release(MemoryCategory::BatchScratch, pending_sibling_scratch_bytes);
            self.memory
                .release(MemoryCategory::BatchScratch, pending_table_scratch_bytes);
            self.memory
                .release(MemoryCategory::BatchScratch, sequence_scratch_bytes);
            self.memory.release(MemoryCategory::BatchScratch, arrival_scratch_bytes);
            let mut match_workspace = std::mem::take(&mut self.match_workspace);
            let before = match_workspace.capacity_bytes();
            match_workspace.retain_current_for_matching();
            let after = match_workspace.capacity_bytes();
            self.memory.release(MemoryCategory::BatchScratch, before - after);
            let prepared_match_workspace = (after != 0).then_some(match_workspace);
            if let Some(match_workspace) = prepared_match_workspace {
                let mut prepared = PreparedBatchMatchingTraversal::new(root);
                prepared.reuse_retained_match_answers = reuse_retained_match_answers;
                prepared.match_workspace = match_workspace;
                self.prepared_batch_matching_traversal = Some(prepared);
            }
        }
        let pseudo_inputs_may_have_changed = transaction
            .markers
            .iter()
            .any(|marker| marker.kind == transaction::InputKind::Environment)
            || transaction.inputs.iter().any(|input| {
                matches!(
                    input.key,
                    InputKey::RuleField(_, RuleField::Activation) | InputKey::SheetActivation(_)
                )
            });
        if !transaction.markers.is_empty() {
            // A complete-scope action or coarsened journal proves no narrower output region, so the
            // plan is the document. An environment action still preserves the exact selector state
            // maintained above because it changed no selector input.
            regions.widen_to_document(&mut self.counters);
        }
        if !self.prefix_caches.borrow().states.is_current() {
            self.discard_retained_prefix_caches();
        }
        regions.normalize(&self.tree);
        let impact_region_scratch_bytes = regions.region_index_capacity_bytes();
        let impact_region_scratch = self
            .memory
            .charge_scratch(MemoryCategory::BatchScratch, impact_region_scratch_bytes);
        let patch_cover = retained_answer_patch_selection
            .as_ref()
            .map(|_| regions.compile_patch_cover(&self.tree, Some(root)));
        self.resolve_already_planned_selector_truth(&regions, patch_cover.as_ref().map(|cover| &cover.full));
        let program_base_version = transaction.program_base_version;
        // A scoped plan consumes retained match answers or packs its missing rows adaptively. Do
        // not walk and snapshot the complete required fact store merely to prepare for that bounded
        // traversal. Broad plans will consume the complete batch, so their broad reaction pays the
        // preparation cost once here and reuses it during matching.
        let prepare_complete_matching_batch = regions.regions().iter().any(|region| {
            matches!(region, ImpactRegion::Document | ImpactRegion::TreeScope(_))
                || *region == ImpactRegion::Subtree(root)
        });
        let prepared_matching_batch_is_complete = if !prepare_complete_matching_batch {
            false
        } else {
            let facts = self.facts.primary();
            let mut inspected = 0;
            let mut is_complete = true;
            regions.for_each(&self.tree, Some(root), |node| {
                inspected += 1;
                is_complete &= facts.row_of(node).is_some();
            });
            self.counters
                .add(Counter::PreparedMatchingBatchCompletenessRowsInspected, inspected);
            is_complete
        };
        let fact_view_bytes = self
            .transaction_fact_view
            .as_ref()
            .map_or(0, TransactionFactView::capacity_bytes);
        self.transaction_fact_view = None;
        self.memory.release(MemoryCategory::BatchScratch, fact_view_bytes);
        let plan_is_broad = regions.regions().contains(&ImpactRegion::Document)
            || regions
                .regions()
                .iter()
                .any(|region| matches!(region, ImpactRegion::TreeScope(_)) || *region == ImpactRegion::Subtree(root));
        let mut direct_action_nodes = DeltaBatch::default();
        if !plan_is_broad {
            for node in transaction.inputs.iter().filter_map(|input| input.key.style_node()) {
                direct_action_nodes.push(node);
            }
            direct_action_nodes.consolidate();
        }
        let direct_action_node_bytes = direct_action_nodes.capacity_bytes();
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, direct_action_node_bytes);
        let mut style_input_reactions: Vec<(StyleNodeID, u8, u8)> = transaction
            .inputs
            .iter()
            .filter_map(|input| match (input.key, input.new) {
                (
                    InputKey::ElementStyleInput(node),
                    InputValue::ElementStyleInput {
                        reaction,
                        inherited_style_groups,
                    },
                ) => Some((node, reaction, inherited_style_groups)),
                _ => None,
            })
            .collect();
        style_input_reactions.sort_unstable_by_key(|&(node, _, _)| node);
        let style_input_reaction_bytes = (style_input_reactions.capacity() * size_of::<(StyleNodeID, u8, u8)>()) as u64;
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, style_input_reaction_bytes);
        self.release_transaction_and_sweep_atoms(transaction);
        // Releasing staging can compact primary payloads. Take the shared view afterwards so that
        // compaction does not need to copy the complete primary arrangement away from its view.
        if prepared_matching_batch_is_complete {
            let batch = self.facts.primary_view();
            // NB: This externally visible counter predates shared primary views. It now counts the
            //     rows made available to the prepared batch without implying a physical copy.
            self.counters
                .add(Counter::PreparedMatchingBatchRowsCloned, batch.live_row_count() as u64);
            match &mut self.prepared_batch_matching_traversal {
                Some(prepared) => prepared.batch = Some(batch),
                None => {
                    let mut prepared = PreparedBatchMatchingTraversal::new(root);
                    prepared.batch = Some(batch);
                    prepared.reuse_retained_match_answers = reuse_retained_match_answers;
                    self.prepared_batch_matching_traversal = Some(prepared);
                }
            }
        }
        if plan_is_broad {
            if !transaction_reaches_no_selector {
                self.retained_match_answers.evict(&mut self.match_answers);
            }
            #[cfg(test)]
            if let Some(capture) = &mut self.diagnostic_plan_capture {
                capture.scoped = false;
            }
        }
        let mut retained_answer_patch =
            retained_answer_patch_selection.map(|selection| self.prepare_retained_answer_patch(selection));
        let retained_answer_patch_scratch_bytes = retained_answer_patch
            .as_ref()
            .map_or(0, RetainedAnswerPatch::capacity_bytes);
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, retained_answer_patch_scratch_bytes);
        let published_retained_answer_dispatch = if retained_answer_patch.is_none() && reuse_retained_match_answers {
            self.retained_answer_dispatch_for_traversal(true)
        } else {
            None
        };
        let compiled_regions = regions.compile_union(regions.regions(), &self.tree, Some(root));
        if let Some(base_version) = program_base_version {
            let current_version = self.program.version();
            if regions.covers_document() {
                self.winner_groups.begin_program_version(current_version);
            } else {
                self.winner_groups
                    .advance_program_version_where(base_version, current_version, |node| {
                        !regions.batch_contains_node(&compiled_regions, node)
                    });
            }
        }

        let mut node_count = 0;
        let mut unattributed_node_count = 0;
        let mut published_match_answers = PublishedMatchAnswers::default();
        // A node inside any coarse region was planned without exact selector provenance. Exact
        // node routes consume their signed changes directly; only incomplete routes refresh.
        let mut selector_truth_changes = std::mem::take(&mut self.selector_truth_changes);
        selector_truth_changes.consolidate(&mut self.counters);
        let selector_truth_change_bytes = selector_truth_changes.capacity_bytes();
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, selector_truth_change_bytes);
        // A winner-pruned route preserves this flush's cascade result but makes its exact answer
        // stale. Retire it only after the current batch has been compiled so publication stays put.
        let mut stale_refresh_nodes = Vec::new();
        let mut patch_preserved_nodes: Vec<StyleNodeID> = Vec::new();
        let mut patch_processed_nodes: Vec<StyleNodeID> = Vec::new();
        for refresh in selector_truth_changes.refreshes.as_slice() {
            if !regions.batch_contains_node(&compiled_regions, refresh.node) {
                self.retained_match_answers
                    .forget_answer(&mut self.match_answers, refresh.node);
            } else {
                stale_refresh_nodes.push(refresh.node);
            }
        }
        stale_refresh_nodes.sort_unstable();
        stale_refresh_nodes.dedup();
        let stale_refresh_node_bytes = (stale_refresh_nodes.capacity() * size_of::<StyleNodeID>()) as u64;
        self.memory
            .reserve_required(MemoryCategory::BatchScratch, stale_refresh_node_bytes);
        let mut patch_node_scratch_bytes = 0_u64;
        let mut published_nodes = Vec::new();
        let mut identity_repair_nodes = Vec::new();
        let mut incremental_cascade_answers = Vec::new();
        let mut previous_cascade_inputs = Vec::new();
        let mut exact_cascade_stop_nodes = DeltaBatch::default();
        let mut exact_cascade_confirmation_nodes = DeltaBatch::default();
        let mut attribution_scratch: Vec<(RuleID, EntryID)> = Vec::new();
        let mut attribution_sweep = AttributionSweep::default();
        regions.for_each_batch(&compiled_regions, |node| {
            #[cfg(test)]
            if let Some(capture) = &mut self.diagnostic_plan_capture {
                capture.nodes.push(node.raw());
            }
            let has_direct_action = direct_action_nodes.as_slice().binary_search(&node).is_ok();
            // Capture before the retained-answer patch below replaces the input, but only store
            // the capture when the node is accepted: `previous_cascade_inputs` is read back by
            // position in `published_nodes`, so an entry for a skipped node would shear the two
            // and pair every later node with another node's previous input.
            let previous_cascade_input = self
                .retained_match_answers
                .cascade_input_lookup(node)
                .sparse()
                .ok()
                .copied();
            let has_signed_delta = !selector_truth_changes.deltas_for(node).is_empty();
            let mut has_output_change = false;
            let mut has_upquery = false;
            let mut repair_match_identity = false;
            let mut can_stop_at_exact_cascade = false;
            let mut can_confirm_exact_cascade = false;
            let emit_node = match retained_answer_patch.as_mut() {
                Some(patch) => {
                    let node_is_coarse_covered = patch_cover
                        .as_ref()
                        .is_some_and(|cover| regions.batch_contains_node(&cover.full, node));
                    // `false` means the node's coverage cannot be proven, which only full
                    // re-derivation answers soundly.
                    let attribution_known = patch_cover.as_ref().is_none_or(|cover| {
                        regions.covering_attributions(cover, &mut attribution_sweep, node, &mut attribution_scratch)
                    });
                    let node_deltas = selector_truth_changes.deltas_for(node);
                    let refreshes = selector_truth_changes.refreshes_for(node);
                    let truth_patch = if node_is_coarse_covered {
                        self.counters.bump(Counter::RetainedPatchesCoarseCovered);
                        has_upquery = true;
                        SelectorTruthPatch::Full
                    } else if !attribution_known {
                        has_upquery = true;
                        SelectorTruthPatch::Full
                    } else if refreshes.iter().any(|refresh| refresh.rule.is_none()) {
                        self.counters.bump(Counter::RetainedPatchesPoisoned);
                        has_upquery = true;
                        SelectorTruthPatch::Full
                    } else if !attribution_scratch.is_empty() {
                        has_upquery = true;
                        SelectorTruthPatch::Attributed {
                            deltas: node_deltas,
                            refreshes,
                            rules: &attribution_scratch,
                        }
                    } else if has_signed_delta && refreshes.is_empty() {
                        SelectorTruthPatch::Direct(node_deltas)
                    } else if patch.requires_full_match {
                        has_upquery = true;
                        SelectorTruthPatch::Full
                    } else if !refreshes.is_empty() {
                        has_upquery = true;
                        SelectorTruthPatch::Refresh {
                            deltas: node_deltas,
                            refreshes,
                        }
                    } else if !node_deltas.is_empty()
                        || (patch.always_emit && patch.rules.is_empty())
                        || !patch.cascade_update_properties.is_empty()
                    {
                        SelectorTruthPatch::Direct(node_deltas)
                    } else {
                        self.counters.bump(Counter::RetainedPatchesUnattributed);
                        has_upquery = true;
                        SelectorTruthPatch::Full
                    };
                    let rule_is_safe = |rule: RuleID, entry: EntryID| {
                        let (program, _) = self.programs.entry_location(entry);
                        self.program.declarations_are_complete_for(rule)
                            && self.program.rule_version(rule).selector_program == Some(program)
                            && !self.programs.get(program).contains_relational_selector()
                    };
                    let node_has_safe_exact_cascade_provenance = match truth_patch {
                        SelectorTruthPatch::Full => false,
                        SelectorTruthPatch::Direct(deltas) => {
                            deltas.iter().all(|delta| rule_is_safe(delta.rule, delta.entry))
                        }
                        SelectorTruthPatch::Refresh { deltas, refreshes } => {
                            deltas.iter().all(|delta| rule_is_safe(delta.rule, delta.entry))
                                && refreshes
                                    .iter()
                                    .all(|refresh| refresh.rule.is_some_and(|(rule, entry)| rule_is_safe(rule, entry)))
                        }
                        SelectorTruthPatch::Attributed {
                            deltas,
                            refreshes,
                            rules,
                        } => {
                            deltas.iter().all(|delta| rule_is_safe(delta.rule, delta.entry))
                                && refreshes
                                    .iter()
                                    .all(|refresh| refresh.rule.is_some_and(|(rule, entry)| rule_is_safe(rule, entry)))
                                && rules.iter().all(|&(rule, entry)| rule_is_safe(rule, entry))
                        }
                    };
                    can_confirm_exact_cascade = transaction_supports_retained_cascade_stops
                        && !plan_is_broad
                        && !has_direct_action
                        && node_has_safe_exact_cascade_provenance
                        && self.match_answer_is_comparable_across_elements(node);
                    can_stop_at_exact_cascade = transaction_supports_global_exact_cascade_stops;
                    match self.patch_retained_match_answer(node, patch, truth_patch) {
                        Some(outcome) => {
                            has_output_change = outcome.emit;
                            if outcome.emit {
                                patch_processed_nodes.push(node);
                                if outcome.identity_preserved {
                                    patch_preserved_nodes.push(node);
                                }
                            }
                            if let Some(answer) = outcome.incremental_cascade_answer {
                                incremental_cascade_answers.push(answer);
                            }
                            outcome.emit
                        }
                        None => {
                            self.counters.bump(Counter::RetainedMatchAnswerPatchMisses);
                            repair_match_identity = match_identity_is_complete_output
                                && !patch.always_emit
                                && !patch.orders_shifted
                                && matches!(self.retained_match_answers.cascade_input_lookup(node), Lookup::Known(_));
                            self.retained_match_answers.forget_answer(&mut self.match_answers, node);
                            has_upquery = true;
                            true
                        }
                    }
                }
                None => {
                    if !transaction_reaches_no_selector {
                        self.retained_match_answers.forget_answer(&mut self.match_answers, node);
                        has_upquery = true;
                    }
                    true
                }
            };
            if !emit_node {
                return;
            }
            // A departure can remain in a conservative region while its routing facts survive,
            // and a live shadow root is a synthetic relation node rather than a style output.
            // Neither has a C++ element to consume a record; every live element whose style either
            // can affect is another member of the region.
            let is_scope_root = self.scope_by_root.get(node).is_some();
            if !self.tree.is_live(node) || is_scope_root {
                return;
            }
            if repair_match_identity {
                identity_repair_nodes.push(node);
            }
            if has_direct_action {
                self.counters.bump(Counter::PlannedNodesWithDirectAction);
            }
            if has_signed_delta {
                self.counters.bump(Counter::PlannedNodesWithSignedDelta);
            }
            if has_output_change {
                self.counters.bump(Counter::PlannedNodesWithOutputChange);
            }
            if has_upquery {
                self.counters.bump(Counter::PlannedNodesWithUpquery);
            }
            if !has_direct_action && !has_signed_delta && !has_output_change && !has_upquery {
                self.counters.bump(Counter::PlannedNodesUnattributed);
                unattributed_node_count += 1;
            }
            node_count += 1;
            published_nodes.push(node);
            previous_cascade_inputs.push(previous_cascade_input);
            if can_stop_at_exact_cascade {
                exact_cascade_stop_nodes.push(node);
            }
            if can_confirm_exact_cascade {
                exact_cascade_confirmation_nodes.push(node);
            }
        });
        exact_cascade_stop_nodes.consolidate();
        exact_cascade_confirmation_nodes.consolidate();
        let exact_cascade_stop_node_bytes = exact_cascade_stop_nodes.capacity_bytes();
        let exact_cascade_confirmation_node_bytes = exact_cascade_confirmation_nodes.capacity_bytes();
        self.memory.reserve_required(
            MemoryCategory::BatchScratch,
            exact_cascade_stop_node_bytes + exact_cascade_confirmation_node_bytes,
        );
        #[cfg(test)]
        if self.diagnostic_plan_capture.is_some() {
            // Plan-only fixtures deliberately omit unrelated fact rows. Complete those fixtures
            // after routing, as the browser has before it consumes the already-computed plan.
            let counters = self.counters.clone();
            for node in self.elements_under(root) {
                self.facts.ensure_row(node);
            }
            self.facts.apply_staged(&mut self.memory);
            self.counters = counters;
        }
        let publish_style_answers = true;
        {
            let published_node_bytes = (published_nodes.capacity() * size_of::<StyleNodeID>()) as u64;
            let previous_cascade_input_bytes =
                (previous_cascade_inputs.capacity() * size_of::<Option<MatchAnswerID>>()) as u64;
            let identity_repair_node_bytes = (identity_repair_nodes.capacity() * size_of::<StyleNodeID>()) as u64;
            self.memory
                .reserve_required(MemoryCategory::BatchScratch, published_node_bytes);
            self.memory
                .reserve_required(MemoryCategory::BatchScratch, previous_cascade_input_bytes);
            self.memory
                .reserve_required(MemoryCategory::BatchScratch, identity_repair_node_bytes);
            if publish_style_answers {
                identity_repair_nodes.sort_unstable();
                identity_repair_nodes.dedup();
                incremental_cascade_answers.sort_unstable_by_key(|answer| answer.node);
                let prefer_complete_batch =
                    published_nodes.len().saturating_mul(16) > self.tree.connected_element_count() as usize;
                let reuse_active_batch_matching_traversal =
                    transaction_reaches_no_selector && self.batch_matching_traversal.is_some();
                if !reuse_active_batch_matching_traversal {
                    self.begin_published_match_answer_completion_batch(root, prefer_complete_batch);
                }
                let retained_answer_dispatch = retained_answer_patch
                    .as_ref()
                    .map(|patch| patch.dispatch.as_ref())
                    .or(published_retained_answer_dispatch.as_deref());
                patch_preserved_nodes.sort_unstable();
                patch_preserved_nodes.dedup();
                patch_processed_nodes.sort_unstable();
                patch_processed_nodes.dedup();
                patch_node_scratch_bytes = ((patch_preserved_nodes.capacity() + patch_processed_nodes.capacity())
                    * size_of::<StyleNodeID>()) as u64;
                self.memory
                    .reserve_required(MemoryCategory::BatchScratch, patch_node_scratch_bytes);
                let mut accepted_node_count = 0;
                // Exact retained answers are interned across elements. Once one sufficiently
                // expensive answer has been compacted in this completion batch, other elements
                // without element declarations can share its cascade input and winner identities.
                let mut completed_retained_answers: HashMap<MatchAnswerID, (StyleNodeID, MatchAnswerID, bool)> =
                    HashMap::default();
                let mut completed_retained_answer_bytes = 0_u64;
                let share_cascade_completions = published_nodes.len() >= MIN_SHARED_CASCADE_COMPLETION_BATCH;
                for index in 0..published_nodes.len() {
                    let node = published_nodes[index];
                    let previous_exact_cascade_input = previous_cascade_inputs[index];
                    let retained_cascade_input = self
                        .retained_match_answers
                        .cascade_input_lookup(node)
                        .sparse()
                        .ok()
                        .copied();
                    let previous_cascade_input = identity_repair_nodes
                        .binary_search(&node)
                        .ok()
                        .and(retained_cascade_input);
                    let retained_answer_identity = (share_cascade_completions
                        && retained_answer_dispatch.is_some()
                        && self.match_answer_is_comparable_across_elements(node)
                        && self.has_no_element_declarations(node))
                    .then(|| self.retained_match_answers.answer_identity(node))
                    .flatten();
                    let published_answer = incremental_cascade_answers
                        .binary_search_by_key(&node, |answer| answer.node)
                        .ok()
                        .map(|answer_index| {
                            let answer = &mut incremental_cascade_answers[answer_index];
                            self.counters.bump(Counter::RetainedMatchAnswerReuses);
                            PublishedMatchAnswer {
                                node,
                                cascade_input: Some(answer.cascade_input),
                                matches: answer.matches.take(),
                                cascade_winners_are_complete: answer.cascade_winners_are_complete,
                                observed: false,
                            }
                        })
                        .or_else(|| {
                            if completed_retained_answers.is_empty() {
                                return None;
                            }
                            let identity = retained_answer_identity?;
                            let (source, cascade_input, cascade_winners_are_complete) =
                                completed_retained_answers.get(&identity).copied()?;
                            self.complete_published_match_answer_from_cascade_input(
                                node,
                                source,
                                cascade_input,
                                retained_answer_dispatch.unwrap(),
                                cascade_winners_are_complete,
                            )
                        })
                        .unwrap_or_else(|| {
                            self.complete_published_match_answer(node, retained_answer_dispatch)
                                .expect("a connected style reaction must have complete selector facts")
                        });
                    if let Some(identity) = retained_answer_identity
                        && let Some(cascade_input) = published_answer.cascade_input
                        && !completed_retained_answers.contains_key(&identity)
                        && self.shared_cascade_completion_is_profitable(identity, cascade_input)
                    {
                        let capacity_before = completed_retained_answers.capacity();
                        completed_retained_answers.entry(identity).or_insert((
                            node,
                            cascade_input,
                            published_answer.cascade_winners_are_complete,
                        ));
                        let added_bytes = ((completed_retained_answers.capacity() - capacity_before)
                            * (size_of::<MatchAnswerID>() + size_of::<(StyleNodeID, MatchAnswerID, bool)>() + 1))
                            as u64;
                        self.memory.reserve_required(MemoryCategory::BatchScratch, added_bytes);
                        completed_retained_answer_bytes += added_bytes;
                    }
                    if let Some(retained_cascade_input) = retained_cascade_input
                        && let Some(published_cascade_input) = published_answer.cascade_input
                    {
                        self.counters
                            .bump(Counter::PublishedMatchAnswerRetainedIdentityComparisons);
                        if retained_cascade_input == published_cascade_input {
                            self.counters.bump(Counter::PublishedMatchAnswerRetainedIdentityMatches);
                        }
                    }
                    // A confirmation may stop a reaction only when its proof is strictly cheaper
                    // than the materialization it avoids. The answer above is current either way:
                    // a fresh completion just derived it exactly, and a reused answer is the
                    // output of this flush's retained-answer patch. A complete winner inventory
                    // plus the same live output-identity predicates the exact-cascade stop trusts
                    // therefore prove the stop without re-matching or cloning anything; verify
                    // mode still cold-matches every stopped node.
                    let confirmed_exact_cascade =
                        exact_cascade_confirmation_nodes.as_slice().binary_search(&node).is_ok()
                            && published_answer.cascade_input.is_some_and(|current_cascade_input| {
                                // A patch consumed this node's routed deltas and refreshes under
                                // the confirmation set's rule-safety gating, so its answer is
                                // authoritative. Only a patch MISS leaves completion standing on
                                // maintained state, where the two guards below must decline.
                                let node_was_patched = patch_processed_nodes.binary_search(&node).is_ok();
                                if node_was_patched
                                    && previous_exact_cascade_input == Some(current_cascade_input)
                                    && patch_preserved_nodes.binary_search(&node).is_ok()
                                {
                                    return true;
                                }
                                // Routing flagged this node's truth as unprovable this flush. A
                                // patch consumed the routed refreshes, so only a patch MISS stands
                                // on maintained state here.
                                if !node_was_patched && stale_refresh_nodes.binary_search(&node).is_ok() {
                                    return false;
                                }
                                // Sibling and positional truth is maintained state that no patch
                                // observes; it answers only while the node's own child sequence
                                // went untouched.
                                if (sequence_truth_is_coarse
                                    || self
                                        .tree
                                        .parent(node)
                                        .is_some_and(|parent| sequence_touched_parents.binary_search(&parent).is_ok()))
                                    && self.answer_observes_sibling_relations(current_cascade_input)
                                {
                                    return false;
                                }
                                let whole_inventory_proof = published_answer.cascade_winners_are_complete
                                    && self.exact_cascade_output_is_unchanged(node)
                                    && self.pseudo_cascade_states_are_unchanged(node);
                                whole_inventory_proof
                                    || previous_exact_cascade_input.is_some_and(|previous_cascade_input| {
                                        // Equality proves nothing by itself: a stale answer equals
                                        // itself, and the patch-preserved case confirmed above.
                                        previous_cascade_input != current_cascade_input
                                            && self.answer_transition_cannot_change_cascade(
                                                node,
                                                previous_cascade_input,
                                                current_cascade_input,
                                            )
                                    })
                            });
                    if confirmed_exact_cascade && let Some(current_cascade_input) = published_answer.cascade_input {
                        verify_style_answer_patch(self, |verifier| {
                            verifier.verify_retained_cascade_input(node, current_cascade_input);
                        });
                    }
                    match published_answer {
                        published_answer
                            if previous_cascade_input.is_some()
                                && published_answer.cascade_input == previous_cascade_input =>
                        {
                            self.counters.bump(Counter::PublishedMatchAnswerIdentityRepairs);
                            self.counters.bump(Counter::PublishedMatchAnswerIdentityRepairStops);
                            node_count -= 1;
                        }
                        _ if confirmed_exact_cascade => {
                            self.counters.bump(Counter::PublishedExactCascadeStops);
                            node_count -= 1;
                        }
                        _ if exact_cascade_stop_nodes.as_slice().binary_search(&node).is_ok()
                            && published_answer.cascade_winners_are_complete
                            && self.exact_cascade_output_is_unchanged(node)
                            && self.pseudo_cascade_states_are_unchanged(node) =>
                        {
                            self.counters.bump(Counter::PublishedExactCascadeStops);
                            node_count -= 1;
                        }
                        published_answer => {
                            if previous_cascade_input.is_some() {
                                self.counters.bump(Counter::PublishedMatchAnswerIdentityRepairs);
                                self.counters.bump(Counter::MatchAnswerChanges);
                                self.counters.bump(Counter::PlannedNodesWithOutputChange);
                            }
                            published_nodes[accepted_node_count] = node;
                            accepted_node_count += 1;
                            published_match_answers.push(published_answer, &mut self.memory, &mut self.counters);
                        }
                    }
                }
                self.memory
                    .release(MemoryCategory::BatchScratch, completed_retained_answer_bytes);
                published_nodes.truncate(accepted_node_count);
                if !reuse_active_batch_matching_traversal {
                    self.end_published_match_answer_completion_batch();
                }
            } else {
                published_nodes.clear();
            }
            self.memory
                .release(MemoryCategory::BatchScratch, identity_repair_node_bytes);
            self.memory
                .release(MemoryCategory::BatchScratch, previous_cascade_input_bytes);
        }
        self.memory.release(
            MemoryCategory::BatchScratch,
            exact_cascade_stop_node_bytes + exact_cascade_confirmation_node_bytes,
        );
        self.memory.release(
            MemoryCategory::BatchScratch,
            sequence_touched_parent_bytes + stale_refresh_node_bytes + patch_node_scratch_bytes,
        );
        verify_style_plan_provenance(self, |_| {
            assert!(
                plan_is_broad || unattributed_node_count == 0,
                "a scoped style transaction published {unattributed_node_count} nodes without semantic provenance"
            );
        });
        let final_retained_answer_patch_scratch_bytes = retained_answer_patch
            .as_ref()
            .map_or(0, RetainedAnswerPatch::capacity_bytes);
        if final_retained_answer_patch_scratch_bytes > retained_answer_patch_scratch_bytes {
            self.memory.reserve_required(
                MemoryCategory::BatchScratch,
                final_retained_answer_patch_scratch_bytes - retained_answer_patch_scratch_bytes,
            );
        } else {
            self.memory.release(
                MemoryCategory::BatchScratch,
                retained_answer_patch_scratch_bytes - final_retained_answer_patch_scratch_bytes,
            );
        }
        if retained_answer_patch.take().is_some() {
            self.retain_prefix_states();
        }
        self.memory
            .release(MemoryCategory::BatchScratch, final_retained_answer_patch_scratch_bytes);
        self.memory
            .release(MemoryCategory::BatchScratch, selector_truth_change_bytes);
        self.memory
            .release(MemoryCategory::BatchScratch, direct_action_node_bytes);
        published_match_answers.sort();
        {
            let mut style_deltas = Vec::with_capacity(published_nodes.len());
            let style_delta_bytes = (style_deltas.capacity() * size_of::<PublishedStyleDeltaRecord>()) as u64;
            self.memory
                .reserve_required(MemoryCategory::BridgeBuffer, style_delta_bytes);
            let mut unresolved_inheritance_sources = BitColumn::default();
            let mut unresolved_inheritance_source_bytes = 0;
            for node in published_nodes.iter().copied() {
                let pseudo_inputs_may_have_changed = pseudo_inputs_may_have_changed
                    || !selector_truth_changes.refreshes_for(node).is_empty()
                    || selector_truth_changes
                        .deltas_for(node)
                        .iter()
                        .any(|delta| self.programs.entry(delta.entry).1.pseudo_element.is_some());
                let answer = published_match_answers
                    .lookup(node)
                    .expect("each accepted style reaction has a published match answer");
                let style_input_reaction_index = style_input_reactions
                    .binary_search_by_key(&node, |&(style_node, _, _)| style_node)
                    .ok();
                let (reaction, inherited_style_groups) = style_input_reaction_index
                    .map_or((transaction::STYLE_REACTION_PUBLISHED_STYLE, 0), |index| {
                        (style_input_reactions[index].1, style_input_reactions[index].2)
                    });
                let pseudo_inputs_may_have_changed =
                    pseudo_inputs_may_have_changed || style_input_reaction_index.is_some();
                let old_style_record = self
                    .computed_group_sets
                    .assigned_style_record(node)
                    .map_or(0, |style_record| style_record.raw());
                let direct_inherited_delta = (reaction == transaction::STYLE_REACTION_INHERITED_STYLE)
                    .then(|| self.tree.flat_tree_parent(node))
                    .flatten()
                    .filter(|parent| {
                        parent
                            .element_index()
                            .is_none_or(|index| !unresolved_inheritance_sources.contains(index as usize))
                    })
                    .and_then(|parent| {
                        self.computed_group_sets.replace_engine_resolvable_inherited_groups(
                            node,
                            parent,
                            inherited_style_groups,
                        )
                    });
                if reaction == transaction::STYLE_REACTION_INHERITED_STYLE && direct_inherited_delta.is_none() {
                    let index =
                        node.element_index()
                            .expect("an inherited style reaction targets an element") as usize;
                    let (_, growth) = unresolved_inheritance_sources.set(index, true);
                    self.memory.reserve_required(MemoryCategory::BatchScratch, growth);
                    unresolved_inheritance_source_bytes += growth;
                }
                let (old_style_record, new_style_record, damage, gap) = direct_inherited_delta.map_or(
                    (
                        old_style_record,
                        0,
                        FfiStyleDeltaDamage::None,
                        FfiStyleDeltaGap::Materialize,
                    ),
                    |(old_style_record, new_style_record)| {
                        (
                            old_style_record.raw(),
                            new_style_record.raw(),
                            FfiStyleDeltaDamage::Full,
                            FfiStyleDeltaGap::None,
                        )
                    },
                );
                let style_delta = PublishedStyleDeltaRecord {
                    style_node: node.raw(),
                    match_answer: answer.cascade_input.map_or(0, |cascade_input| cascade_input.0),
                    old_style_record,
                    new_style_record,
                    damage,
                    reaction: reaction
                        | if pseudo_inputs_may_have_changed {
                            transaction::STYLE_REACTION_PSEUDO_INPUTS_MAY_HAVE_CHANGED
                        } else {
                            0
                        },
                    inherited_style_groups,
                    pseudo_kind: u8::MAX,
                    gap,
                };
                style_deltas.push(style_delta);
            }
            if !style_deltas.is_empty() {
                self.settle_computed_memory();
                self.counters
                    .add(Counter::PublishedMatchAnswerRecords, style_deltas.len() as u64);
                emit(transaction_version, program_version, &style_deltas);
            }
            self.memory.release(MemoryCategory::BridgeBuffer, style_delta_bytes);
            self.memory.release(
                MemoryCategory::BatchScratch,
                (published_nodes.capacity() * size_of::<StyleNodeID>()) as u64,
            );
            self.memory
                .release(MemoryCategory::BatchScratch, unresolved_inheritance_source_bytes);
        }
        self.memory
            .release(MemoryCategory::BatchScratch, style_input_reaction_bytes);
        drop(impact_region_scratch);
        published_match_answers.match_element_calls_at_publication = self
            .counters
            .get(Counter::MatchElementCallsDuringPublishedStyleTransaction);
        published_match_answers.discard_unobserved_retained_answers = publish_document_root_arrival || plan_is_broad;
        self.published_match_answers = published_match_answers;
        if initial_tree_was_bulk_loaded {
            self.counters.bump(Counter::InitialBulkMatchLoads);
            self.counters.add(Counter::InitialBulkMatchRows, node_count);
        }
        self.counters.add(Counter::InvalidatedStyleNodes, node_count);
        if node_count == 0 {
            self.discard_prepared_batch_matching_traversal();
            self.memory.release(MemoryCategory::BatchScratch, topology_bytes);
        } else {
            // C++ may propagate a changed inherited output below the nodes named by the plan. Those
            // descendants did not receive a selector delta, so their retained answers are still
            // current and must be consumed instead of matching them again.
            match &mut self.prepared_batch_matching_traversal {
                Some(prepared) => prepared.reuse_retained_match_answers = true,
                None => {
                    let mut prepared = PreparedBatchMatchingTraversal::new(root);
                    prepared.reuse_retained_match_answers = true;
                    self.prepared_batch_matching_traversal = Some(prepared);
                }
            }
            if !self.prepare_topology_for_matching(root, &mut regions) {
                self.memory.release(MemoryCategory::BatchScratch, topology_bytes);
            }
        }
        !publish_document_root_arrival && !plan_is_broad
    }

    pub(super) fn prepare_topology_for_matching(&mut self, root: StyleNodeID, regions: &mut ImpactRegions) -> bool {
        let Some(topology) = regions.take_topology() else {
            return false;
        };
        match &mut self.prepared_batch_matching_traversal {
            Some(prepared) => {
                debug_assert_eq!(prepared.root, root);
                prepared.topology = Some(topology);
            }
            None => {
                let mut prepared = PreparedBatchMatchingTraversal::new(root);
                prepared.topology = Some(topology);
                self.prepared_batch_matching_traversal = Some(prepared);
            }
        }
        true
    }

    /// The feature the input is about, when the changed node is the one that carries it.
    ///
    /// Postings answer "which elements carry this feature *now*", and by routing time the delta has
    /// already been applied. So a class being removed makes the element that lost it fail its own
    /// subject check, and the change reaches nothing at all. The feature in flux counts as carried
    /// by the node it changed on, whichever direction it moved.
    pub(super) fn feature_in_flux(input: &NormalizedInput) -> Option<(StyleNodeID, DispatchKey)> {
        let InputKey::LocalFeature(node, feature) = input.key else {
            return None;
        };
        let key = match feature {
            LocalFeatureKey::Class(atom) => DispatchKey::Class(atom),
            LocalFeatureKey::Part(atom) => DispatchKey::Part(atom),
            LocalFeatureKey::CustomState(atom) => DispatchKey::CustomState(atom),
            // Emptiness is not a feature a compound dispatches on, so nothing is in flux for it.
            LocalFeatureKey::Emptiness => return None,
            LocalFeatureKey::Attribute(atom) => DispatchKey::AttributeName(atom),
            LocalFeatureKey::Id => match (input.old, input.new) {
                (InputValue::Feature(FeatureValue::Atom(atom)), _)
                | (_, InputValue::Feature(FeatureValue::Atom(atom))) => DispatchKey::Id(atom),
                _ => return None,
            },
            // A resolved language or directionality carries its own value, like an ID.
            // Neither a language nor a part exposure is dispatched on its value, so nothing is in
            // flux for either.
            LocalFeatureKey::Language | LocalFeatureKey::PartExposure | LocalFeatureKey::HeadingLevel => return None,
            LocalFeatureKey::Directionality => match (input.old, input.new) {
                (InputValue::Feature(FeatureValue::Atom(atom)), _)
                | (_, InputValue::Feature(FeatureValue::Atom(atom))) => DispatchKey::Directionality(atom),
                _ => return None,
            },
            // A tag never changes, so it is never in flux. Neither is the folded key an arrival's
            // facts are journalled under: every fact it stands for holds on the element now.
            LocalFeatureKey::TagName | LocalFeatureKey::FoldedTagName | LocalFeatureKey::ArrivingFacts => return None,
        };
        Some((node, key))
    }

    /// Every local feature the transaction moves, as every dispatch key a compound can name it by.
    pub(super) fn feature_delta_for(&self, transaction: &StyleTransaction) -> FeatureFluxColumn {
        let mut entries = Vec::new();
        for input in &transaction.inputs {
            let Some((node, key)) = Self::feature_in_flux(input) else {
                continue;
            };
            entries.push((node, key));
            if let DispatchKey::AttributeName(name) = key {
                for name in self.facts.attribute_name_keys(name) {
                    entries.push((node, DispatchKey::AttributeName(name)));
                }
            }
        }
        FeatureFluxColumn::from_entries(entries)
    }

    /// The keys this transaction is moving on one node, which routing has to name it by as well as
    /// by the keys the facts still say it carries.
    pub(super) fn moved_features_of(&self, node: StyleNodeID) -> &[DispatchKey] {
        self.transaction_fact_view
            .as_ref()
            .map_or(&[], |view| view.moved_features.keys_of_node(node))
    }

    #[must_use]
    /// Materialize old child sequences directly from the tree family's frozen before rows.
    pub(super) fn install_before_sibling_geometry(&self, view: &mut TransactionFactView) -> bool {
        let staged_rows = self.tree_staging.rows();
        if staged_rows.is_empty() {
            view.finish_before_sibling_relations();
            return true;
        }

        let mut parents = Vec::new();
        for &(_, before, after) in &staged_rows {
            if let Some(relations) = before {
                parents.extend(relations.parent);
            }
            if let Some(relations) = after {
                parents.extend(relations.parent);
            }
        }
        parents.extend(
            self.tree_staging
                .first_children()
                .into_iter()
                .map(|(parent, _, _)| parent),
        );
        parents.sort_unstable();
        parents.dedup();

        for &(node, before, _) in &staged_rows {
            if before.is_none() {
                view.mark_before_absent(node);
            }
        }

        let maximum_sequence_length = self.tree.connected_element_count() as usize + staged_rows.len() + 1;
        for parent in parents {
            let resident_first = self
                .tree
                .is_live(parent)
                .then(|| self.tree.first_element_child(parent))
                .flatten();
            let mut child = self
                .tree_staging
                .before_first_child(parent, resident_first)
                .or_else(|| {
                    staged_rows.iter().find_map(|&(node, before, _)| {
                        before
                            .is_some_and(|relations| {
                                relations.parent == Some(parent) && relations.previous_element_sibling.is_none()
                            })
                            .then_some(node)
                    })
                });
            let mut sequence = Vec::new();
            while let Some(node) = child {
                assert!(
                    sequence.len() < maximum_sequence_length,
                    "frozen before-side child sequence must be acyclic"
                );
                sequence.push(node);
                let resident = self.tree.is_live(node).then(|| self.settled_tree_relations(node));
                child = self
                    .tree_staging
                    .before_relations(node, resident)
                    .and_then(|relations| relations.next_element_sibling);
            }
            view.insert_before_sibling_sequence(parent, sequence);
        }
        view.finish_before_sibling_relations();
        true
    }
}
