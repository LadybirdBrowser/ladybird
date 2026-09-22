/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What an evaluation step may read, written down as compile-time witnesses.
//!
//! An evaluation step borrows the engine's retained state and the update's committed answers, and
//! writes only this flush's scratch. For many workers to run the same step over one read side,
//! everything the step reads has to be shareable across threads and the per-worker scratch has to
//! be movable to one. None of this is a runtime check: a type that stops being shareable stops
//! compiling, which is how the port of the read side found its own work list.

use super::*;

const _: () = {
    const fn assert_sync<T: Sync + ?Sized>() {}
    assert_sync::<PublishedMatchAnswers>();
};

fn assert_member_is_sync<T: Sync + ?Sized>(_member: &T) {}

/// Every member of `RetainedState` is `Sync`, except the two named at the end.
///
/// The destructuring is exhaustive on purpose: there is no `..`, so a new member of the retained
/// state does not compile until it is named here, either as shareable or as one of the exemptions
/// below. That is what keeps `assert_sync::<RetainedState>()` -- which this becomes, unchanged in
/// meaning, once the exemptions are gone -- from being a list somebody forgets to update.
///
/// Both exemptions are one thing: the prefix caches, which a single `Rc<RefCell<PrefixCaches>>`
/// shares between the engine and every matching traversal that borrows it -- so
/// `batch_matching_traversal` fails the bound for exactly the reason `prefix_caches` does.
/// Giving a walk prefix caches of its own is what removes both. A lock here would not: the
/// borrows nest, so it would only turn a loud re-entrant panic into a silent deadlock.
#[expect(dead_code, reason = "a compile-time witness, never called")]
fn every_retained_member_is_shareable(state: &RetainedState) {
    let RetainedState {
        memory,
        admission,
        deferred_pseudo_element,
        tree,
        program,
        native_rules,
        declaration_block_version,
        last_transaction_only_derived_child_reactions,
        sheets_excluded_from_routing,
        routing_needs_detachment_sweep,
        match_workspace,
        query_match_workspace,
        selector_query_generation,
        query_settled_transaction_version,
        query_sorted_candidates,
        query_sorted_candidates_stamp,
        query_preorder_ranks,
        query_preorder_ranks_stamp,
        query_workspace_generation,
        exact_covered_scratch,
        cascade_compaction_scratch,
        cascade_compaction_scratch_memory,
        next_style_transaction_version,
        document_style_computation_inputs,
        font_resolution,
        layer_topology_version,
        sheet_order_version,
        specified_values,
        winner_groups,
        computed_group_sets,
        custom_property_environments,
        nodes_with_substituted_records,
        custom_property_registrations_changed,
        pending_element_style_computation_selections,
        pending_pseudo_style_computation_selections,
        engine_computed_records_pending,
        flush_stamp,
        style_input_nodes_for_cpp,
        parent_inputs_moved_nodes,
        engine_pseudo_record_cache,
        engine_cold_record_cache,
        engine_cold_record_donors,
        computed_group_set_memory,
        custom_property_environment_memory,
        computed_fixed_metadata_memory,
        computed_longhand_table_memory,
        style_record_memory,
        animation_overlay_memory,
        computed_pseudo_assignment_memory,
        style_invalidation_cache,
        match_answers,
        selector_truth_sets,
        retained_match_answers,
        retained_selector_incidences,
        selector_incidence_is_current,
        batch_matching_traversal,
        route_pruning_states,
        completion_exactness,
        prefix_caches,
        #[cfg(test)]
        force_bounded_prefix_completion,
        prepared_batch_matching_traversal,
        published_match_answers,
        transaction_fact_view,
        facts,
        programs,
        attribute_value_text_names,
        attribute_value_text_requirements_version,
        selector_programs_need_sweep,
        routing,
        selector_truth_changes,
        already_planned_selector_truth,
        selector_truth_changes_active,
        relational_witnesses,
        pending_witness_effects,
        witness_effect_scratch,
        relational_witness_residency,
        scope_roots,
        scope_by_root,
        scope_programs,
        vacant_scope_programs,
        scope_dispatch_templates,
        scope_cascade_templates,
        ancestor_dispatch_templates,
        scope_program_by_scope,
        atoms,
        html_element_namespace,
        fold_id_and_class_name_case,
        #[cfg(test)]
        diagnostic_plan_capture,
    } = state;
    assert_member_is_sync(memory);
    assert_member_is_sync(admission);
    assert_member_is_sync(deferred_pseudo_element);
    assert_member_is_sync(tree);
    assert_member_is_sync(program);
    assert_member_is_sync(native_rules);
    assert_member_is_sync(declaration_block_version);
    assert_member_is_sync(last_transaction_only_derived_child_reactions);
    assert_member_is_sync(sheets_excluded_from_routing);
    assert_member_is_sync(routing_needs_detachment_sweep);
    assert_member_is_sync(match_workspace);
    assert_member_is_sync(query_match_workspace);
    assert_member_is_sync(selector_query_generation);
    assert_member_is_sync(query_settled_transaction_version);
    assert_member_is_sync(query_sorted_candidates);
    assert_member_is_sync(query_sorted_candidates_stamp);
    assert_member_is_sync(query_preorder_ranks);
    assert_member_is_sync(query_preorder_ranks_stamp);
    assert_member_is_sync(query_workspace_generation);
    assert_member_is_sync(exact_covered_scratch);
    assert_member_is_sync(cascade_compaction_scratch);
    assert_member_is_sync(cascade_compaction_scratch_memory);
    assert_member_is_sync(next_style_transaction_version);
    assert_member_is_sync(document_style_computation_inputs);
    assert_member_is_sync(layer_topology_version);
    assert_member_is_sync(sheet_order_version);
    assert_member_is_sync(specified_values);
    assert_member_is_sync(winner_groups);
    assert_member_is_sync(nodes_with_substituted_records);
    assert_member_is_sync(custom_property_registrations_changed);
    assert_member_is_sync(pending_element_style_computation_selections);
    assert_member_is_sync(pending_pseudo_style_computation_selections);
    assert_member_is_sync(engine_computed_records_pending);
    assert_member_is_sync(flush_stamp);
    assert_member_is_sync(style_input_nodes_for_cpp);
    assert_member_is_sync(parent_inputs_moved_nodes);
    assert_member_is_sync(engine_pseudo_record_cache);
    assert_member_is_sync(engine_cold_record_cache);
    assert_member_is_sync(engine_cold_record_donors);
    assert_member_is_sync(computed_group_set_memory);
    assert_member_is_sync(custom_property_environment_memory);
    assert_member_is_sync(computed_fixed_metadata_memory);
    assert_member_is_sync(computed_longhand_table_memory);
    assert_member_is_sync(style_record_memory);
    assert_member_is_sync(animation_overlay_memory);
    assert_member_is_sync(computed_pseudo_assignment_memory);
    assert_member_is_sync(style_invalidation_cache);
    assert_member_is_sync(match_answers);
    assert_member_is_sync(selector_truth_sets);
    assert_member_is_sync(retained_match_answers);
    assert_member_is_sync(retained_selector_incidences);
    assert_member_is_sync(selector_incidence_is_current);
    assert_member_is_sync(route_pruning_states);
    assert_member_is_sync(completion_exactness);
    #[cfg(test)]
    assert_member_is_sync(force_bounded_prefix_completion);
    assert_member_is_sync(prepared_batch_matching_traversal);
    assert_member_is_sync(published_match_answers);
    assert_member_is_sync(transaction_fact_view);
    assert_member_is_sync(facts);
    assert_member_is_sync(programs);
    assert_member_is_sync(attribute_value_text_names);
    assert_member_is_sync(attribute_value_text_requirements_version);
    assert_member_is_sync(selector_programs_need_sweep);
    assert_member_is_sync(routing);
    assert_member_is_sync(selector_truth_changes);
    assert_member_is_sync(already_planned_selector_truth);
    assert_member_is_sync(selector_truth_changes_active);
    assert_member_is_sync(relational_witnesses);
    assert_member_is_sync(pending_witness_effects);
    assert_member_is_sync(witness_effect_scratch);
    assert_member_is_sync(relational_witness_residency);
    assert_member_is_sync(scope_roots);
    assert_member_is_sync(scope_by_root);
    assert_member_is_sync(scope_programs);
    assert_member_is_sync(vacant_scope_programs);
    assert_member_is_sync(scope_dispatch_templates);
    assert_member_is_sync(scope_cascade_templates);
    assert_member_is_sync(ancestor_dispatch_templates);
    assert_member_is_sync(scope_program_by_scope);
    assert_member_is_sync(atoms);
    assert_member_is_sync(html_element_namespace);
    assert_member_is_sync(fold_id_and_class_name_case);
    assert_member_is_sync(custom_property_environments);
    assert_member_is_sync(font_resolution);
    assert_member_is_sync(computed_group_sets);
    #[cfg(test)]
    assert_member_is_sync(diagnostic_plan_capture);
    // Exempt: the document's shared prefix caches; see above.
    let _ = prefix_caches;
    let _ = batch_matching_traversal;
}
