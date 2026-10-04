/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

/// A style pass the host installs in waves. The pass settles its rows in the order the host applies
/// them, and a wave stops before the first row that reads a row of the wave only the host settles: a
/// record the host computes, or one it composes as it installs it. The host installs the rows before
/// that one and takes the next wave of the same pass, which settles the row over the installed rows
/// under the facts of the transaction that planned it.
pub(super) struct StylePass {
    transaction_version: StyleTransactionVersion,
    program_version: ProgramVersion,
    scoped: bool,
    published_nodes: Vec<StyleNodeID>,
    previous_cascade_inputs: Vec<Option<MatchAnswerID>>,
    style_input_reactions: Vec<(StyleNodeID, u8, u8)>,
    selector_truth_changes: SelectorTruthChanges,
    published_match_answers: PublishedMatchAnswers,
    rule_declarations_edited: bool,
    pseudo_inputs_may_have_changed: bool,
    named_rules_moved: NamedRuleContextsMoved,
    inputs_moved: RowInputsMoved,
    hidden_style_readers: HashSet<StyleNodeID>,
    /// The reactions rows of the pass derived for children that are rows of it too, which join the
    /// child's own reaction.
    derived_child_reactions: HashMap<StyleNodeID, (u8, u8)>,
    scratch: publication::EngineComputedRecordScratch,
    /// The first row the host has not been given yet.
    next_index: usize,
}

/// What moved under the records of rows that no winner of theirs shows, each recorded beside the
/// style input the row owes. A transaction takes them with its rows, and a pass that gives a row
/// up gives them back with it.
#[derive(Default)]
pub(super) struct RowInputsMoved {
    /// The rows whose containers moved under what their queries or container-relative lengths read
    /// of them.
    containers: HashSet<StyleNodeID>,
    /// The rows whose parent's display, which their box-type transformation reads, moved.
    parent_display: HashSet<StyleNodeID>,
    /// The rows a pass gave up before it reached them. What moved under them that only its
    /// transaction saw, such as the viewport, or the answers and winners it installed for them,
    /// no later transaction sees.
    given_up: HashSet<StyleNodeID>,
}

impl RowInputsMoved {
    pub(super) fn containers_moved(&self, node: StyleNodeID) -> bool {
        self.containers.contains(&node)
    }

    pub(super) fn parent_display_moved(&self, node: StyleNodeID) -> bool {
        self.parent_display.contains(&node)
    }

    pub(super) fn given_up(&self, node: StyleNodeID) -> bool {
        self.given_up.contains(&node)
    }

    pub(super) fn note_containers_moved(&mut self, node: StyleNodeID) {
        self.containers.insert(node);
    }

    pub(super) fn note_parent_display_moved(&mut self, node: StyleNodeID) {
        self.parent_display.insert(node);
    }

    pub(super) fn forget(&mut self, node: StyleNodeID) {
        self.containers.remove(&node);
        self.parent_display.remove(&node);
        self.given_up.remove(&node);
    }

    /// Move what moved under `node` from `owed` to these.
    fn join(&mut self, owed: &mut Self, node: StyleNodeID) {
        if owed.containers.remove(&node) {
            self.containers.insert(node);
        }
        if owed.parent_display.remove(&node) {
            self.parent_display.insert(node);
        }
        if owed.given_up.remove(&node) {
            self.given_up.insert(node);
        }
    }

    /// Give the `rows` a pass did not reach back to `owed`, given up, with what moved under them.
    fn give_back(mut self, rows: &[StyleNodeID], owed: &mut Self) {
        for &node in rows {
            owed.join(&mut self, node);
            owed.given_up.insert(node);
        }
    }
}

/// One clock shared by every phase; rounded cumulative endpoints make the intervals additive.
/// It owns no engine borrow, and is read only for diagnostics.
struct TransactionClock {
    started_at: std::time::Instant,
    elapsed_microseconds: u64,
    phase: Counter,
}

impl TransactionClock {
    fn new() -> Self {
        Self {
            started_at: std::time::Instant::now(),
            elapsed_microseconds: 0,
            phase: Counter::CommitMicroseconds,
        }
    }

    fn enter(&mut self, phase: Counter, counters: &mut Counters) {
        let elapsed = self.started_at.elapsed().as_micros().min(u128::from(u64::MAX)) as u64;
        counters.add(self.phase, elapsed - self.elapsed_microseconds);
        self.elapsed_microseconds = elapsed;
        self.phase = phase;
    }

    fn finish(mut self, counters: &mut Counters) {
        self.enter(Counter::TransactionRemainderMicroseconds, counters);
        counters.add(Counter::TransactionMicroseconds, self.elapsed_microseconds);
    }
}

const MIN_SHARED_CASCADE_COMPLETION_BATCH: usize = 16;

/// A scoped timer for one pass of the transaction, charged to its own counter.
///
/// The four phase clocks say which quarter of the transaction a millisecond is in; these say
/// which *pass*, which is what decides whether a pass is per-node work or bookkeeping around it.
/// Two monotonic reads per pass and a dozen passes per flush is real money on a document that
/// flushes thousands of times, so the instrument is off unless `LIBWEB_STYLE_PASS_CLOCKS` is set,
/// and costs one cached environment test per pass when it is not: a diagnostic costs nothing when
/// it is not read.
pub(super) struct PassTimer(Option<std::time::Instant>);

fn pass_clocks_are_enabled() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ENABLED.get_or_init(|| std::env::var_os("LIBWEB_STYLE_PASS_CLOCKS").is_some())
}

impl PassTimer {
    pub(super) fn start() -> Self {
        Self(pass_clocks_are_enabled().then(std::time::Instant::now))
    }

    pub(super) fn stop(self, counter: Counter, counters: &mut Counters) {
        if let Some(started_at) = self.0 {
            counters.add(
                counter,
                started_at.elapsed().as_micros().min(u128::from(u64::MAX)) as u64,
            );
        }
    }
}

/// The named rule contexts a transaction moved. Each reaches a record by a route of its own.
#[derive(Clone, Copy, Default)]
struct NamedRuleContextsMoved {
    counter_styles: bool,
    custom_functions: bool,
    font_feature_values: bool,
}

impl NamedRuleContextsMoved {
    fn any(self) -> bool {
        self.counter_styles || self.custom_functions || self.font_feature_values
    }
}

impl RetainedState {
    /// Whether a moved named rule context drives the node's record again in full, its winners
    /// standing as they were: a `@function` reaches an element whose style called one, and a
    /// counter style a record that resolved one, which the engine resolves against the published
    /// definitions and registry.
    fn named_rule_contexts_drive_in_full(&self, node: StyleNodeID, moved: NamedRuleContextsMoved) -> bool {
        (moved.custom_functions && self.facts.uses_custom_functions(node))
            || (moved.counter_styles && self.node_reads_counter_styles(node))
    }

    /// Why the row's reaction, the moved named rule contexts and what moved under the row drive its
    /// record again in full without its winners showing it, if they do.
    fn full_drive_reason(
        &self,
        node: StyleNodeID,
        reaction: u8,
        named_rules_moved: NamedRuleContextsMoved,
        inputs_moved: &RowInputsMoved,
    ) -> Option<publication::FullDriveReason> {
        use super::publication::FullDriveReason;
        if inputs_moved.given_up(node) {
            return Some(FullDriveReason::PassGivenUp);
        }
        // A descendant recompute stands for inputs no winner shows: the root's font metrics, an
        // ancestor's direction, writing mode or container type. An ancestor becoming visible
        // stands for a record whose style was cleared on entry to display:none.
        if reaction
            & (transaction::STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES
                | transaction::STYLE_REACTION_ANCESTOR_BECAME_VISIBLE)
            != 0
        {
            return Some(FullDriveReason::AncestorChange);
        }
        if self.named_rule_contexts_drive_in_full(node, named_rules_moved) {
            return Some(FullDriveReason::NamedRuleContext);
        }
        // The verdicts of the node's gated rules are decided again where the record is driven;
        // what a value measures of the containers is not.
        if inputs_moved.containers_moved(node) && self.container_input_drives_in_full(node) {
            return Some(FullDriveReason::ContainerMoved);
        }
        // The element's font environment moved, or the published feature-value table every font
        // resolution of the font-environment generation reads.
        (named_rules_moved.font_feature_values || reaction & transaction::STYLE_REACTION_FONT_INPUTS_CHANGED != 0)
            .then_some(FullDriveReason::FontEnvironment)
    }

    /// Whether a row needs no style because it is in a display:none subtree: the host holds no
    /// style for the node, and the nearest ancestor it holds one for once it applied the rows
    /// before this one, the record this flush settled for it or the one it holds, is in that
    /// subtree, or the flush answered an ancestor in between as hidden. A read or the subtree's
    /// reveal asks for the node's record.
    fn row_is_hidden(
        &self,
        node: StyleNodeID,
        row_of: impl Fn(StyleNodeID) -> Option<publication::DerivedChildInputs>,
    ) -> bool {
        if self.held_style_records.contains_key(&node) {
            return false;
        }
        let mut ancestor = self.tree.inheritance_parent(node);
        while let Some(current) = ancestor {
            let row = row_of(current);
            if row.is_some_and(|row| row.hidden) {
                return true;
            }
            let record = if row.is_some_and(|row| row.settled) {
                self.computed_group_sets
                    .assigned_style_record(current)
                    .map(computed::FinalStyleRecordID::raw)
            } else {
                self.held_style_records.get(&current).copied()
            };
            if let Some(record) = record {
                return self.record_is_in_display_none_subtree(record);
            }
            ancestor = self.tree.inheritance_parent(current);
        }
        false
    }

    /// Note what the record loop reads of the answer a transaction publishes for the node before that
    /// answer is installed: its pseudo-element inventory, kept with the node's record columns for as
    /// long as that answer stands, the answer's completeness and the matches its custom-property
    /// cascade runs over.
    fn note_batch_answer_facts(&mut self, node: StyleNodeID, answers: &PublishedMatchAnswers) {
        let Some(answer) = answers.lookup(node) else {
            return;
        };
        let mask = self.answer_pseudo_style_mask(answer);
        self.computed_group_sets.set_node_pseudo_style_mask(node, mask);
        if self.any_custom_property_is_declared()
            && let Some(matches) = self.batch_custom_property_matches_of(answers, answer)
        {
            self.batch_custom_property_matches.insert(node, matches);
        }
        if let Some(matches) = self.batch_backing_pseudo_matches_of(node, answers, answer) {
            self.batch_backing_pseudo_matches.insert(node, matches);
        }
        if let Some(complete) = self.answer_is_complete_but_for_custom_properties(node, answers, answer) {
            self.batch_answers_complete_but_for_custom_properties
                .insert(node, complete);
        }
    }

    /// Forget what the record loop read of the answers of a pass that has settled its last row.
    fn clear_batch_answer_facts(&mut self) {
        self.batch_answers_complete_but_for_custom_properties.clear();
        self.batch_custom_property_matches.clear();
        self.batch_backing_pseudo_matches.clear();
    }

    /// Whether the node's record, or the record of one of its pseudo-elements, resolved a counter
    /// style: a `content` counter or a `list-style-type` naming one that can be overridden.
    fn node_reads_counter_styles(&self, node: StyleNodeID) -> bool {
        let reads = |record: Option<computed::FinalStyleRecordID>| {
            record
                .and_then(|record| self.computed_group_sets.style_record_view(record.raw()))
                .is_some_and(|view| view.counter_style_environment_identity != 0)
        };
        reads(self.computed_group_sets.assigned_style_record(node))
            || self
                .computed_group_sets
                .assigned_pseudo_kinds(node)
                .any(|kind| reads(self.computed_group_sets.pseudo_style_record(node, kind)))
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
        let (node, feature) = match input.key {
            InputKey::LocalFeature(node, feature) => (node, feature),
            InputKey::State(node, fact) => {
                return fact.has_selector_posting().then_some((node, DispatchKey::State(fact)));
            }
            _ => return None,
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
}

impl StyleEngineState {
    fn take_style_transaction_with_clock(
        &mut self,
        root: StyleNodeID,
        mut emit: impl FnMut(StyleTransactionVersion, ProgramVersion, &[PublishedStyleDeltaRecord]),
        clock: &mut TransactionClock,
        counters: &mut Counters,
    ) -> bool {
        // The previous transaction's uninstalled records can no longer be consumed. Revert
        // them before this transaction publishes anything: a later C++ computation can install
        // the same record, which must not then be mistaken for an unconsumed derivation.
        self.discard_engine_computed_records(counters);
        self.reclaim_computed_memory_if_needed(counters);
        self.sync_tier3_benefit_observations(counters);
        let tier3_evictions = self.retained.memory.finish_tier3_quota_period();
        for &category in &TIER3_REFUSAL_CATEGORIES {
            if tier3_evictions[category as usize] && self.evict_tier3_category(category) {
                counters.bump(Counter::Tier3BenefitEvictions);
            }
        }
        self.retained.memory.begin_tier3_quota_period();
        self.retained.refresh_admission_facts();
        self.retained.winner_groups.begin_quota_period();
        self.retained.flush_stamp += 1;
        self.retained.winner_groups.begin_flush(self.retained.flush_stamp);
        self.retained.relational_witnesses.set_admitting(true);
        self.retained
            .route_pruning_states
            .lock()
            .expect("the route-pruning memo is never held across a panic")
            .clear();
        #[cfg(test)]
        if let Some(capture) = &mut self.retained.diagnostic_plan_capture {
            capture.nodes.clear();
            capture.scoped = true;
        }
        self.discard_prepared_batch_matching_traversal();
        self.discard_published_match_answers(counters);
        // Winners whose container verdicts moved since they were published are published again
        // from the nodes' retained answers, or dropped under a changing rule program: an answer
        // can name a rule this transaction removes or replaces.
        self.retained.refresh_winners_whose_container_verdicts_moved(
            self.host.program_staging.is_dirty() || self.host.sheet_rule_replacement.is_some(),
            publication::WinnerRepublication::for_flush(),
            counters,
        );
        // A transaction made of derived child reactions alone continues the style change whose
        // reactions C++ applied last, one tree generation further.
        self.retained.last_transaction_only_derived_child_reactions =
            self.host.deferred_element_style_inputs_are_pending
                && !self.host.deferred_element_style_inputs.is_empty()
                && self.host.journal.is_empty()
                && self.host.tree_staging.is_empty()
                && !self.host.program_staging.is_dirty()
                && self.host.sheet_rule_replacement.is_none();
        for input in std::mem::take(&mut self.host.deferred_element_style_inputs) {
            self.record_input(input.key, input.old, input.new, counters);
        }
        self.host.deferred_element_style_inputs_are_pending = false;
        let inputs_moved = std::mem::take(&mut self.retained.row_inputs_moved);
        self.host
            .deferred_element_style_input_memory
            .resize_required_to(&mut self.retained.memory, 0);
        let document_root_arrival_is_pending =
            self.host.journal.pending_old(InputKey::TreeRelations(root)) == Some(InputValue::TreeRelations(None));
        let initial_tree_was_bulk_loaded =
            self.host.initial_tree_bulk_load_is_pending && document_root_arrival_is_pending;
        let publish_document_root_arrival = document_root_arrival_is_pending;

        // Whatever the last flush's late exact asks left in the flush-local workspace was measured
        // in the previous topology. Every exact evaluation below reads current-side sibling
        // geometry from it, so it starts empty here, before this transaction's tree is applied.
        let stale_match_workspace_bytes = self.retained.match_workspace.capacity_bytes();
        self.retained.match_workspace = MatchScratch::default();
        self.retained
            .memory
            .release(MemoryCategory::BatchScratch, stale_match_workspace_bytes);
        let mut transaction = self.drain_transaction(counters);
        self.apply_staged_transaction(&mut transaction, counters);
        self.retained.program.share_rule_storage();
        self.retained.native_rules.targets.share();
        self.retained.programs.share_indices(&mut self.retained.memory);
        if transaction.is_empty() {
            self.release_transaction_and_sweep_atoms(transaction, counters);
            clock.enter(Counter::TransactionRemainderMicroseconds, counters);
            return true;
        }
        // Routing invokes exact planning and prefix matching inline; separating those clocks
        // would require per-node timers or moving work.
        clock.enter(Counter::RoutingPlanningMicroseconds, counters);
        let routing_setup_timer = PassTimer::start();
        let preserves_selector_incidence = !transaction.has_coarsened_markers()
            && transaction.inputs.iter().all(|input| {
                matches!(
                    input.key,
                    InputKey::RuleField(_, RuleField::Activation | RuleField::Declarations | RuleField::Layer)
                        | InputKey::SheetActivation(_)
                        | InputKey::CustomPropertyRegistration(_)
                        | InputKey::CascadeTopology(_)
                        | InputKey::ElementDeclaration(..)
                        | InputKey::ElementStyleInput(_)
                )
            });
        if !preserves_selector_incidence {
            self.retained.retained_selector_incidences.clear();
        }
        self.retained.selector_incidence_is_current = preserves_selector_incidence;
        let transaction_version = self.retained.next_style_transaction_version;
        self.retained.next_style_transaction_version = StyleTransactionVersion(
            self.retained
                .next_style_transaction_version
                .0
                .checked_add(1)
                .expect("style transaction version space exhausted"),
        );
        let program_version = self.retained.program.version();
        self.retained.selector_truth_changes = SelectorTruthChanges::default();
        self.retained.already_planned_selector_truth = DeltaBatch::default();
        self.retained.selector_truth_changes_active = false;
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
                        | InputKey::CustomPropertyRegistration(_)
                )
            });
        let match_identity_is_complete_output = !transaction.has_coarsened_markers()
            && transaction.inputs.iter().all(|input| {
                !matches!(
                    input.key,
                    InputKey::RuleField(_, RuleField::Declarations | RuleField::Layer) | InputKey::CascadeTopology(_)
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
                        .all(|affected| self.retained.program.declarations_are_complete_for(affected.rule))
                    && selection.affected.iter().all(|affected| {
                        self.retained
                            .program
                            .rule_version(affected.rule)
                            .selector_program
                            .is_some_and(|program| !self.retained.programs.get(program).contains_relational_selector())
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
        self.retained.selector_truth_changes_active = retained_answer_patch_selection.is_some();
        let reuse_retained_match_answers = transaction_reaches_no_selector || retained_answer_patch_selection.is_some();
        if reuse_retained_match_answers {
            let mut prepared = PreparedBatchMatchingTraversal::new(root);
            prepared.reuse_retained_match_answers = true;
            self.retained.prepared_batch_matching_traversal = Some(prepared);
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
            self.retained
                .retained_match_answers
                .evict(&mut self.retained.match_answers);
            self.release_transaction_and_sweep_atoms(transaction, counters);
            routing_setup_timer.stop(Counter::RoutingSetupMicroseconds, counters);
            clock.enter(Counter::TransactionRemainderMicroseconds, counters);
            return false;
        }

        // A normalized transaction this wide will ask enough tree-shaped questions to amortize one
        // preorder pass. Keep the coordinates in Tier-4 scratch and let smaller updates continue to
        // walk the mandatory relation columns directly.
        const TRANSACTION_TOPOLOGY_MINIMUM_INPUTS: usize = 32;
        let mut regions = if transaction.inputs.len() >= TRANSACTION_TOPOLOGY_MINIMUM_INPUTS {
            let regions = ImpactRegions::with_topology(&self.retained.tree, root);
            let bytes = regions.topology_capacity_bytes();
            self.retained
                .memory
                .reserve_required(MemoryCategory::BatchScratch, bytes);
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
                        self.retained
                            .tree
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
            regions.add(ImpactRegion::Subtree(arrival));
        }
        // An arriving subtree's own retained answers date from before it detached, and facts can
        // have moved off the journal's books while it was out; they must match cold rather than
        // be patched. A subtree that crossed parents within one flush is not an arrival, its
        // relations fold to two connected sides, but its ancestor context moved just the same
        // and nothing else re-proves its answers: the DOM side restyles it directly and the
        // traversal would otherwise reuse the answers from its old place.
        if retained_answer_patch_selection.is_some() {
            for &arrival in &outer_arrivals {
                for node in self.retained.tree.preorder(arrival) {
                    self.retained
                        .retained_match_answers
                        .forget(&mut self.retained.match_answers, node);
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
                    for node in self.retained.tree.preorder(mover) {
                        self.retained
                            .retained_match_answers
                            .forget_answer(&mut self.retained.match_answers, node);
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
                    if let Some(parent) = self.retained.tree.parent(node) {
                        sequence_touched_parents.push(parent);
                    }
                }
            }
            sequence_touched_parents.sort_unstable();
            sequence_touched_parents.dedup();
        }
        let sequence_touched_parent_bytes = (sequence_touched_parents.capacity() * size_of::<StyleNodeID>()) as u64;
        self.retained
            .memory
            .reserve_required(MemoryCategory::BatchScratch, sequence_touched_parent_bytes);
        let connected_element_count = self.retained.tree.connected_element_count() as usize;
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
        self.retained.transaction_fact_view = Some(transaction_fact_view);
        let mut caches = self.retained.prefix_caches.borrow_mut();
        caches.states.mark_previous();
        // The relation maintains document-scoped prefixes. Incomplete transitions and
        // tag changes, whose of-type effects extend to siblings, require cold reconstruction.
        if caches.states.has_relation() {
            let mut reconstruct = self
                .transaction_fact_view
                .as_ref()
                .is_none_or(|view| view.prefix.is_none());
            let mut confined_elsewhere = false;
            for input in &transaction.inputs {
                if reconstruct {
                    break;
                }
                let (tree_scope, maintained) = match input.key {
                    InputKey::LocalFeature(node, LocalFeatureKey::TagName | LocalFeatureKey::FoldedTagName) => {
                        (self.retained.tree.tree_scope(node), false)
                    }
                    key => match key.style_node() {
                        Some(node) => (self.retained.tree.tree_scope(node), true),
                        None => (key.program_tree_scope().unwrap_or(TreeScopeID::DOCUMENT), false),
                    },
                };
                if tree_scope != TreeScopeID::DOCUMENT {
                    confined_elsewhere = true;
                } else if !maintained {
                    reconstruct = true;
                }
            }
            if reconstruct {
                caches.states.release();
                caches.answers.release(&mut self.retained.match_answers);
            } else if confined_elsewhere {
                // The other scope's own retained transitions are not maintained.
                caches.states.release_transition_states();
                caches.answers.release(&mut self.retained.match_answers);
            }
        }
        drop(caches);
        let fact_view_bytes = self
            .transaction_fact_view
            .as_ref()
            .map_or(0, TransactionFactView::capacity_bytes);
        self.retained
            .memory
            .reserve_required(MemoryCategory::BatchScratch, fact_view_bytes);
        let mut sequences = SequenceChanges::new();
        if !transaction.has_coarsened_markers() {
            // Routing reads the program as it stands now, but the inputs happened before it did.
            // A sheet that went away in this same transaction was still deciding when the mutations
            // ahead of it in the journal were recorded, so its rules cannot be skipped as inactive.
            let program_delta = self.host.program_staging.delta();
            let retained_winners_are_current = transaction
                .inputs
                .iter()
                .all(|input| matches!(input.key, InputKey::LocalFeature(..) | InputKey::State(..)));
            if self.retained.selector_incidence_is_current {
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
                self.retain_selector_incidences(&programs, root, counters);
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
            self.retained
                .memory
                .reserve_required(MemoryCategory::BatchScratch, arrival_scratch_bytes);
            let tree_routing = TreeRoutingMode {
                use_exact: use_exact_tree_routing,
                has_before_sibling_relations,
                transaction_inputs: &transaction.inputs,
            };
            Arc::get_mut(&mut self.retained.routing)
                .expect("routing program is shared outside a planning epoch")
                .prepare_route_liveness(&self.retained.program, &self.retained.programs);
            let routing_for_siblings = Arc::clone(&self.retained.routing);
            let sibling_entries = routing_for_siblings.live_sibling_entries(&self.retained.program);
            let mut sibling_candidates = routing_for_siblings.live_sibling_workspace(&self.retained.program);
            let mut pending_routes = PendingRoutes::new();
            let mut pending_sibling_routes = PendingSiblingRoutes::new();
            let mut pending_prefix_producers = Vec::new();
            let collect_pending_prefix_producers = self.retained.prefix_caches.borrow().states.is_retained();
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
            routing_setup_timer.stop(Counter::RoutingSetupMicroseconds, counters);
            let routing_inputs_timer = PassTimer::start();
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
                    counters,
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
                        counters,
                    );
                    if regions.covers_document() {
                        break;
                    }
                }
            }
            if self.retained.selector_truth_changes_active && !removed_rules_requiring_refresh.is_empty() {
                removed_rules_requiring_refresh.sort_unstable();
                removed_rules_requiring_refresh.dedup();
                let refreshes = &mut self.retained.selector_truth_changes.refreshes;
                self.retained
                    .retained_match_answers
                    .for_each_answer_containing_any_rule(
                        &self.retained.match_answers,
                        &removed_rules_requiring_refresh,
                        |node| refreshes.push(SelectorTruthRefresh { node, rule: None }),
                    );
            }
            let prefix_producer_admission = if !regions.covers_document() && collect_pending_prefix_producers {
                Some(self.prepare_scope_program(TreeScopeID::DOCUMENT))
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
                                && self.retained.routing.relational_routes().is_empty()
                    );
                    if nested_tree_arrival || nested_fact_arrival_without_relational_selectors {
                        continue;
                    }
                    self.route_input(
                        input,
                        &program_delta.sheets,
                        &program_delta.selector_programs,
                        tree_routing,
                        sibling_entries,
                        &mut sibling_candidates,
                        &mut pending_sibling_routes,
                        &mut pending_routes,
                        &mut pending_prefix_producers,
                        prefix_producer_admission.as_ref(),
                        &mut prefix_producer_seen,
                        &mut sequences,
                        &mut regions,
                        counters,
                    );
                    if regions.covers_document() {
                        break;
                    }
                }
            }
            routing_inputs_timer.stop(Counter::RoutingInputsMicroseconds, counters);
            pending_routes.finish();
            pending_sibling_routes.finish();
            let pending_table_scratch_bytes = (pending_routes.capacity_bytes()
                + pending_sibling_routes.capacity_bytes()
                + pending_prefix_producers.capacity() * size_of::<PendingPrefixProducer>()
                + prefix_producer_seen.capacity() * size_of::<u32>())
                as u64;
            self.retained
                .memory
                .reserve_required(MemoryCategory::BatchScratch, pending_table_scratch_bytes);
            sequences.finish();
            let sequence_scratch_bytes = sequences.capacity_bytes();
            self.retained
                .memory
                .reserve_required(MemoryCategory::BatchScratch, sequence_scratch_bytes);
            let mut planning_workspace = ImpactPlanningWorkspace::default();
            let mut deferred_sequence_routes = DeferredSequenceRoutes::default();
            let sequence_routing_timer = PassTimer::start();
            if !regions.covers_document() {
                deferred_sequence_routes = self.route_sequence_changes(
                    &sequences,
                    &program_delta.sheets,
                    tree_routing,
                    &mut regions,
                    &mut planning_workspace,
                    counters,
                );
            }
            if !regions.covers_document() && !self.retained.routing.relational_routes().is_empty() {
                // The elements that left the document. Their subtrees are staged with them, so the
                // list names every element that left. An element that moved to another parent took
                // its subtree along without staging it, which no list of rows can speak for.
                let mut departed = Some(Vec::new());
                for (node, before, after) in self.host.tree_staging.rows() {
                    let Some(before) = before else {
                        continue;
                    };
                    match after {
                        None => {
                            if let Some(departed) = departed.as_mut() {
                                departed.push(node);
                            }
                        }
                        Some(after) if after.parent != before.parent => {
                            departed = None;
                            break;
                        }
                        Some(_) => {}
                    }
                }
                self.route_relational_sequence_changes(&sequences, departed.as_deref(), &mut regions, counters);
            }
            sequence_routing_timer.stop(Counter::SequenceRoutingMicroseconds, counters);
            let mut prefix_convergence = PrefixConvergenceOutcome::default();
            let pending_route_flush_timer = PassTimer::start();
            if !regions.covers_document() {
                prefix_convergence = self.flush_pending_routes(
                    &mut pending_routes,
                    &mut regions,
                    &mut planning_workspace,
                    retained_winners_are_current,
                    &transaction,
                    &sequences,
                    &pending_prefix_producers,
                    counters,
                );
            }
            self.flush_deferred_sequence_routes(
                deferred_sequence_routes,
                prefix_convergence.positional_routes_are_covered,
                &sequences,
                &mut regions,
                &mut planning_workspace,
                counters,
            );
            if !regions.covers_document() {
                self.flush_pending_sibling_routes(
                    &mut pending_sibling_routes,
                    sibling_entries,
                    &mut regions,
                    &mut planning_workspace,
                    prefix_convergence.sibling_routes_are_covered,
                    counters,
                );
            }
            drop(planning_workspace);
            pending_route_flush_timer.stop(Counter::PendingRouteFlushMicroseconds, counters);
            let pending_route_scratch_bytes = pending_routes
                .values()
                .map(|routes| (routes.capacity() * size_of::<ImpactRegion>()) as u64)
                .sum();
            self.retained
                .memory
                .release(MemoryCategory::BatchScratch, pending_route_scratch_bytes);
            let pending_sibling_scratch_bytes = pending_sibling_routes
                .values()
                .map(|routes| (routes.capacity() * size_of::<ImpactRegion>()) as u64)
                .sum();
            self.retained
                .memory
                .release(MemoryCategory::BatchScratch, pending_sibling_scratch_bytes);
            self.retained
                .memory
                .release(MemoryCategory::BatchScratch, pending_table_scratch_bytes);
            self.retained
                .memory
                .release(MemoryCategory::BatchScratch, sequence_scratch_bytes);
            self.retained
                .memory
                .release(MemoryCategory::BatchScratch, arrival_scratch_bytes);
            let mut match_workspace = std::mem::take(&mut self.retained.match_workspace);
            let before = match_workspace.capacity_bytes();
            match_workspace.retain_current_for_matching();
            let after = match_workspace.capacity_bytes();
            self.retained
                .memory
                .release(MemoryCategory::BatchScratch, before - after);
            let prepared_match_workspace = (after != 0).then_some(match_workspace);
            if let Some(match_workspace) = prepared_match_workspace {
                let mut prepared = PreparedBatchMatchingTraversal::new(root);
                prepared.reuse_retained_match_answers = reuse_retained_match_answers;
                prepared.match_workspace = match_workspace;
                self.retained.prepared_batch_matching_traversal = Some(prepared);
            }
        } else {
            routing_setup_timer.stop(Counter::RoutingSetupMicroseconds, counters);
        }
        let environment_changed = transaction
            .markers
            .iter()
            .any(|marker| marker.kind == transaction::InputKind::Environment);
        // A rule's declarations edited may be custom properties, which are no winners. The
        // descriptors of a named rule context, such as a counter style's, are no declarations a
        // winner holds: the context moves, as below.
        let rule_declarations_edited = transaction.inputs.iter().any(|input| {
            matches!(input.key, InputKey::RuleField(rule, RuleField::Declarations)
            if !matches!(
                self.retained.program.rule_version(rule).kind,
                RuleKind::CounterStyle | RuleKind::Function | RuleKind::FontFeatureValues
            ))
        });
        let mut named_rules_moved = NamedRuleContextsMoved::default();
        for delta in &transaction.program_joins {
            match self.retained.program.rule_version(delta.rule).kind {
                RuleKind::CounterStyle => named_rules_moved.counter_styles = true,
                RuleKind::Function => named_rules_moved.custom_functions = true,
                RuleKind::FontFeatureValues => named_rules_moved.font_feature_values = true,
                _ => {}
            }
        }
        let pseudo_inputs_may_have_changed = environment_changed
            || rule_declarations_edited
            || named_rules_moved.any()
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
            regions.widen_to_document(counters);
        }
        if !self.retained.prefix_caches.borrow().states.is_current() {
            self.discard_retained_prefix_caches();
        }
        let batch_compilation_timer = PassTimer::start();
        regions.normalize(&self.retained.tree);
        let impact_region_scratch_bytes = regions.region_index_capacity_bytes();
        let impact_region_scratch = self
            .memory
            .charge_scratch(MemoryCategory::BatchScratch, impact_region_scratch_bytes);
        let patch_cover = retained_answer_patch_selection
            .as_ref()
            .map(|_| regions.compile_patch_cover(&self.retained.tree, Some(root)));
        self.resolve_already_planned_selector_truth(&regions, patch_cover.as_ref().map(|cover| &cover.full), counters);
        batch_compilation_timer.stop(Counter::BatchCompilationMicroseconds, counters);
        let program_base_version = transaction.program_base_version;
        // A scoped plan consumes retained match answers or packs its missing rows adaptively. Do
        // not walk and snapshot the complete required fact store merely to prepare for that bounded
        // traversal. Broad plans will consume the complete batch, so their broad reaction pays the
        // preparation cost once here and reuses it during matching.
        let prepare_complete_matching_batch = regions.regions().iter().any(|region| {
            matches!(region, ImpactRegion::Document | ImpactRegion::TreeScope(_))
                || *region == ImpactRegion::Subtree(root)
        });
        let completeness_timer = PassTimer::start();
        let prepared_matching_batch_is_complete = if !prepare_complete_matching_batch {
            false
        } else {
            let facts = self.retained.facts.primary();
            let mut inspected = 0;
            let mut is_complete = true;
            regions.for_each(&self.retained.tree, Some(root), |node| {
                inspected += 1;
                is_complete &= facts.row_of(node).is_some();
            });
            counters.add(Counter::PreparedMatchingBatchCompletenessRowsInspected, inspected);
            is_complete
        };
        completeness_timer.stop(Counter::BatchCompilationMicroseconds, counters);
        let fact_view_bytes = self
            .transaction_fact_view
            .as_ref()
            .map_or(0, TransactionFactView::capacity_bytes);
        self.retained.transaction_fact_view = None;
        self.retained
            .memory
            .release(MemoryCategory::BatchScratch, fact_view_bytes);
        let plan_is_broad = regions.regions().contains(&ImpactRegion::Document)
            || regions
                .regions()
                .iter()
                .any(|region| matches!(region, ImpactRegion::TreeScope(_)) || *region == ImpactRegion::Subtree(root));
        let mut direct_action_nodes = DeltaBatch::default();
        for node in transaction.inputs.iter().filter_map(|input| input.key.style_node()) {
            direct_action_nodes.push(node);
        }
        direct_action_nodes.consolidate();
        let direct_action_node_bytes = direct_action_nodes.capacity_bytes();
        self.retained
            .memory
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
                ) => {
                    // A pseudo-only input publishes the originating element's record, which the
                    // engine settles from its retained winners; installing it installs the
                    // pseudo-elements beside it.
                    let reaction = if reaction == transaction::STYLE_REACTION_PSEUDO_INPUTS_MAY_HAVE_CHANGED {
                        reaction | transaction::STYLE_REACTION_PUBLISHED_STYLE
                    } else {
                        reaction
                    };
                    Some((node, reaction, inherited_style_groups))
                }
                _ => None,
            })
            .collect();
        style_input_reactions.sort_unstable_by_key(|&(node, _, _)| node);
        let style_input_reaction_bytes = (style_input_reactions.capacity() * size_of::<(StyleNodeID, u8, u8)>()) as u64;
        self.retained
            .memory
            .reserve_required(MemoryCategory::BatchScratch, style_input_reaction_bytes);
        self.release_transaction_and_sweep_atoms(transaction, counters);
        // Releasing staging can compact primary payloads. Take the shared view afterwards so that
        // compaction does not need to copy the complete primary arrangement away from its view.
        if prepared_matching_batch_is_complete {
            let batch = self.retained.facts.primary_view();
            // NB: This externally visible counter predates shared primary views. It now counts the
            //     rows made available to the prepared batch without implying a physical copy.
            counters.add(Counter::PreparedMatchingBatchRowsCloned, batch.live_row_count() as u64);
            match &mut self.retained.prepared_batch_matching_traversal {
                Some(prepared) => prepared.batch = Some(batch),
                None => {
                    let mut prepared = PreparedBatchMatchingTraversal::new(root);
                    prepared.batch = Some(batch);
                    prepared.reuse_retained_match_answers = reuse_retained_match_answers;
                    self.retained.prepared_batch_matching_traversal = Some(prepared);
                }
            }
        }
        #[cfg(test)]
        if plan_is_broad && let Some(capture) = &mut self.retained.diagnostic_plan_capture {
            capture.scoped = false;
        }
        let patch_preparation_timer = PassTimer::start();
        let mut retained_answer_patch =
            retained_answer_patch_selection.map(|selection| self.prepare_retained_answer_patch(selection));
        patch_preparation_timer.stop(Counter::RetainedAnswerPatchLoopMicroseconds, counters);
        let retained_answer_patch_scratch_bytes = retained_answer_patch
            .as_ref()
            .map_or(0, RetainedAnswerPatch::capacity_bytes);
        self.retained
            .memory
            .reserve_required(MemoryCategory::BatchScratch, retained_answer_patch_scratch_bytes);
        let published_retained_answer_dispatch = if retained_answer_patch.is_none() && reuse_retained_match_answers {
            self.retained_answer_dispatch_for_traversal(true)
        } else {
            None
        };
        let compile_union_timer = PassTimer::start();
        let compiled_regions = regions.compile_union(regions.regions(), &self.retained.tree, Some(root));
        compile_union_timer.stop(Counter::BatchCompilationMicroseconds, counters);
        let winner_version_timer = PassTimer::start();
        if let Some(base_version) = program_base_version {
            let current_version = self.retained.program.version();
            if regions.covers_document() {
                self.retained.winner_groups.begin_program_version(current_version);
            } else {
                self.retained
                    .winner_groups
                    .advance_program_version_where(base_version, current_version, |node| {
                        !regions.batch_contains_node(&compiled_regions, node)
                    });
            }
        }
        winner_version_timer.stop(Counter::WinnerVersionAdvanceMicroseconds, counters);

        clock.enter(Counter::PrepareMicroseconds, counters);
        if self.retained.tree.has_tree_scopes() || !self.retained.scope_roots.is_empty() {
            let mut nodes = Vec::new();
            regions.for_each_batch(&compiled_regions, |node| nodes.push(node));
            let bytes = (nodes.capacity() * size_of::<StyleNodeID>()) as u64;
            self.retained
                .memory
                .reserve_required(MemoryCategory::BatchScratch, bytes);
            self.prepare_scope_programs_for_nodes(nodes);
            self.retained.memory.release(MemoryCategory::BatchScratch, bytes);
        } else {
            self.prepare_scope_program(TreeScopeID::DOCUMENT);
        }
        clock.enter(Counter::MatchingCascadeMicroseconds, counters);
        let mut node_count = 0;
        let mut unattributed_node_count = 0;
        let mut published_match_answers = PublishedMatchAnswers::default();
        // A node inside any coarse region was planned without exact selector provenance. Exact
        // node routes consume their signed changes directly; only incomplete routes refresh.
        let mut selector_truth_changes = std::mem::take(&mut self.retained.selector_truth_changes);
        selector_truth_changes.consolidate(counters);
        let selector_truth_change_bytes = selector_truth_changes.capacity_bytes();
        self.retained
            .memory
            .reserve_required(MemoryCategory::BatchScratch, selector_truth_change_bytes);
        // A winner-pruned route preserves this flush's cascade result but makes its exact answer
        // stale. Retire it only after the current batch has been compiled so publication stays put.
        let mut stale_refresh_nodes = Vec::new();
        let mut patch_preserved_nodes: Vec<StyleNodeID> = Vec::new();
        let mut patch_processed_nodes: Vec<StyleNodeID> = Vec::new();
        for refresh in selector_truth_changes.refreshes.as_slice() {
            if !regions.batch_contains_node(&compiled_regions, refresh.node) {
                published_match_answers.answer_effects.forget_answer(
                    refresh.node,
                    &mut self.retained.match_answers,
                    &mut self.retained.memory,
                );
            } else {
                stale_refresh_nodes.push(refresh.node);
            }
        }
        stale_refresh_nodes.sort_unstable();
        stale_refresh_nodes.dedup();
        let stale_refresh_node_bytes = (stale_refresh_nodes.capacity() * size_of::<StyleNodeID>()) as u64;
        self.retained
            .memory
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
        let patch_loop_timer = PassTimer::start();
        regions.for_each_batch(&compiled_regions, |node| {
            counters.bump(Counter::ReachedStyleNodes);
            #[cfg(test)]
            if let Some(capture) = &mut self.retained.diagnostic_plan_capture {
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
                        regions.covering_attributions(
                            cover,
                            &self.retained.tree,
                            &mut attribution_sweep,
                            node,
                            &mut attribution_scratch,
                        )
                    });
                    let node_deltas = selector_truth_changes.deltas_for(node);
                    let refreshes = selector_truth_changes.refreshes_for(node);
                    let truth_patch = if node_is_coarse_covered {
                        counters.bump(Counter::RetainedPatchesCoarseCovered);
                        has_upquery = true;
                        SelectorTruthPatch::Full
                    } else if !attribution_known {
                        has_upquery = true;
                        SelectorTruthPatch::Full
                    } else if refreshes.iter().any(|refresh| refresh.rule.is_none()) {
                        counters.bump(Counter::RetainedPatchesPoisoned);
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
                        || (patch.always_emit_for(node) && patch.rule_keys.is_empty())
                        || !patch.cascade_update_properties.is_empty()
                    {
                        SelectorTruthPatch::Direct(node_deltas)
                    } else {
                        counters.bump(Counter::RetainedPatchesUnattributed);
                        has_upquery = true;
                        SelectorTruthPatch::Full
                    };
                    let rule_is_safe = |rule: RuleID, entry: EntryID| {
                        let (program, _) = self.retained.programs.entry_location(entry);
                        self.retained.program.declarations_are_complete_for(rule)
                            && self.retained.program.rule_version(rule).selector_program == Some(program)
                            && !self.retained.programs.get(program).contains_relational_selector()
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
                        && !has_direct_action
                        && node_has_safe_exact_cascade_provenance
                        && self.match_answer_is_comparable_across_elements(node);
                    can_stop_at_exact_cascade = transaction_supports_global_exact_cascade_stops;
                    match self.patch_retained_match_answer(
                        &mut published_match_answers.answer_effects,
                        node,
                        patch,
                        truth_patch,
                        counters,
                    ) {
                        Some(outcome) => {
                            if outcome.emit
                                && !has_direct_action
                                && !patch.has_non_selector_inputs
                                && patch.always_emit_nodes.binary_search(&node).is_err()
                                && let Some(deferred) = self.retained.deferred_pseudo_element
                                && let Lookup::Known(previous) = self.retained.retained_match_answers.lookup(node)
                                && let Some(previous) = self.retained.match_answers.answer(*previous)
                                && let Some(current) = published_match_answers
                                    .answer_effects
                                    .answer_identity(&self.retained.retained_match_answers, node)
                                && let Some(current) = self.retained.match_answers.answer(current)
                            {
                                let is_observable = |entry: &&RetainedRuleMatch| {
                                    self.retained.programs.get(entry.program).entries()[entry.entry as usize]
                                        .pseudo_element
                                        .is_none_or(|target| target.kind != deferred)
                                };
                                let previous = previous.iter().filter(is_observable);
                                let current = current.iter().filter(is_observable);
                                if previous.clone().eq(current.clone())
                                    && previous
                                        .chain(current)
                                        .all(|entry| patch.cascade_update_rules.binary_search(&entry.rule).is_err())
                                {
                                    counters.bump(Counter::RetainedMatchAnswerPatchStops);
                                    return;
                                }
                            }
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
                            counters.bump(Counter::RetainedMatchAnswerPatchMisses);
                            repair_match_identity = match_identity_is_complete_output
                                && !patch.always_emit_for(node)
                                && !patch.orders_shifted
                                && matches!(
                                    self.retained.retained_match_answers.cascade_input_lookup(node),
                                    Lookup::Known(_)
                                );
                            published_match_answers.answer_effects.forget_answer(
                                node,
                                &mut self.retained.match_answers,
                                &mut self.retained.memory,
                            );
                            has_upquery = true;
                            true
                        }
                    }
                }
                None => {
                    if !transaction_reaches_no_selector {
                        published_match_answers.answer_effects.forget_answer(
                            node,
                            &mut self.retained.match_answers,
                            &mut self.retained.memory,
                        );
                        has_upquery = true;
                    }
                    true
                }
            };
            if !emit_node {
                return;
            }
            // A departure can remain in a conservative region while its routing facts survive, and
            // a live shadow root or document is a synthetic relation node rather than a style
            // output. None has a C++ element to consume a record; every live element whose style
            // any of them can affect is another member of the region.
            let is_scope_root = self.retained.scope_by_root.get(node).is_some();
            if !self.retained.tree.is_live(node) || is_scope_root || self.retained.tree.is_relation_only(node) {
                return;
            }
            if repair_match_identity {
                identity_repair_nodes.push(node);
            }
            if has_direct_action {
                counters.bump(Counter::PlannedNodesWithDirectAction);
            }
            if has_signed_delta {
                counters.bump(Counter::PlannedNodesWithSignedDelta);
            }
            if has_output_change {
                counters.bump(Counter::PlannedNodesWithOutputChange);
            }
            if has_upquery {
                counters.bump(Counter::PlannedNodesWithUpquery);
            }
            if !has_direct_action && !has_signed_delta && !has_output_change && !has_upquery {
                counters.bump(Counter::PlannedNodesUnattributed);
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
        if let Some(patch) = retained_answer_patch.as_mut() {
            let mut caches = patch.prefix_caches.borrow_mut();
            if let Lookup::Known(states) = caches.states.lookup_mut(patch.scope_program) {
                states.install_prefix_effects(&mut patch.prefix_context.effects);
            }
            caches.states.settle_memory(&mut self.retained.memory);
        }
        patch_loop_timer.stop(Counter::RetainedAnswerPatchLoopMicroseconds, counters);
        exact_cascade_stop_nodes.consolidate();
        exact_cascade_confirmation_nodes.consolidate();
        let exact_cascade_stop_node_bytes = exact_cascade_stop_nodes.capacity_bytes();
        let exact_cascade_confirmation_node_bytes = exact_cascade_confirmation_nodes.capacity_bytes();
        self.retained.memory.reserve_required(
            MemoryCategory::BatchScratch,
            exact_cascade_stop_node_bytes + exact_cascade_confirmation_node_bytes,
        );
        #[cfg(test)]
        if self.retained.diagnostic_plan_capture.is_some() {
            // Plan-only fixtures deliberately omit unrelated fact rows. Complete those fixtures
            // after routing, as the browser has before it consumes the already-computed plan.
            let saved_counters = counters.clone();
            for node in self.elements_under(root) {
                self.retained.facts.ensure_row(node);
            }
            self.retained.facts.apply_staged(&mut self.retained.memory);
            *counters = saved_counters;
        }
        let publish_style_answers = true;
        {
            let published_node_bytes = (published_nodes.capacity() * size_of::<StyleNodeID>()) as u64;
            let previous_cascade_input_bytes =
                (previous_cascade_inputs.capacity() * size_of::<Option<MatchAnswerID>>()) as u64;
            let identity_repair_node_bytes = (identity_repair_nodes.capacity() * size_of::<StyleNodeID>()) as u64;
            self.retained
                .memory
                .reserve_required(MemoryCategory::BatchScratch, published_node_bytes);
            self.retained
                .memory
                .reserve_required(MemoryCategory::BatchScratch, previous_cascade_input_bytes);
            self.retained
                .memory
                .reserve_required(MemoryCategory::BatchScratch, identity_repair_node_bytes);
            if publish_style_answers {
                identity_repair_nodes.sort_unstable();
                identity_repair_nodes.dedup();
                incremental_cascade_answers.sort_unstable_by_key(|answer| answer.node);
                let prefer_complete_batch =
                    published_nodes.len().saturating_mul(16) > self.retained.tree.connected_element_count() as usize;
                let reuse_active_batch_matching_traversal =
                    transaction_reaches_no_selector && self.retained.batch_matching_traversal.is_some();
                clock.enter(Counter::PrepareMicroseconds, counters);
                if !reuse_active_batch_matching_traversal {
                    let completion_begin_timer = PassTimer::start();
                    self.begin_published_match_answer_completion_batch(root, prefer_complete_batch, counters);
                    completion_begin_timer.stop(Counter::CompletionBatchBeginMicroseconds, counters);
                } else if let Some(mut traversal) = self.retained.batch_matching_traversal.take() {
                    if let Some(batch) = traversal.batch.as_ref() {
                        self.prepare_prefix_rows_for_batch(batch, &mut traversal.prefix_contexts);
                    }
                    self.retained.batch_matching_traversal = Some(traversal);
                }
                clock.enter(Counter::MatchingCascadeMicroseconds, counters);
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
                self.retained
                    .memory
                    .reserve_required(MemoryCategory::BatchScratch, patch_node_scratch_bytes);
                let mut accepted_node_count = 0;
                // Exact retained answers are interned across elements. Once one sufficiently
                // expensive answer has been compacted in this completion batch, other elements
                // without element declarations can share its cascade input and winner identities.
                let mut completed_retained_answers: HashMap<MatchAnswerID, (StyleNodeID, MatchAnswerID, bool)> =
                    HashMap::default();
                let mut completed_retained_answer_bytes = 0_u64;
                let share_cascade_completions = published_nodes.len() >= MIN_SHARED_CASCADE_COMPLETION_BATCH;
                let completion_pass_timer = PassTimer::start();
                let mut traversal = self.retained.batch_matching_traversal.take();
                for index in 0..published_nodes.len() {
                    let node = published_nodes[index];
                    let previous_exact_cascade_input = previous_cascade_inputs[index];
                    let retained_cascade_input = published_match_answers
                        .answer_effects
                        .cascade_input(&self.retained.retained_match_answers, node);
                    let previous_cascade_input = identity_repair_nodes
                        .binary_search(&node)
                        .ok()
                        .and(retained_cascade_input);
                    let retained_answer_identity = (share_cascade_completions
                        && retained_answer_dispatch.is_some()
                        && self.match_answer_is_comparable_across_elements(node)
                        && self.has_no_element_declarations(node))
                    .then(|| {
                        published_match_answers
                            .answer_effects
                            .answer_identity(&self.retained.retained_match_answers, node)
                    })
                    .flatten();
                    let published_answer = incremental_cascade_answers
                        .binary_search_by_key(&node, |answer| answer.node)
                        .ok()
                        .map(|answer_index| {
                            let answer = &mut incremental_cascade_answers[answer_index];
                            counters.bump(Counter::RetainedMatchAnswerReuses);
                            PublishedMatchAnswer {
                                node,
                                cascade_input: Some(answer.cascade_input),
                                matches: answer.matches.take(),
                                cascade_winners_are_complete: answer.cascade_winners_are_complete,
                                observed: false,
                            }
                        })
                        .or_else(|| {
                            if self.retained.last_transaction_only_derived_child_reactions {
                                self.reuse_published_match_answer(
                                    &mut published_match_answers.answer_effects,
                                    node,
                                    counters,
                                )
                            } else {
                                None
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
                                &mut published_match_answers.answer_effects,
                                node,
                                source,
                                cascade_input,
                                cascade_winners_are_complete,
                                counters,
                            )
                        })
                        .unwrap_or_else(|| {
                            self.complete_published_match_answer_in_traversal(
                                &mut published_match_answers.answer_effects,
                                node,
                                traversal.as_deref_mut(),
                                retained_answer_dispatch,
                                counters,
                            )
                            .expect("a connected style reaction must have complete selector facts")
                        });
                    if let Some(identity) = retained_answer_identity
                        && let Some(cascade_input) = published_answer.cascade_input
                        && !completed_retained_answers.contains_key(&identity)
                        && self.shared_cascade_completion_is_profitable(identity)
                        && self.shared_cascade_completion_is_node_independent(identity)
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
                        self.retained
                            .memory
                            .reserve_required(MemoryCategory::BatchScratch, added_bytes);
                        completed_retained_answer_bytes += added_bytes;
                    }
                    if let Some(retained_cascade_input) = retained_cascade_input
                        && let Some(published_cascade_input) = published_answer.cascade_input
                    {
                        counters.bump(Counter::PublishedMatchAnswerRetainedIdentityComparisons);
                        if retained_cascade_input == published_cascade_input {
                            counters.bump(Counter::PublishedMatchAnswerRetainedIdentityMatches);
                        }
                    }
                    // A confirmation may stop a reaction only when its proof is strictly cheaper
                    // than the materialization it avoids. The answer above is current either way:
                    // a fresh completion just derived it exactly, and a reused answer is the
                    // output of this flush's retained-answer patch. A complete winner inventory
                    // plus the same live output-identity predicates the exact-cascade stop trusts
                    // therefore prove the stop without re-matching or cloning anything; verify
                    // mode still cold-matches every stopped node.
                    let confirmed_exact_cascade = exact_cascade_confirmation_nodes
                        .as_slice()
                        .binary_search(&node)
                        .is_ok()
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
                                && self
                                    .exact_cascade_output_is_unchanged(&published_match_answers.answer_effects, node)
                                && self.pseudo_cascade_states_are_unchanged_with_effects(
                                    &published_match_answers.answer_effects,
                                    node,
                                );
                            whole_inventory_proof
                                || previous_exact_cascade_input.is_some_and(|previous_cascade_input| {
                                    // Equality proves nothing by itself: a stale answer equals
                                    // itself, and the patch-preserved case confirmed above.
                                    previous_cascade_input != current_cascade_input
                                        && self.answer_transition_cannot_change_cascade(
                                            node,
                                            previous_cascade_input,
                                            current_cascade_input,
                                            counters,
                                        )
                                })
                        });
                    if confirmed_exact_cascade && let Some(current_cascade_input) = published_answer.cascade_input {
                        verify_style_answer_patch(self, counters, |verifier| {
                            verifier.verify_retained_cascade_input(
                                &published_match_answers.answer_effects,
                                node,
                                current_cascade_input,
                            );
                        });
                    }
                    match published_answer {
                        published_answer
                            if previous_cascade_input.is_some()
                                && published_answer.cascade_input == previous_cascade_input =>
                        {
                            counters.bump(Counter::PublishedMatchAnswerIdentityRepairs);
                            counters.bump(Counter::PublishedMatchAnswerIdentityRepairStops);
                            node_count -= 1;
                        }
                        _ if confirmed_exact_cascade => {
                            counters.bump(Counter::PublishedExactCascadeStops);
                            node_count -= 1;
                        }
                        _ if exact_cascade_stop_nodes.as_slice().binary_search(&node).is_ok()
                            && published_answer.cascade_winners_are_complete
                            && self
                                .exact_cascade_output_is_unchanged(&published_match_answers.answer_effects, node)
                            && self.pseudo_cascade_states_are_unchanged_with_effects(
                                &published_match_answers.answer_effects,
                                node,
                            ) =>
                        {
                            counters.bump(Counter::PublishedExactCascadeStops);
                            node_count -= 1;
                        }
                        published_answer => {
                            if previous_cascade_input.is_some() {
                                counters.bump(Counter::PublishedMatchAnswerIdentityRepairs);
                                counters.bump(Counter::MatchAnswerChanges);
                                counters.bump(Counter::PlannedNodesWithOutputChange);
                            }
                            published_nodes[accepted_node_count] = node;
                            previous_cascade_inputs[accepted_node_count] = previous_exact_cascade_input;
                            accepted_node_count += 1;
                            published_match_answers.push(published_answer, &mut self.retained.memory, counters);
                        }
                    }
                }
                self.install_answer_effects(std::mem::take(&mut published_match_answers.answer_effects));
                self.retained.batch_matching_traversal = traversal;
                completion_pass_timer.stop(Counter::CompletionPassMicroseconds, counters);
                self.retained
                    .memory
                    .release(MemoryCategory::BatchScratch, completed_retained_answer_bytes);
                published_nodes.truncate(accepted_node_count);
                previous_cascade_inputs.truncate(accepted_node_count);
                if !reuse_active_batch_matching_traversal {
                    self.end_published_match_answer_completion_batch();
                }
            } else {
                published_nodes.clear();
                previous_cascade_inputs.clear();
            }
            self.retained
                .memory
                .release(MemoryCategory::BatchScratch, identity_repair_node_bytes);
            self.retained
                .memory
                .release(MemoryCategory::BatchScratch, previous_cascade_input_bytes);
        }
        self.retained.memory.release(
            MemoryCategory::BatchScratch,
            exact_cascade_stop_node_bytes + exact_cascade_confirmation_node_bytes,
        );
        self.retained.memory.release(
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
            self.retained.memory.reserve_required(
                MemoryCategory::BatchScratch,
                final_retained_answer_patch_scratch_bytes - retained_answer_patch_scratch_bytes,
            );
        } else {
            self.retained.memory.release(
                MemoryCategory::BatchScratch,
                retained_answer_patch_scratch_bytes - final_retained_answer_patch_scratch_bytes,
            );
        }
        if retained_answer_patch.take().is_some() {
            self.retain_prefix_states();
        }
        self.retained
            .memory
            .release(MemoryCategory::BatchScratch, final_retained_answer_patch_scratch_bytes);
        self.retained
            .memory
            .release(MemoryCategory::BatchScratch, selector_truth_change_bytes);
        self.retained
            .memory
            .release(MemoryCategory::BatchScratch, direct_action_node_bytes);
        published_match_answers.sort();
        // Record computation interns and installs immediately in the current evaluator.
        clock.enter(Counter::ComputationPublicationMicroseconds, counters);
        let mut suspended_pass = None;
        {
            let mut style_delta_memory = MemoryLease::new(MemoryCategory::BridgeBuffer);
            let mut computation_scratch_memory = MemoryLease::new(MemoryCategory::BatchScratch);
            let mut style_deltas = Vec::with_capacity(published_nodes.len());
            let style_delta_bytes = (style_deltas.capacity() * size_of::<PublishedStyleDeltaRecord>()) as u64;
            style_delta_memory.resize_required_to(&mut self.retained.memory, style_delta_bytes);
            let mut engine_computed_record_scratch = publication::EngineComputedRecordScratch::default();
            engine_computed_record_scratch.document_environment_moved = environment_changed;
            engine_computed_record_scratch.host_applies_animation_plans = true;
            // The viewport the records were driven against last, against the one they are driven
            // against now.
            let viewport = (
                self.retained.document_style_computation_inputs.viewport_width,
                self.retained.document_style_computation_inputs.viewport_height,
            );
            engine_computed_record_scratch.viewport_moved =
                std::mem::replace(&mut self.retained.driven_viewport, viewport) != viewport;
            let computation_loop_timer = PassTimer::start();
            computation_scratch_memory.resize_required_to(
                &mut self.retained.memory,
                engine_computed_record_scratch.capacity_bytes(),
            );
            let font_environment_generation = self
                .retained
                .document_style_computation_inputs
                .font_environment_generation;
            if let Some(resolver) = &mut self.retained.font_resolution {
                resolver.prepare(font_environment_generation);
            }
            // NB: This input crosses fixed-font intermediaries independently of inheritance.
            //     Incremental roots use the current retained document inputs already submitted.
            if let Some((root_index, root)) = published_nodes.iter().copied().enumerate().find(|(_, node)| {
                self.retained.computed_group_sets.adjustment_facts(*node)
                    & bridge::element_adjustment_fact::IS_DOCUMENT_ELEMENT
                    != 0
            }) {
                let answer = published_match_answers.lookup(root).unwrap();
                let reaction = style_input_reactions
                    .binary_search_by_key(&root, |&(node, _, _)| node)
                    .map_or(transaction::STYLE_REACTION_PUBLISHED_STYLE, |index| {
                        style_input_reactions[index].1
                    });
                let parent_inputs = publication::ParentInputsMoved {
                    inherited_style: reaction & transaction::STYLE_REACTION_INHERITED_STYLE != 0,
                    display: inputs_moved.parent_display_moved(root),
                };
                // The probe settles what the root's own row settles, under the same unseen inputs.
                let can_prepare = !self.retained.computed_group_sets.node_answer_is_incomplete(root);
                if can_prepare {
                    let flipped_rules = selector_truth_changes.deltas_for(root);
                    let answer_is_unchanged =
                        answer.cascade_input.is_some() && answer.cascade_input == previous_cascade_inputs[root_index];
                    let flipped: publication::FlippedRules = flipped_rules
                        .iter()
                        .map(|delta| {
                            self.retained
                                .programs
                                .entry(delta.entry)
                                .1
                                .pseudo_element
                                .map(|pseudo| pseudo.kind.0)
                        })
                        .collect();
                    let answer_refreshed =
                        !selector_truth_changes.refreshes_for(root).is_empty() || inputs_moved.given_up(root);
                    let winners_are_exact = !rule_declarations_edited
                        && !answer_refreshed
                        && (answer_is_unchanged
                            || (!flipped_rules.is_empty()
                                && flipped_rules
                                    .iter()
                                    .all(|delta| self.retained.program.declarations_are_complete_for(delta.rule))));
                    engine_computed_record_scratch.answer_or_declarations_moved =
                        rule_declarations_edited || !flipped_rules.is_empty() || answer_refreshed;
                    engine_computed_record_scratch.pseudo_inputs_alone = reaction
                        == transaction::STYLE_REACTION_PUBLISHED_STYLE
                            | transaction::STYLE_REACTION_PSEUDO_INPUTS_MAY_HAVE_CHANGED;
                    let full_drive_reason = self.full_drive_reason(root, reaction, named_rules_moved, &inputs_moved);
                    self.prepare_root_font_inputs(
                        root,
                        answer.cascade_winners_are_complete,
                        winners_are_exact.then_some(flipped),
                        parent_inputs,
                        full_drive_reason,
                        &mut engine_computed_record_scratch,
                        counters,
                    );
                } else {
                    counters.bump(Counter::RootFontInputsUnprovenFallbacks);
                }
                computation_scratch_memory.resize_required_to(
                    &mut self.retained.memory,
                    engine_computed_record_scratch.capacity_bytes(),
                );
            }
            self.host.batch_moves_for_retries = engine_computed_record_scratch.batch_moves();
            self.host.retry_full_drive_reasons.clear();
            // Each published node's pseudo-element inventory, read from the answer this transaction
            // publishes for it before that answer is installed and kept with the node's record
            // columns for as long as that answer stands, and beside it, for the record loop, the
            // answer's completeness and the matches its custom-property cascade runs over.
            // The host took the container effects of the rows it installed from the last
            // transaction.
            self.retained.container_effects_for_host.clear();
            self.retained.clear_batch_answer_facts();
            for &node in &published_nodes {
                self.retained.note_batch_answer_facts(node, &published_match_answers);
            }
            // The rows that read style while hidden, an SVG element (which an SVG resource reference
            // paints) and an element with animations, and every element they inherit from: none of
            // them is answered as hidden.
            let mut hidden_style_readers = HashSet::<StyleNodeID>::default();
            for &node in &published_nodes {
                use bridge::element_adjustment_fact as fact;
                if self.computed_group_sets.adjustment_facts(node) & (fact::IS_SVG_ELEMENT | fact::HAS_ANIMATIONS) == 0
                {
                    continue;
                }
                let mut current = Some(node);
                while let Some(element) = current
                    && hidden_style_readers.insert(element)
                {
                    current = self.tree.inheritance_parent(element);
                }
            }
            // The rows settle in the order the host applies them, each after the rows of the batch
            // it inherits from, which it then finds settled rather than still to come.
            if published_nodes.len() > 1 {
                let ranks = self.tree.style_reaction_order_ranks(published_nodes.iter().copied());
                let mut rows: Vec<_> = published_nodes
                    .iter()
                    .copied()
                    .zip(previous_cascade_inputs.iter().copied())
                    .collect();
                rows.sort_unstable_by_key(|(node, _)| ranks[node]);
                for (index, (node, previous_cascade_input)) in rows.into_iter().enumerate() {
                    published_nodes[index] = node;
                    previous_cascade_inputs[index] = previous_cascade_input;
                }
            }
            let mut pass = StylePass {
                transaction_version,
                program_version,
                scoped: !publish_document_root_arrival && !plan_is_broad,
                published_nodes,
                previous_cascade_inputs,
                style_input_reactions,
                selector_truth_changes,
                published_match_answers,
                rule_declarations_edited,
                pseudo_inputs_may_have_changed,
                named_rules_moved,
                inputs_moved,
                hidden_style_readers,
                derived_child_reactions: HashMap::default(),
                scratch: engine_computed_record_scratch,
                next_index: 0,
            };
            self.run_style_pass(
                &mut pass,
                &mut style_deltas,
                &mut style_delta_memory,
                &mut computation_scratch_memory,
                counters,
            );
            computation_loop_timer.stop(Counter::ComputationLoopMicroseconds, counters);
            computation_scratch_memory.resize_required_to(&mut self.retained.memory, pass.scratch.capacity_bytes());
            computation_scratch_memory.release();
            if !style_deltas.is_empty() {
                self.settle_computed_memory();
                counters.add(Counter::PublishedMatchAnswerRecords, style_deltas.len() as u64);
                clock.enter(Counter::EmitMicroseconds, counters);
                emit(transaction_version, program_version, &style_deltas);
            }
            clock.enter(Counter::TransactionRemainderMicroseconds, counters);
            self.retained.memory.release(
                MemoryCategory::BatchScratch,
                (pass.published_nodes.capacity() * size_of::<StyleNodeID>()) as u64,
            );
            published_match_answers = std::mem::take(&mut pass.published_match_answers);
            // NB: A pass the host installs in waves keeps its rows until its last wave; their batch
            //     scratch accounting is released with the first one.
            if pass.next_index < pass.published_nodes.len() {
                suspended_pass = Some(pass);
            }
        }
        self.retained
            .memory
            .release(MemoryCategory::BatchScratch, style_input_reaction_bytes);
        drop(impact_region_scratch);
        published_match_answers.match_element_calls_at_publication =
            counters.get(Counter::MatchElementCallsDuringPublishedStyleTransaction);
        published_match_answers.discard_unobserved_retained_answers = publish_document_root_arrival || plan_is_broad;
        // The rows of the waves still to come read the batch's answers as the first one did.
        if suspended_pass.is_none() {
            self.retained.clear_batch_answer_facts();
        }
        self.retained.published_match_answers = published_match_answers;
        self.host.suspended_style_pass = suspended_pass;
        if initial_tree_was_bulk_loaded {
            counters.bump(Counter::InitialBulkMatchLoads);
            counters.add(Counter::InitialBulkMatchRows, node_count);
        }
        counters.add(Counter::InvalidatedStyleNodes, node_count);
        if node_count == 0 {
            self.discard_prepared_batch_matching_traversal();
            self.retained
                .memory
                .release(MemoryCategory::BatchScratch, topology_bytes);
        } else {
            // C++ may propagate a changed inherited output below the nodes named by the plan. Those
            // descendants did not receive a selector delta, so their retained answers are still
            // current and must be consumed instead of matching them again.
            match &mut self.retained.prepared_batch_matching_traversal {
                Some(prepared) => prepared.reuse_retained_match_answers = true,
                None => {
                    let mut prepared = PreparedBatchMatchingTraversal::new(root);
                    prepared.reuse_retained_match_answers = true;
                    self.retained.prepared_batch_matching_traversal = Some(prepared);
                }
            }
            if !self.prepare_topology_for_matching(root, &mut regions) {
                self.retained
                    .memory
                    .release(MemoryCategory::BatchScratch, topology_bytes);
            }
        }
        !publish_document_root_arrival && !plan_is_broad
    }

    /// Settle the pass's rows from the first one the host has not been given, in the order the host
    /// applies them, up to the first row that waits for a row of this wave the host has to install
    /// before it. That row, and every row after it, wait for the next wave. The first row of a wave
    /// waits for nothing: the host installed every row before it.
    #[allow(clippy::too_many_lines)]
    fn run_style_pass(
        &mut self,
        pass: &mut StylePass,
        style_deltas: &mut Vec<PublishedStyleDeltaRecord>,
        style_delta_memory: &mut MemoryLease,
        computation_scratch_memory: &mut MemoryLease,
        counters: &mut Counters,
    ) {
        // What the chain above a node proves, read by its children in the same pass. A published
        // ancestor's change is exact for a descendant only when none of the ancestors can move
        // anything the descendant inherits, so the fold below is the upward walk the gate used to
        // run for every node: it stops at the first row a settled ancestor left behind and fills in
        // the rows of the nodes it crossed. The rows settle after the rows they inherit from, so
        // every fact it folds is final. A row an earlier wave installed holds no row here: what it
        // moved is in place.
        type DerivedChildInputRows = column::PagedColumn<column::PagedValuePage<publication::DerivedChildInputs>>;
        let row_of = |rows: &DerivedChildInputRows, node: StyleNodeID| {
            node.element_index().and_then(|index| rows.get(index as usize))
        };
        let mut crossed_ancestors = Vec::new();
        let ancestor_chain = |engine: &Self,
                              answers: &PublishedMatchAnswers,
                              reactions: &[(StyleNodeID, u8, u8)],
                              rows: &mut DerivedChildInputRows,
                              crossed: &mut Vec<StyleNodeID>,
                              node: StyleNodeID|
         -> publication::AncestorChain {
            crossed.clear();
            let mut current = Some(node);
            let mut chain = loop {
                let Some(ancestor) = current else {
                    break publication::AncestorChain::ROOT;
                };
                if let Some(chain) = row_of(rows, ancestor).and_then(|row| row.chain) {
                    break chain;
                }
                crossed.push(ancestor);
                current = engine.tree.inheritance_parent(ancestor);
            };
            for &ancestor in crossed.iter().rev() {
                let row = row_of(rows, ancestor);
                // A node of the batch holds a row from where it was processed, before the rows
                // inheriting from it; the walk stops at any other row.
                let published = row.is_some();
                // An ancestor whose answer the winners do not hold whole is C++'s to compute, and
                // what its winner delta says it moved is not all it moves.
                let answer_is_incomplete = answers.lookup(ancestor).is_some_and(|answer| {
                    !answer.cascade_winners_are_complete
                        && !engine.cascade_winners_are_complete_but_for_custom_properties(ancestor)
                });
                let unconfined = published
                    && (answer_is_incomplete
                        || !(reactions
                            .binary_search_by_key(&ancestor, |&(style_node, _, _)| style_node)
                            .is_err()
                            && engine.winner_delta_is_engine_confined(ancestor)
                            && !engine.node_environment_may_move(ancestor)));
                let settled = row.is_some_and(|row| row.settled);
                chain = publication::AncestorChain::fold(chain, published, unconfined, settled);
                if let Some(index) = ancestor.element_index() {
                    let mut row = row.unwrap_or_default();
                    row.chain = Some(chain);
                    rows.insert(index as usize, row);
                }
            }
            chain
        };
        let wave_start = pass.next_index;
        let mut next_published_index = pass.next_index;
        let mut pending_parent_inputs = None;
        let mut wave_stops = false;
        while !wave_stops && next_published_index < pass.published_nodes.len() {
            let mut suspended_on_font = false;
            for (published_index, node) in pass
                .published_nodes
                .iter()
                .copied()
                .enumerate()
                .skip(next_published_index)
            {
                // A refreshed answer cannot say which entries moved, and neither can a row a pass
                // before this one gave up, whose answer and winners that pass installed.
                let answer_refreshed =
                    !pass.selector_truth_changes.refreshes_for(node).is_empty() || pass.inputs_moved.given_up(node);
                // The kinds of element-backed pseudo-elements whose rules may have moved for the
                // node: those of the pseudo-element rules that flipped, or every kind when its
                // answer or the rules' declarations moved in another way.
                let element_reference_kinds_moved = if pass.pseudo_inputs_may_have_changed || answer_refreshed {
                    publication::pseudo_kind::ELEMENT_REFERENCE_KINDS
                } else {
                    pass.selector_truth_changes
                        .deltas_for(node)
                        .iter()
                        .filter_map(|delta| self.retained.programs.entry(delta.entry).1.pseudo_element)
                        .fold(0, |kinds, pseudo| {
                            kinds | 1_u64.checked_shl(u32::from(pseudo.kind.0)).unwrap_or(0)
                        })
                        & publication::pseudo_kind::ELEMENT_REFERENCE_KINDS
                };
                let pseudo_inputs_may_have_changed = pass.pseudo_inputs_may_have_changed
                    || answer_refreshed
                    || pass
                        .selector_truth_changes
                        .deltas_for(node)
                        .iter()
                        .any(|delta| self.retained.programs.entry(delta.entry).1.pseudo_element.is_some());
                // A demand the host made as it installed an earlier wave matched the node again and
                // took the answer this pass published for it: the host answers the row from its
                // demand.
                let answer = pass.published_match_answers.lookup(node);
                let answer_was_taken = answer.is_none();
                debug_assert!(
                    !answer_was_taken || wave_start > 0,
                    "each accepted style reaction has a published match answer"
                );
                let (answer_cascade_input, answer_winners_are_complete) = answer.map_or((None, false), |answer| {
                    (answer.cascade_input, answer.cascade_winners_are_complete)
                });
                let style_input_reaction_index = pass
                    .style_input_reactions
                    .binary_search_by_key(&node, |&(style_node, _, _)| style_node)
                    .ok();
                let (reaction, inherited_style_groups) = style_input_reaction_index
                    .map_or((transaction::STYLE_REACTION_PUBLISHED_STYLE, 0), |index| {
                        (pass.style_input_reactions[index].1, pass.style_input_reactions[index].2)
                    });
                let (reaction, inherited_style_groups) = match pass.derived_child_reactions.get(&node) {
                    Some(&(derived_reaction, derived_groups)) => {
                        (reaction | derived_reaction, inherited_style_groups | derived_groups)
                    }
                    None => (reaction, inherited_style_groups),
                };
                let pseudo_inputs_may_have_changed =
                    pseudo_inputs_may_have_changed || style_input_reaction_index.is_some();
                let old_style_record = self
                    .computed_group_sets
                    .assigned_style_record(node)
                    .map_or(0, |style_record| style_record.raw());
                // Custom properties alone leave an answer complete enough: the engine computes
                // the environment they decide.
                let answer_is_incomplete =
                    !answer_winners_are_complete && !self.cascade_winners_are_complete_but_for_custom_properties(node);
                self.retained
                    .computed_group_sets
                    .set_node_answer_incomplete(node, answer_is_incomplete);
                let prepared_parent_inputs = pass
                    .scratch
                    .prepared_root_font
                    .take_if(|(root, _, _)| *root == node)
                    .map(|(_, parent_inputs, font_drive)| {
                        pass.scratch.font_drive = font_drive;
                        parent_inputs
                    });
                let resuming_font = pass.scratch.font_drive.is_pending_for(node);
                let hidden = !resuming_font
                    && !pass.hidden_style_readers.contains(&node)
                    && self
                        .retained
                        .row_is_hidden(node, |ancestor| row_of(&pass.scratch.derived_child_inputs, ancestor));
                // The immediate parent's own unresolved fact, which the direct inherited-group
                // path reads without asking about the chain above it.
                // A node whose winners hold gated rules is derived where their conditions are
                // decided again and what they read of the containers is handed to the host.
                let direct_inherited_delta = (reaction == transaction::STYLE_REACTION_INHERITED_STYLE
                    && !resuming_font
                    && !hidden
                    && !answer_was_taken
                    && !self.retained.published_container_verdicts.contains_key(&node))
                .then(|| self.retained.tree.inheritance_parent(node))
                .flatten()
                .filter(|&parent| {
                    !row_of(&pass.scratch.derived_child_inputs, parent).is_some_and(|row| row.inheritance_unresolved)
                })
                .and_then(|parent| {
                    self.computed_group_sets.replace_engine_resolvable_inherited_groups(
                        node,
                        parent,
                        inherited_style_groups,
                    )
                });
                // A regular style reaction whose moved winners the engine can compute itself
                // publishes its new record here, so C++ applies a record instead of running a
                // style computation. An ancestor this flush already settled holds the record the
                // node inherits from.
                // The answer says whether the node relied on a settled ancestor, whose record it
                // then inherits from in full. C++ closes its batch over the nodes between a
                // published descendant and its published ancestor, and folds what the ancestor's
                // application derives into each of them before the descendant, so the node's
                // parent holds what it inherits from only when every node up to the settled
                // ancestor is settled as well.
                // A refreshed answer cannot say which entries moved, so the flag stays conservative
                // for C++; the pseudo winner states themselves are current here and settle it.
                let mut parent_inputs_moved =
                    pending_parent_inputs
                        .take()
                        .or(prepared_parent_inputs)
                        .unwrap_or(publication::ParentInputsMoved {
                            inherited_style: reaction & transaction::STYLE_REACTION_INHERITED_STYLE != 0,
                            // A slotted element's box-type parent moves with its slot's place in
                            // the shadow tree, which no input of its own says.
                            display: pass.inputs_moved.parent_display_moved(node)
                                || self.retained.tree.assigned_slot_of(node).is_some(),
                        });
                let full_drive_reason =
                    self.full_drive_reason(node, reaction, pass.named_rules_moved, &pass.inputs_moved);
                // Whether the row reads a row of this wave that is only in place once the host
                // installed it.
                let mut waits_for_installation = false;
                // NB: Entry gates were already established for a suspended computation.
                //     Its completed originating record must not change that decision.
                let engine_computed_gate_passes = if resuming_font {
                    true
                } else if hidden || answer_was_taken || direct_inherited_delta.is_some() {
                    false
                } else if reaction == transaction::STYLE_REACTION_INHERITED_CUSTOM_PROPERTIES
                    && !parent_inputs_moved.display
                    && !self.node_style_reads_custom_properties(node)
                {
                    // A non-consumer's record and environment do not move with what it
                    // inherits, beyond the environment move that already reached it. The row
                    // still goes to the host so that what it derives for the children carries
                    // the reaction on to the descendants whose style reads the environment.
                    false
                } else {
                    match self
                        .tree
                        .inheritance_parent(node)
                        .map_or(publication::AncestorChain::ROOT, |parent| {
                            ancestor_chain(
                                self,
                                &pass.published_match_answers,
                                &pass.style_input_reactions,
                                &mut pass.scratch.derived_child_inputs,
                                &mut crossed_ancestors,
                                parent,
                            )
                        })
                        .ancestors_are_confined()
                    {
                        None => {
                            counters.bump(Counter::EngineComputedRecordGateAncestors);
                            waits_for_installation = true;
                            false
                        }
                        Some(relied_on_settled_ancestor) => {
                            parent_inputs_moved.inherited_style |= relied_on_settled_ancestor;
                            true
                        }
                    }
                };
                let engine_record_answer = engine_computed_gate_passes.then(|| {
                    // Unchanged winners stand for an unchanged record only when the reaction
                    // is rules flipping for the node, every one of them known and declaring
                    // nothing past its winners, or the node's answer is the one it had.
                    let flipped_rules = pass.selector_truth_changes.deltas_for(node);
                    let answer_is_unchanged = answer_cascade_input.is_some()
                        && answer_cascade_input == pass.previous_cascade_inputs[published_index];
                    let flipped: publication::FlippedRules = flipped_rules
                        .iter()
                        .map(|delta| {
                            self.retained
                                .programs
                                .entry(delta.entry)
                                .1
                                .pseudo_element
                                .map(|pseudo| pseudo.kind.0)
                        })
                        .collect();
                    let winners_are_exact = !pass.rule_declarations_edited
                        && !answer_refreshed
                        && (answer_is_unchanged
                            || (!flipped_rules.is_empty()
                                && flipped_rules
                                    .iter()
                                    .all(|delta| self.retained.program.declarations_are_complete_for(delta.rule))));
                    pass.scratch.answer_or_declarations_moved =
                        pass.rule_declarations_edited || !flipped_rules.is_empty() || answer_refreshed;
                    pass.scratch.pseudo_inputs_alone = reaction
                        == transaction::STYLE_REACTION_PUBLISHED_STYLE
                            | transaction::STYLE_REACTION_PSEUDO_INPUTS_MAY_HAVE_CHANGED;
                    self.engine_computed_record_delta(
                        node,
                        answer_winners_are_complete,
                        winners_are_exact.then_some(flipped),
                        parent_inputs_moved,
                        full_drive_reason,
                        &mut pass.scratch,
                        counters,
                    )
                });
                // A row whose inheritance parent the host styles in this update waits for it as a
                // row behind an unsettled ancestor does.
                if let Some(Err(publication::Unanswered::AwaitsParent)) = engine_record_answer {
                    waits_for_installation = true;
                }
                // A row of this wave before this one is what it waits for: the wave stops here, and
                // the next one settles the row once the host installed that row. The host installed
                // every row before the first one of a wave, so one that still waits there waits for
                // what C++ settles as it applies the row, such as a parent without a style it closes
                // the batch over: the host answers the row from its demand.
                if waits_for_installation && published_index > wave_start {
                    next_published_index = published_index;
                    wave_stops = true;
                    break;
                }
                if let Some(Err(publication::Unanswered::Suspended(publication::Suspension::Font))) =
                    engine_record_answer
                {
                    // NB: Retain this canonical suffix across refill. Descendants have not read
                    //     their pending parent's old record, and the journal may be empty.
                    next_published_index = published_index;
                    pending_parent_inputs = Some(parent_inputs_moved);
                    suspended_on_font = true;
                    break;
                }
                let engine_computed_delta = engine_record_answer.and_then(Result::ok);
                next_published_index = published_index + 1;
                // A first record C++ declines for the custom-property environment it inherits
                // takes its descendants' first records down with it: a descendant's environment is
                // the parent's own, which fails the same check whenever the parent's did.
                // What this node tells its children, decided here, where it settles. Every
                // processed node keeps a row, settled or not: that is what lets a
                // descendant's fold stop at it instead of walking past it to the root.
                let settled = direct_inherited_delta.is_some() || engine_computed_delta.is_some();
                if let Some(index) = node.element_index() {
                    pass.scratch.derived_child_inputs.insert(
                        index as usize,
                        publication::DerivedChildInputs {
                            settled,
                            declined: !settled && !hidden,
                            hidden,
                            inheritance_unresolved: !settled && reaction == transaction::STYLE_REACTION_INHERITED_STYLE,
                            // A child folds the chain when it asks; this node's own facts
                            // are final from here on, so the fold it caches is kept.
                            chain: None,
                        },
                    );
                }
                let (old_style_record, new_style_record, damage, gap) =
                    match (direct_inherited_delta, engine_computed_delta) {
                        (Some((old_style_record, new_style_record)), _) => (
                            old_style_record.raw(),
                            new_style_record.raw(),
                            FfiStyleDeltaDamage::Full,
                            FfiStyleDeltaGap::None,
                        ),
                        (None, Some((old_style_record, new_style_record))) => (
                            old_style_record.raw(),
                            new_style_record.raw(),
                            FfiStyleDeltaDamage::Full,
                            FfiStyleDeltaGap::Computed,
                        ),
                        (None, None) => (
                            old_style_record,
                            0,
                            FfiStyleDeltaDamage::None,
                            if hidden {
                                FfiStyleDeltaGap::Hidden
                            } else {
                                FfiStyleDeltaGap::Materialize
                            },
                        ),
                    };
                // A moved inherited environment the engine leaves to C++ reaches what the node's
                // custom declarations and substitutions read: C++ recomputes such a node, which
                // it cannot tell from an engine-computed record.
                let reaction = if gap == FfiStyleDeltaGap::Materialize
                    && reaction & transaction::STYLE_REACTION_INHERITED_CUSTOM_PROPERTIES != 0
                    && reaction & transaction::STYLE_REACTION_RECOMPUTE_STYLE == 0
                    && self.node_style_reads_custom_properties(node)
                {
                    reaction | transaction::STYLE_REACTION_RECOMPUTE_STYLE
                } else {
                    reaction
                };
                let style_delta = PublishedStyleDeltaRecord {
                    style_node: node.raw(),
                    match_answer: answer_cascade_input.map_or(0, |cascade_input| cascade_input.0),
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
                    uses_substitution: gap == FfiStyleDeltaGap::Computed && pass.scratch.element_uses_substitution,
                    record_reads: if gap == FfiStyleDeltaGap::Computed {
                        self.node_record_reads(node)
                    } else {
                        0
                    },
                    explicitly_inherited_groups: if gap == FfiStyleDeltaGap::Computed {
                        pass.scratch.element_explicitly_inherited_groups
                    } else {
                        0
                    },
                    // What the element's move damages is answered with the record it computed or
                    // the inherited groups it swapped.
                    record_damage: if matches!(gap, FfiStyleDeltaGap::None | FfiStyleDeltaGap::Computed)
                        && old_style_record != 0
                        && old_style_record != new_style_record
                    {
                        self.retained
                            .element_record_damage(node, false, old_style_record, new_style_record)
                            | bridge::FfiStyleInvalidationField::EngineComputed as u32
                    } else {
                        0
                    },
                    owes_an_animation_plan: gap == FfiStyleDeltaGap::Computed
                        && self
                            .retained
                            .row_owes_an_animation_plan(node, old_style_record, new_style_record),
                    owes_a_transition_step: gap == FfiStyleDeltaGap::Computed
                        && self.retained.row_owes_a_transition_step(node),
                    composed_by_the_host: gap == FfiStyleDeltaGap::Computed
                        && self
                            .retained
                            .host_composes_row(node, old_style_record, new_style_record),
                };
                if style_deltas.len() == style_deltas.capacity() {
                    style_deltas.reserve(1);
                    style_delta_memory.resize_required_to(
                        &mut self.retained.memory,
                        capacity::ShallowCapacityBytes::shallow_capacity_bytes(&*style_deltas),
                    );
                }
                let row_index = style_deltas.len();
                style_deltas.push(style_delta);
                // The pseudo-element records the engine settled beside an engine-computed record
                // follow it, for C++ to install with it.
                if gap == FfiStyleDeltaGap::Computed {
                    for pseudo in pass.scratch.pseudo_deltas.drain(..) {
                        let (old_pseudo_record, new_pseudo_record) =
                            (pseudo.old_style_record.raw(), pseudo.new_style_record.raw());
                        // What the pseudo-element's move damages is answered with its record
                        // too, decided against the element's new record.
                        let record_damage = self.retained.pseudo_element_record_damage(
                            node,
                            pseudo.kind,
                            old_pseudo_record,
                            new_pseudo_record,
                            new_style_record,
                            false,
                        ) | bridge::FfiStyleInvalidationField::EngineComputed as u32;
                        if style_deltas.len() == style_deltas.capacity() {
                            style_deltas.reserve(1);
                            style_delta_memory.resize_required_to(
                                &mut self.retained.memory,
                                capacity::ShallowCapacityBytes::shallow_capacity_bytes(&*style_deltas),
                            );
                        }
                        style_deltas.push(PublishedStyleDeltaRecord {
                            style_node: node.raw(),
                            match_answer: style_delta.match_answer,
                            old_style_record: old_pseudo_record,
                            new_style_record: new_pseudo_record,
                            damage: FfiStyleDeltaDamage::Full,
                            reaction: transaction::STYLE_REACTION_PUBLISHED_STYLE,
                            inherited_style_groups: 0,
                            pseudo_kind: pseudo.kind,
                            gap: FfiStyleDeltaGap::Computed,
                            uses_substitution: false,
                            record_reads: 0,
                            explicitly_inherited_groups: 0,
                            record_damage,
                            owes_an_animation_plan: false,
                            owes_a_transition_step: false,
                            composed_by_the_host: false,
                        });
                    }
                }
                // A host's element-backed pseudo-elements are elements of its shadow tree, which
                // take the host's rules: the moved ones restyle as their own rows.
                if element_reference_kinds_moved != 0
                    && matches!(gap, FfiStyleDeltaGap::None | FfiStyleDeltaGap::Computed)
                {
                    self.record_backing_element_inputs(node, element_reference_kinds_moved);
                }
                // What applying a row the engine settled derives for the element's children is read
                // from the row's records and their damage. A child that is a row of this batch takes
                // it as its own reaction before it settles, rather than in another transaction once
                // the host's application of this row derived it; the host still derives it for every
                // other child, and finds it taken for a child the batch settled with it.
                if matches!(gap, FfiStyleDeltaGap::None | FfiStyleDeltaGap::Computed)
                    && self.engine_row_derives_children(node, old_style_record, new_style_record)
                {
                    let mut derived = smallvec::SmallVec::<[child_reactions::DerivedChildReaction; 8]>::new();
                    self.derive_engine_row_child_reactions(
                        node,
                        style_delta.reaction,
                        old_style_record,
                        new_style_record,
                        style_deltas[row_index..].iter().map(|row| row.record_damage),
                        |child| {
                            if pass.published_match_answers.lookup(child.child).is_some() {
                                derived.push(child);
                            }
                        },
                    );
                    for child in derived {
                        if child.parent_display_moved {
                            pass.inputs_moved.note_parent_display_moved(child.child);
                        }
                        if child.reaction != 0 {
                            let joined = pass.derived_child_reactions.entry(child.child).or_insert((0, 0));
                            joined.0 |= child.reaction;
                            joined.1 |= child.inherited_style_groups;
                        }
                    }
                }
                // NB: Sample scratch coexistence without scanning its containers per element.
                if (published_index + 1).is_multiple_of(256) {
                    computation_scratch_memory
                        .resize_required_to(&mut self.retained.memory, pass.scratch.capacity_bytes());
                }
            }
            if suspended_on_font {
                let request = pass.scratch.font_drive.take_suspended_request();
                computation_scratch_memory.resize_required_to(&mut self.retained.memory, pass.scratch.capacity_bytes());
                let node = pass.published_nodes[next_published_index];
                self.refill_font_request(node, request, counters);
            }
        }
        // The host installed every row before the one the wave started at, so that row waits for
        // nothing: a wave that settled no row would be taken again as it is, forever.
        debug_assert!(
            next_published_index > wave_start || wave_start == pass.published_nodes.len(),
            "a style pass wave stops at the row it started at"
        );
        pass.next_index = next_published_index;
    }

    /// Take the next wave of the pass the host is installing: the rows up to the next one that
    /// waits for a row the host installs, settled over the rows the host installed from the waves
    /// before.
    fn continue_style_pass(
        &mut self,
        mut pass: StylePass,
        mut emit: impl FnMut(StyleTransactionVersion, ProgramVersion, &[PublishedStyleDeltaRecord]),
        counters: &mut Counters,
    ) -> bool {
        // A record the host did not install from the last wave is gone, as at a transaction
        // boundary: the host answered that row from its demand, or skipped it.
        self.discard_engine_computed_records(counters);
        // The host took the container effects of the rows it installed from the last wave.
        self.retained.container_effects_for_host.clear();
        self.join_reactions_of_installed_rows(&mut pass, counters);
        pass.published_match_answers = std::mem::take(&mut self.retained.published_match_answers);
        // Every row of the waves before is installed: its record is the one its descendants inherit
        // from, and what it moved is in place, which a fresh scratch reads as it now stands.
        pass.scratch = publication::EngineComputedRecordScratch::for_next_wave(&pass.scratch);
        let font_environment_generation = self
            .retained
            .document_style_computation_inputs
            .font_environment_generation;
        if let Some(resolver) = &mut self.retained.font_resolution {
            resolver.prepare(font_environment_generation);
        }
        let mut style_delta_memory = MemoryLease::new(MemoryCategory::BridgeBuffer);
        let mut computation_scratch_memory = MemoryLease::new(MemoryCategory::BatchScratch);
        let mut style_deltas = Vec::new();
        self.run_style_pass(
            &mut pass,
            &mut style_deltas,
            &mut style_delta_memory,
            &mut computation_scratch_memory,
            counters,
        );
        computation_scratch_memory.release();
        if !style_deltas.is_empty() {
            self.settle_computed_memory();
            counters.add(Counter::PublishedMatchAnswerRecords, style_deltas.len() as u64);
            emit(pass.transaction_version, pass.program_version, &style_deltas);
        }
        style_delta_memory.release();
        self.retained.published_match_answers = std::mem::take(&mut pass.published_match_answers);
        let scoped = pass.scoped;
        if pass.next_index < pass.published_nodes.len() {
            self.host.suspended_style_pass = Some(pass);
        } else {
            self.retained.clear_batch_answer_facts();
        }
        // A wave continues the style change whose rows the host installed last.
        self.retained.last_transaction_only_derived_child_reactions = true;
        scoped
    }

    /// What the host's installation of the last wave derived for the rows still to come joins the
    /// pass: a row takes it with its own reaction, and an element between an installed row and a
    /// row still to come joins the pass before the first row below it, as the host would have
    /// settled it in the batch that applied its ancestor.
    fn join_reactions_of_installed_rows(&mut self, pass: &mut StylePass, counters: &mut Counters) {
        if !self.has_deferred_element_style_inputs() {
            return;
        }
        let rows = &pass.published_nodes[pass.next_index..];
        for &node in rows {
            let absorbed = self.absorb_element_style_input(node, 0, 0, true);
            if absorbed == 0 {
                continue;
            }
            let joined = pass.derived_child_reactions.entry(node).or_insert((0, 0));
            joined.0 |= (absorbed & 0xff) as u8;
            joined.1 |= (absorbed >> 8) as u8;
            pass.inputs_moved.join(&mut self.retained.row_inputs_moved, node);
        }
        if !self.has_deferred_element_style_inputs() {
            return;
        }
        let pass_rows: HashSet<StyleNodeID> = pass.published_nodes.iter().copied().collect();
        let mut visited = HashSet::<StyleNodeID>::default();
        // Each joining element, outermost first, beside the first row still to come below it.
        let mut joining = Vec::<(usize, StyleNodeID)>::new();
        let mut chain = Vec::new();
        for (index, &row) in rows.iter().enumerate() {
            chain.clear();
            let mut ancestor = self.tree.inheritance_parent(row);
            while let Some(current) = ancestor
                && !pass_rows.contains(&current)
                && visited.insert(current)
            {
                if self.has_deferred_element_style_input(current) {
                    chain.push(current);
                }
                ancestor = self.tree.inheritance_parent(current);
            }
            joining.extend(chain.iter().rev().map(|&node| (pass.next_index + index, node)));
        }
        if joining.is_empty() {
            return;
        }
        let nodes: Vec<StyleNodeID> = joining.iter().map(|&(_, node)| node).collect();
        // NB: An element whose answer cannot be completed stays owed to the next transaction, as it
        //     would be had the host not closed a batch over it.
        if self
            .retained
            .complete_published_match_answers_for_closure(&nodes, counters)
            .is_err()
        {
            return;
        }
        self.install_pending_matching_context();
        let answers = std::mem::take(&mut self.retained.published_match_answers);
        let mut published_nodes = Vec::with_capacity(pass.published_nodes.len() + joining.len());
        let mut previous_cascade_inputs = Vec::with_capacity(published_nodes.capacity());
        let mut joining = joining.into_iter().peekable();
        for (index, &node) in pass.published_nodes.iter().enumerate() {
            while let Some((_, joined)) = joining.next_if(|&(before, _)| before == index) {
                let absorbed = self.absorb_element_style_input(joined, 0, 0, true);
                let position = pass
                    .style_input_reactions
                    .partition_point(|&(style_node, _, _)| style_node < joined);
                pass.style_input_reactions
                    .insert(position, (joined, (absorbed & 0xff) as u8, (absorbed >> 8) as u8));
                pass.inputs_moved.join(&mut self.retained.row_inputs_moved, joined);
                self.retained.note_batch_answer_facts(joined, &answers);
                published_nodes.push(joined);
                previous_cascade_inputs.push(None);
            }
            published_nodes.push(node);
            previous_cascade_inputs.push(pass.previous_cascade_inputs[index]);
        }
        self.retained.published_match_answers = answers;
        pass.published_nodes = published_nodes;
        pass.previous_cascade_inputs = previous_cascade_inputs;
    }

    /// Give up a pass the host is installing: each row it did not reach is owed again, to the next
    /// transaction, with what moved under it, and is driven there in full.
    pub(super) fn abandon_style_pass(&mut self, pass: StylePass) {
        self.retained.clear_batch_answer_facts();
        let unreached = &pass.published_nodes[pass.next_index..];
        for &node in unreached {
            let (reaction, inherited_style_groups) = pass
                .style_input_reactions
                .binary_search_by_key(&node, |&(style_node, _, _)| style_node)
                .map_or((transaction::STYLE_REACTION_PUBLISHED_STYLE, 0), |index| {
                    (pass.style_input_reactions[index].1, pass.style_input_reactions[index].2)
                });
            let (derived_reaction, derived_groups) =
                pass.derived_child_reactions.get(&node).copied().unwrap_or_default();
            self.record_derived_element_style_input(
                node,
                reaction | derived_reaction | transaction::STYLE_REACTION_RECOMPUTE_STYLE,
                inherited_style_groups | derived_groups,
            );
        }
        pass.inputs_moved
            .give_back(unreached, &mut self.retained.row_inputs_moved);
    }

    #[must_use]
    /// Materialize old child sequences directly from the tree family's frozen before rows.
    pub(super) fn install_before_sibling_geometry(&self, view: &mut TransactionFactView) -> bool {
        let staged_rows: Vec<_> = self.host.tree_staging.rows().collect();
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
        parents.extend(self.host.tree_staging.first_children().map(|(parent, _, _)| parent));
        parents.sort_unstable();
        parents.dedup();

        // Index the fallback first children once. Scanning every staged row for each parent
        // would make batches with many newly created or removed subtrees quadratic.
        let mut before_first_children = HashMap::default();
        for &(node, before, _) in &staged_rows {
            if before.is_none() {
                view.mark_before_absent(node);
            }
            if let Some(relations) = before
                && let Some(parent) = relations.parent
                && relations.previous_element_sibling.is_none()
            {
                before_first_children.entry(parent).or_insert(node);
            }
        }

        let maximum_sequence_length = self.retained.tree.connected_element_count() as usize + staged_rows.len() + 1;
        for parent in parents {
            let resident_first = self
                .tree
                .is_live(parent)
                .then(|| self.retained.tree.first_element_child(parent))
                .flatten();
            let mut child = self
                .host
                .tree_staging
                .before_first_child(parent, resident_first)
                .or_else(|| before_first_children.get(&parent).copied());
            let mut sequence = Vec::new();
            while let Some(node) = child {
                assert!(
                    sequence.len() < maximum_sequence_length,
                    "frozen before-side child sequence must be acyclic"
                );
                sequence.push(node);
                let resident = self
                    .retained
                    .tree
                    .is_live(node)
                    .then(|| self.settled_tree_relations(node));
                child = self
                    .host
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

impl StyleEngineState {
    /// Restyle the elements of a host's shadow tree that back its pseudo-elements of the given
    /// `kinds`, as the rules they take, the host's, moved.
    fn record_backing_element_inputs(&mut self, host: StyleNodeID, kinds: u64) {
        let backing_elements: SmallVec<[StyleNodeID; 2]> = self.retained.backing_elements(host, kinds).collect();
        for node in backing_elements {
            self.record_derived_element_style_input(node, transaction::STYLE_REACTION_RECOMPUTE_STYLE, 0);
        }
    }

    pub fn take_style_transaction(
        &mut self,
        root: StyleNodeID,
        emit: impl FnMut(StyleTransactionVersion, ProgramVersion, &[PublishedStyleDeltaRecord]),
        counters: &mut Counters,
    ) -> bool {
        let mut clock = TransactionClock::new();
        self.install_witness_effects();
        self.install_pending_matching_context();
        let scoped = match self.host.suspended_style_pass.take() {
            // A wave reads the inputs and facts its pass was planned from. Once the host submitted an
            // input as it installed the waves before, such as a declaration of a row still to come,
            // or the tree, its features or the rule program moved, the rows the pass did not reach
            // are owed to the transaction that takes those changes instead.
            Some(pass)
                if self.host.journal.is_empty()
                    && !self.facts.has_dirty_staging()
                    && self.host.tree_staging.is_empty()
                    && !self.host.program_staging.is_dirty()
                    && self.host.sheet_rule_replacement.is_none() =>
            {
                self.continue_style_pass(pass, emit, counters)
            }
            pass => {
                if let Some(pass) = pass {
                    self.abandon_style_pass(pass);
                }
                self.take_style_transaction_with_clock(root, emit, &mut clock, counters)
            }
        };
        self.finish_memory_evaluation_loop();
        // Include transaction-local destruction on both ordinary and early-return paths.
        clock.finish(counters);
        scoped
    }
}
