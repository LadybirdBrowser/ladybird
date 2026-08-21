/*
 * Copyright (c) 2018-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/ScopeGuard.h>
#include <LibGC/RootVector.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Invalidation/SlotInvalidator.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/PseudoElement.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/Box.h>

namespace Web::CSS {

using StyleUpdateMode = DOM::Document::StyleUpdateMode;

extern "C" void ladybird_animated_properties_unref(void const*);
extern "C" void ladybird_string_unref(size_t);
extern "C" void ladybird_utf16_fly_string_unref(size_t);

static void finish_complete_style_update()
{
    auto releases = StyleValueFFI::rust_style_ffi_complete_style_update_end();
    ScopeGuard clear_releases = StyleValueFFI::rust_deferred_cpp_releases_clear;
    for (size_t i = 0; i < releases.fly_string_count; ++i)
        ladybird_utf16_fly_string_unref(releases.fly_strings[i]);
    for (size_t i = 0; i < releases.string_count; ++i)
        ladybird_string_unref(releases.strings[i]);
    for (size_t i = 0; i < releases.animated_property_count; ++i)
        ladybird_animated_properties_unref(releases.animated_properties[i]);
}

static void update_style(DOM::Document&);
static bool update_style_for_element(DOM::Document&, DOM::AbstractElement const&, StyleUpdateMode);

static void apply_element_style_invalidation_after_style_change(DOM::Element& element, RequiredInvalidationAfterStyleChange const& invalidation)
{
    if (invalidation.accumulated_visual_contexts() == AccumulatedVisualContextInvalidation::UpdateValues)
        element.document().schedule_accumulated_visual_context_value_update(element);

    if (invalidation.needs_scrollable_overflow_recalculation())
        element.document().schedule_scrollable_overflow_recalculation(element);

    if (invalidation.changes_containing_block_establishment)
        element.document().partial_relayout_invalidation().record_escape(DOM::PartialRelayoutEscapeReason::ContainingBlockEstablishmentChangedByStyleChange);

    if (invalidation.needs_relayout()) {
        // A relayout-only style change on an absolutely positioned partial relayout boundary
        // stays confined to it: the box contributes nothing to ancestor layout, and partial
        // relayout re-resolves the boundary's own size and position. A rendered ::backdrop
        // disqualifies the element, because pseudo-element style diffs are merged into the
        // element's invalidation while the ::backdrop box is a sibling of the element's box,
        // outside the subtree a boundary-self relayout covers.
        auto* box = as_if<Layout::Box>(element.unsafe_layout_node());
        if (!invalidation.needs_layout_tree_rebuild()
            && box
            && box->is_absolutely_positioned()
            && box->is_partial_relayout_boundary()
            && !element.pseudo_element_unsafe_layout_node(CSS::PseudoElement::Backdrop)) {
            box->set_needs_own_geometry_update();
            element.set_needs_layout_update(DOM::SetNeedsLayoutReason::StyleChange, Layout::LayoutUpdatePropagation::BoundarySelfOnly);
        } else {
            element.set_needs_layout_update(DOM::SetNeedsLayoutReason::StyleChange);
        }
    }
    if (invalidation.needs_layout_tree_rebuild())
        element.set_needs_layout_tree_rebuild(DOM::SetNeedsLayoutTreeUpdateReason::StyleChange, invalidation.layout_tree_rebuild_root());
}

static void apply_document_style_invalidation_after_style_change(DOM::Document& document, RequiredInvalidationAfterStyleChange const& invalidation)
{
    if (invalidation.needs_repaint())
        document.set_needs_to_record_display_list();
    if (invalidation.accumulated_visual_contexts() == AccumulatedVisualContextInvalidation::Rebuild)
        document.set_needs_accumulated_visual_contexts_update(true);
    if (invalidation.needs_stacking_context_tree_rebuild())
        document.invalidate_stacking_context_tree();
}

// Consume everything recorded since the last transaction boundary and publish its match answers.
//
// The reaction batch is a superset by construction: routing may over-approximate, and every subject
// it yields is checked exactly before publication. What it may not do is under-approximate, so a
// region that could not be proven narrower covers the whole document rather than guessing at part
// of it. Reactions are consumed as the engine emits them so bridge scratch cannot determine the
// transaction's scope.
struct StyleEngineTransaction {
    Vector<StyleEngine::PublishedStyleDelta> reactions;
    Optional<StyleEngine::PublishedTransactionVersion> published_version;
    bool prefers_broad_matching_batch { false };
};

static Vector<u64> style_inheritance_path(DOM::Element const& element)
{
    Vector<u64> path;
    for (Optional<DOM::AbstractElement> ancestor = DOM::AbstractElement { element }; ancestor.has_value();) {
        auto parent = ancestor->element_to_inherit_style_from();
        u64 branch = 0;
        if (parent.has_value()) {
            // Shadow-tree children are traversed before the host's light-tree children. Assigned
            // slottables follow the slot's own fallback subtree.
            if (ancestor->element().assigned_slot_internal() == GC::Ptr<DOM::Element> { parent->element() })
                branch = 2;
            else if (parent->element().shadow_root() && !ancestor->element().parent_node()->is_shadow_root())
                branch = 1;
        }
        path.append((branch << 32) | ancestor->element().style_node_id().value());
        ancestor = parent;
    }
    return path;
}

static void sort_style_engine_reactions_for_direct_application(StyleComputer& style_computer, Vector<StyleEngine::PublishedStyleDelta>& reactions)
{
    // Apply each inheritance branch contiguously in preorder. Besides making every parent ready
    // before its descendants, this lets a parent's derived reaction merge into an unconsumed child
    // reaction in the same batch.
    HashMap<u32, Vector<u64>> style_inheritance_paths;
    for (auto const& reaction : reactions) {
        auto element = style_computer.element_for_style_node(reaction.style_node);
        VERIFY(element);
        style_inheritance_paths.set(reaction.style_node, style_inheritance_path(*element));
    }
    quick_sort(reactions, [&](auto const& first, auto const& second) {
        auto const& first_path = style_inheritance_paths.get(first.style_node).value();
        auto const& second_path = style_inheritance_paths.get(second.style_node).value();
        auto common_length = min(first_path.size(), second_path.size());
        for (size_t offset = 1; offset <= common_length; ++offset) {
            auto first_node = first_path[first_path.size() - offset];
            auto second_node = second_path[second_path.size() - offset];
            if (first_node != second_node)
                return first_node < second_node;
        }
        return first_path.size() < second_path.size();
    });
}

static StyleEngineTransaction take_style_engine_transaction(DOM::Document& document)
{
    StyleEngineTransaction transaction;
    auto& style_computer = document.style_computer();
    // One element's computed style answers for another only while the inputs it was keyed on still
    // mean what they meant. A transaction boundary is exactly where they stop doing so: a
    // declaration keyed on by identity may have been edited, and a sheet may have come or gone.
    auto transaction_setup_started_at = MonotonicTime::now();
    ++document.style_invalidation_counters().style_engine_transaction_setups;
    style_computer.prepare_for_style_engine_transaction();
    document.style_invalidation_counters().style_engine_transaction_setup_microseconds += (MonotonicTime::now() - transaction_setup_started_at).to_microseconds();
    auto* root = document.document_element();
    if (!root || root->style_node_id() == 0) {
        style_computer.style_engine().flush();
        return transaction;
    }

    auto planning_started_at = MonotonicTime::now();
    auto published_transaction = style_computer.style_engine().take_style_transaction(root->style_node_id());
    if (!published_transaction.reactions.is_empty())
        transaction.published_version = published_transaction.version;
    for (auto const& answer : published_transaction.reactions) {
        // The complete answer remains in Rust transaction scratch under this node. The identity
        // names both the semantic reaction and the payload that consumes it.
        VERIFY(style_computer.element_for_style_node(answer.style_node));
        transaction.reactions.append(answer);
    }
    document.style_invalidation_counters().style_engine_planning_microseconds += (MonotonicTime::now() - planning_started_at).to_microseconds();

    // A reaction batch covering more than one sixteenth of the connected elements is dense enough that
    // packing the scope once is cheaper than repeatedly reconstructing cold facts while matching
    // the planned elements.
    transaction.prefers_broad_matching_batch = !published_transaction.is_scoped
        || transaction.reactions.size() * 16 > style_computer.style_engine().connected_element_count();

    return transaction;
}

static void record_direct_child_style_engine_inputs(DOM::ParentNode& parent, Optional<StyleNodeID> excluded_style_node = {})
{
    parent.for_each_child([&](auto& child) {
        auto* element = as_if<DOM::Element>(child);
        if (element && element->style_node_id() != 0
            && (!excluded_style_node.has_value() || element->style_node_id() != *excluded_style_node))
            element->document().style_computer().style_engine().record_element_style_input_change(element->style_node_id());
        return IterationDecision::Continue;
    });
}

static void record_style_engine_reaction(HashMap<u32, StyleEngine::PublishedStyleDelta*>& reaction_batch, DOM::Element& element, u8 reaction, u8 inherited_style_groups = 0)
{
    if (reaction == 0 || element.style_node_id() == 0)
        return;
    if (auto pending_reaction = reaction_batch.find(element.style_node_id().value()); pending_reaction != reaction_batch.end()) {
        auto& pending = *pending_reaction->value;
        if (!StyleEngine::published_style_delta_can_absorb_reaction(pending, reaction, inherited_style_groups)) {
            element.document().style_computer().style_engine().record_element_style_input_change(element.style_node_id(), reaction, inherited_style_groups);
            return;
        }
        pending.reaction |= reaction;
        pending.inherited_style_groups |= inherited_style_groups;
        return;
    }
    element.document().style_computer().style_engine().record_element_style_input_change(element.style_node_id(), reaction, inherited_style_groups);
}

static void record_direct_child_style_engine_reactions(DOM::ParentNode& parent, HashMap<u32, StyleEngine::PublishedStyleDelta*>& reaction_batch, u8 reaction, u8 inherited_style_groups = 0)
{
    if (reaction == 0)
        return;
    parent.for_each_child([&](auto& child) {
        auto* element = as_if<DOM::Element>(child);
        if (element && !element->assigned_slot_internal())
            record_style_engine_reaction(reaction_batch, *element, reaction, inherited_style_groups);
        return IterationDecision::Continue;
    });
}

static void record_assigned_slottable_style_engine_reactions(DOM::Element& element, HashMap<u32, StyleEngine::PublishedStyleDelta*>& reaction_batch)
{
    auto* slot = as_if<HTML::HTMLSlotElement>(element);
    if (!slot)
        return;
    for (auto const& slottable : slot->assigned_nodes_internal()) {
        if (auto const* assigned_element = slottable.get_pointer<GC::Ref<DOM::Element>>())
            record_style_engine_reaction(reaction_batch, **assigned_element, StyleEngine::RecomputeStyle);
    }
}

// The static inherited-group swap answers a pure inherited-style reaction without recomputing the element. That
// is only sound while the element's computed style is a pure function of its cascade inputs and the swapped
// groups: an element with animations may resolve keyframe values (`inherit`, neutral keyframes) against the
// parent's style, and an element with transitions or transition-property entries must compare before-change and
// after-change styles at every style change event. These are the conditions under which
// Element::apply_style_engine_reaction declines its own inherited-style group swap.
static bool element_style_depends_on_more_than_the_inherited_groups(DOM::Element const& element)
{
    return element.has_relevant_animations()
        || element.has_css_defined_animations()
        || !element.property_ids_with_existing_transitions({}).is_empty()
        || !element.property_ids_with_matching_transition_property_entry({}).is_empty();
}

static RequiredInvalidationAfterStyleChange apply_style_engine_reactions(DOM::Document& document, Span<StyleEngine::PublishedStyleDelta> reactions)
{
    HashMap<u32, StyleEngine::PublishedStyleDelta*> reaction_batch;
    reaction_batch.ensure_capacity(reactions.size());
    for (auto& reaction : reactions)
        reaction_batch.set(reaction.style_node, &reaction);

    RequiredInvalidationAfterStyleChange transaction_invalidation;
    for (auto& reaction : reactions) {
        reaction_batch.remove(reaction.style_node);
        auto element = document.style_computer().element_for_style_node(reaction.style_node);
        if (!element)
            continue;

        bool const has_published_style_reaction = reaction.reaction & StyleEngine::PublishedStyle;
        if (has_published_style_reaction) {
            ++document.style_invalidation_counters().style_engine_published_reactions;
        }
        if (reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::Materialize) {
            if (has_published_style_reaction)
                ++document.style_invalidation_counters().style_engine_materialized_gaps;
        } else {
            ++document.style_invalidation_counters().style_engine_record_deltas_applied;
        }

        if (reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::Materialize) {
            VERIFY(reaction.new_style_record == 0);
            VERIFY(reaction.damage == StyleEngineFFI::FfiStyleDeltaDamage::None);
        } else {
            VERIFY(reaction.old_style_record == element->style_record_identity().value());
            VERIFY(reaction.new_style_record != 0);
            VERIFY(reaction.damage == StyleEngineFFI::FfiStyleDeltaDamage::Full);
        }

        bool was_unstyled = false;
        bool was_display_none = false;
        {
            auto previous_style = element->computed_style();
            was_unstyled = !previous_style;
            was_display_none = previous_style && previous_style->display().is_none();
        }
        bool const needs_regular_style_recompute = reaction.reaction & (StyleEngine::PublishedStyle | StyleEngine::RecomputeStyle | StyleEngine::RecomputeDescendantStyles | StyleEngine::AncestorBecameVisible);
        bool const needs_custom_property_recompute = reaction.reaction & StyleEngine::InheritedCustomProperties;
        bool const needs_inherited_style_recompute = reaction.reaction & StyleEngine::InheritedStyle;
        bool const ancestor_became_visible = reaction.reaction & StyleEngine::AncestorBecameVisible;
        bool did_change_custom_properties = false;
        RequiredInvalidationAfterStyleChange invalidation;

        auto const* style_input_record = element->style_input_record();
        bool const cascade_reads_custom_properties = style_input_record && style_input_record->cascade_reads_custom_properties;
        bool const needs_full_custom_property_recompute = needs_custom_property_recompute && (element->style_uses_var_css_function() || element->style_uses_inherit_css_function() || cascade_reads_custom_properties);
        bool const materializes_inherited_style_reaction = needs_inherited_style_recompute && !needs_regular_style_recompute && !needs_full_custom_property_recompute;
        bool const can_use_inherited_style_group_swap = materializes_inherited_style_reaction && !needs_custom_property_recompute;
        if (reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::None) {
            VERIFY(!needs_regular_style_recompute);
            VERIFY(needs_inherited_style_recompute);
            VERIFY(!needs_custom_property_recompute);
            VERIFY(reaction.pseudo_kind == NumericLimits<u8>::max());
            if (element_style_depends_on_more_than_the_inherited_groups(*element)) {
                invalidation = element->apply_style_engine_reaction(
                    did_change_custom_properties,
                    DOM::Element::StyleEngineRecomputeReason::General,
                    0);
            } else {
                auto* input_record = element->style_input_record();
                VERIFY(input_record);
                auto const* payloads = static_cast<void const* const*>(document.style_computer().style_record_payloads(StyleRecordID { reaction.new_style_record }));
                VERIFY(payloads);
                constexpr size_t inherited_group_count = ComputedValues::inherited_style_group_count;
                VERIFY(input_record->words.size() >= inherited_group_count);
                input_record->pinned_parent_groups.set({ payloads, inherited_group_count });
                for (size_t index = 0; index < inherited_group_count; ++index)
                    input_record->words[index] = bit_cast<FlatPtr>(payloads[index]);
                element->set_computed_style({}, StyleRecordID { reaction.new_style_record });
                invalidation = RequiredInvalidationAfterStyleChange::full();
                for (size_t index = 0; index < inherited_group_count; ++index) {
                    if (reaction.inherited_style_groups & (1 << index))
                        invalidation.mark_inherited_style_group_changed(index);
                }
            }
        } else if (needs_regular_style_recompute || needs_inherited_style_recompute || needs_full_custom_property_recompute) {
            if (!needs_regular_style_recompute
                && !needs_inherited_style_recompute
                && document.style_computer().can_reuse_style_after_inherited_custom_property_change(*element)) {
                document.style_invalidation_counters().element_style_input_reused++;
            } else {
                if (materializes_inherited_style_reaction)
                    ++document.style_invalidation_counters().element_inherited_style_recomputations;
                auto recompute_reason = can_use_inherited_style_group_swap
                    ? DOM::Element::StyleEngineRecomputeReason::InheritedOnly
                    : has_published_style_reaction && !(reaction.reaction & StyleEngine::PseudoInputsMayHaveChanged)
                    ? DOM::Element::StyleEngineRecomputeReason::PseudoInputsUnchanged
                    : DOM::Element::StyleEngineRecomputeReason::General;
                // NB: The changed groups travel with the reaction whenever the swap is possible, not
                //     only when every group carries a computed closure: the swap can still decline
                //     inside the element (a current-color dependence, a non-standard inheritance,
                //     a color-scheme change), and the materialization it falls back to must learn
                //     which inherited groups changed or its masked recompute will keep stale
                //     values for them. When the swap succeeds the parameter goes unused.
                auto inherited_style_groups_for_closure = can_use_inherited_style_group_swap ? reaction.inherited_style_groups : 0;
                invalidation = element->apply_style_engine_reaction(
                    did_change_custom_properties,
                    recompute_reason,
                    inherited_style_groups_for_closure);
                if (materializes_inherited_style_reaction && invalidation.is_none() && !did_change_custom_properties)
                    ++document.style_invalidation_counters().element_inherited_style_noop_recomputations;
                if (needs_regular_style_recompute)
                    document.style_computer().style_engine().consume_recorded_element_style_input_change(reaction.style_node);
            }
        } else if (needs_custom_property_recompute && element->refresh_inherited_custom_property_data()) {
            did_change_custom_properties = true;
            element->invalidate_descendant_styles_depending_on_style_container_query();
        }

        if (!invalidation.is_none() || did_change_custom_properties || ancestor_became_visible)
            record_assigned_slottable_style_engine_reactions(*element, reaction_batch);
        apply_element_style_invalidation_after_style_change(*element, invalidation);
        transaction_invalidation |= invalidation;

        auto current_style = element->computed_style();
        VERIFY(current_style);
        if (current_style->display().is_none()) {
            if (was_unstyled) {
                record_direct_child_style_engine_reactions(*element, reaction_batch, StyleEngine::RecomputeStyle);
                if (auto shadow_root = element->shadow_root())
                    record_direct_child_style_engine_reactions(*shadow_root, reaction_batch, StyleEngine::RecomputeStyle);
            } else if (did_change_custom_properties || invalidation.inherited_style_changed() || invalidation.recompute_descendant_styles) {
                u8 child_reaction = 0;
                if (did_change_custom_properties)
                    child_reaction |= StyleEngine::InheritedCustomProperties;
                if (invalidation.inherited_style_changed())
                    child_reaction |= StyleEngine::InheritedStyle;
                if (invalidation.recompute_descendant_styles)
                    child_reaction |= StyleEngine::RecomputeDescendantStyles;
                record_direct_child_style_engine_reactions(*element, reaction_batch, child_reaction, invalidation.inherited_style_groups_changed());
                if (auto shadow_root = element->shadow_root())
                    record_direct_child_style_engine_reactions(*shadow_root, reaction_batch, child_reaction, invalidation.inherited_style_groups_changed());
            }
            continue;
        }

        u8 common_child_reaction = 0;
        if (needs_custom_property_recompute || did_change_custom_properties)
            common_child_reaction |= StyleEngine::InheritedCustomProperties;
        if (invalidation.needs_layout_tree_rebuild())
            common_child_reaction |= StyleEngine::RecomputeStyle;
        if ((reaction.reaction & StyleEngine::RecomputeDescendantStyles) || invalidation.recompute_descendant_styles)
            common_child_reaction |= StyleEngine::RecomputeDescendantStyles;
        if (ancestor_became_visible
            || (was_display_none && !current_style->in_display_none_subtree()))
            common_child_reaction |= StyleEngine::AncestorBecameVisible;

        auto light_tree_child_reaction = common_child_reaction;
        u8 light_tree_inherited_style_groups = invalidation.inherited_style_groups_changed();
        if (!invalidation.is_none() && element->children_may_depend_on_non_inherited_property_inheritance())
            light_tree_inherited_style_groups = RequiredInvalidationAfterStyleChange::all_inherited_style_groups;
        if (light_tree_inherited_style_groups != 0)
            light_tree_child_reaction |= StyleEngine::InheritedStyle;
        record_direct_child_style_engine_reactions(*element, reaction_batch, light_tree_child_reaction, light_tree_inherited_style_groups);

        if (auto shadow_root = element->shadow_root()) {
            auto shadow_tree_child_reaction = common_child_reaction;
            u8 shadow_tree_inherited_style_groups = invalidation.inherited_style_groups_changed();
            if (!invalidation.is_none() && shadow_root->children_may_depend_on_non_inherited_property_inheritance())
                shadow_tree_inherited_style_groups = RequiredInvalidationAfterStyleChange::all_inherited_style_groups;
            if (shadow_tree_inherited_style_groups != 0)
                shadow_tree_child_reaction |= StyleEngine::InheritedStyle;
            record_direct_child_style_engine_reactions(*shadow_root, reaction_batch, shadow_tree_child_reaction, shadow_tree_inherited_style_groups);
        }
    }

    return transaction_invalidation;
}

static void update_style(DOM::Document& document)
{
    StyleValueFFI::rust_style_ffi_complete_style_update_begin();
    ScopeGuard leave_complete_style_update = finish_complete_style_update;
    auto style_update_started_at = MonotonicTime::now();
    ScopeGuard record_style_update_time = [&] {
        document.style_invalidation_counters().style_update_microseconds += (MonotonicTime::now() - style_update_started_at).to_microseconds();
    };

    // NOTE: If our parent document needs a relayout, we must do that *first*. This is required as it may cause the
    // viewport to change which will can affect media query evaluation and the value of the `vw` unit.
    if (auto navigable = document.navigable(); navigable && navigable->container() && &navigable->container()->document() != &document)
        navigable->container()->document().update_layout(DOM::UpdateLayoutReason::ChildDocumentStyleUpdate);

    if (!document.browsing_context())
        return;

    // NOTE: If this is a document hosting <template> contents, style update is unnecessary.
    if (document.created_for_appropriate_template_contents())
        return;

    document.synchronize_dirty_style_attributes();

    document.begin_style_stabilization_epoch();
    ScopeGuard end_stabilization_epoch = [&] {
        document.end_style_stabilization_epoch();
    };

    // Fetch the viewport rect once, instead of repeatedly, during style computation.
    document.update_style_computer_viewport_rect();

    // An element may have rendering-only descendants that must join the transaction which first styles it. Prepare
    // those descendants before selector inputs cross the transaction boundary.
    document.style_computer().prepare_elements_for_style_computation();

    // Media rules are evaluated before the transaction boundary below, because evaluating them is
    // itself a source of inputs: a rule that starts or stops applying publishes its activation. A
    // transaction taken ahead of that would leave those inputs for the next flush, so the flush that made
    // a rule apply would not be the flush that recomputed the elements it applies to.
    if (document.needs_media_rule_evaluation())
        document.evaluate_media_rules_for_style_update();

    // The user-agent and user sheets have no author-sheet attachment event, so compare their
    // identities before deciding whether there is a transaction to take. Rendering opportunities
    // call update_style() even for quiescent documents, and animation ticks do not themselves
    // change selector or cascade inputs. Apply an animation-only update first, then take a
    // transaction only if the resulting inherited-style feedback requires one.
    record_non_author_stylesheets(document);
    if (document.has_completed_style_update()
        && !document.style_computer().style_engine().has_pending_transaction()) {
        document.update_animated_style_if_needed();
        if (!document.style_computer().style_engine().has_pending_transaction())
            return;
    }

    // A style flush is a transaction boundary. Everything recorded since the last one crosses into
    // StyleEngine as one flat batch, is normalized there, and is routed into the region its
    // transpose programs reach. A transaction that could not be proven narrower publishes a
    // complete document reaction batch. Only a transaction that cannot complete its answers falls
    // back to document invalidation.
    auto style_engine_transaction = take_style_engine_transaction(document);

    if (!style_engine_transaction.reactions.is_empty())
        document.note_style_stabilization_has_style_reactions();
    document.update_animated_style_if_needed();

    auto style_engine_reactions = move(style_engine_transaction.reactions);
    auto prefers_broad_matching_batch = style_engine_transaction.prefers_broad_matching_batch;
    if (style_engine_reactions.is_empty()
        && document.style_computer().style_engine().has_pending_transaction()) {
        auto feedback_transaction = take_style_engine_transaction(document);
        style_engine_reactions = move(feedback_transaction.reactions);
        prefers_broad_matching_batch = feedback_transaction.prefers_broad_matching_batch;
    }

    // This pass belongs to the current style change event. The outer stabilization epoch advanced
    // the transition generation once, before any style, animation, or layout feedback ran.
    document.record_style_stabilization_pass();

    if (style_engine_reactions.is_empty())
        return;

    document.build_registered_properties_cache_for_style_update();

    bool has_cold_matching_traversal = false;
    if (auto* root = document.document_element(); root && root->style_node_id() != 0) {
        if (prefers_broad_matching_batch) {
            has_cold_matching_traversal = document.style_computer().style_engine().begin_cold_matching_batch(root->style_node_id());
        } else {
            document.style_computer().style_engine().begin_adaptive_cold_matching_batch(root->style_node_id());
            has_cold_matching_traversal = true;
        }
    }
    ScopeGuard end_cold_matching_batch = [&] {
        if (has_cold_matching_traversal)
            document.style_computer().style_engine().end_cold_matching_batch();
    };

    RequiredInvalidationAfterStyleChange invalidation;
    constexpr size_t max_style_update_passes = 8;
    size_t style_update_pass = 0;
    size_t style_reaction_pass = 0;
    while (!style_engine_reactions.is_empty()) {
        if (style_reaction_pass++ > 0)
            document.record_style_stabilization_pass();

        size_t published_reaction_count = 0;
        for (auto const& reaction : style_engine_reactions) {
            if (reaction.reaction & StyleEngine::PublishedStyle)
                ++published_reaction_count;
        }
        if (published_reaction_count > 0) {
            if (++style_update_pass > max_style_update_passes) {
                ++document.style_invalidation_counters().style_update_pass_guard_hits;
                break;
            }
        }

        HashTable<StyleNodeID> reaction_set;
        reaction_set.ensure_capacity(style_engine_reactions.size());
        for (auto const& reaction : style_engine_reactions)
            reaction_set.set(StyleNodeID { reaction.style_node });
        Vector<StyleNodeID> inheritance_closure;

        // A reaction can name an element created by editing after its new inheritance parent was
        // inserted. Close the batch over unstyled inheritance prerequisites, which are bounded by
        // the reaction paths rather than discovered by a document traversal.
        for (size_t index = 0; index < style_engine_reactions.size(); ++index) {
            auto element = document.style_computer().element_for_style_node(style_engine_reactions[index].style_node);
            if (!element || !element->is_connected() || &element->document() != &document)
                continue;
            for (auto ancestor = DOM::AbstractElement { *element }.element_to_inherit_style_from(); ancestor.has_value() && !ancestor->has_style(); ancestor = ancestor->element_to_inherit_style_from()) {
                auto prerequisite = ancestor->element().style_node_id();
                VERIFY(prerequisite != 0);
                if (reaction_set.set(prerequisite) == AK::HashSetResult::InsertedNewEntry) {
                    style_engine_reactions.append({
                        .style_node = prerequisite.value(),
                        .match_answer = 0,
                        .old_style_record = 0,
                        .new_style_record = 0,
                        .damage = StyleEngineFFI::FfiStyleDeltaDamage::None,
                        .reaction = StyleEngine::RecomputeStyle,
                        .inherited_style_groups = 0,
                        .pseudo_kind = NumericLimits<u8>::max(),
                        .gap = StyleEngineFFI::FfiStyleDeltaGap::Materialize,
                    });
                    inheritance_closure.append(prerequisite);
                }
            }
        }

        // A published descendant may have an inheritance ancestor in the batch while the nodes
        // between them have no selector reaction of their own. Keep zero-bit scheduling slots for
        // that gap so derived inheritance bits can reach the descendant before its published
        // reaction is consumed.
        auto reaction_count_before_inheritance_closure = style_engine_reactions.size();
        for (size_t index = 0; index < reaction_count_before_inheritance_closure; ++index) {
            auto element = document.style_computer().element_for_style_node(style_engine_reactions[index].style_node);
            if (!element)
                continue;
            Vector<StyleNodeID> inheritance_gap;
            for (auto ancestor = DOM::AbstractElement { *element }.element_to_inherit_style_from(); ancestor.has_value(); ancestor = ancestor->element_to_inherit_style_from()) {
                auto ancestor_style_node = ancestor->element().style_node_id();
                VERIFY(ancestor_style_node != 0);
                if (reaction_set.contains(ancestor_style_node)) {
                    for (auto style_node : inheritance_gap) {
                        if (reaction_set.set(style_node) == AK::HashSetResult::InsertedNewEntry) {
                            style_engine_reactions.append({
                                .style_node = style_node.value(),
                                .match_answer = 0,
                                .old_style_record = 0,
                                .new_style_record = 0,
                                .damage = StyleEngineFFI::FfiStyleDeltaDamage::None,
                                .reaction = 0,
                                .inherited_style_groups = 0,
                                .pseudo_kind = NumericLimits<u8>::max(),
                                .gap = StyleEngineFFI::FfiStyleDeltaGap::Materialize,
                            });
                            inheritance_closure.append(style_node);
                        }
                    }
                    break;
                }
                inheritance_gap.append(ancestor_style_node);
            }
        }
        if (!inheritance_closure.is_empty())
            VERIFY(document.style_computer().style_engine().complete_published_match_answers_for_closure(inheritance_closure));

        Vector<StyleEngine::PublishedStyleDelta> applicable_style_engine_reactions;
        for (auto const& reaction : style_engine_reactions) {
            auto element = document.style_computer().element_for_style_node(reaction.style_node);
            if (!element)
                continue;
            if (!element->is_connected() || &element->document() != &document)
                continue;
            applicable_style_engine_reactions.append(reaction);
        }
        style_engine_reactions.clear();
        if (!applicable_style_engine_reactions.is_empty()) {
            sort_style_engine_reactions_for_direct_application(document.style_computer(), applicable_style_engine_reactions);
            auto& counters = document.style_invalidation_counters();
            if (published_reaction_count > 0) {
                ++counters.style_engine_reaction_batch_runs;
                counters.style_engine_reaction_elements += published_reaction_count;
            }
            invalidation |= apply_style_engine_reactions(document, applicable_style_engine_reactions);
        }

        // Exact consequences produced while recomputing become the next transaction in this
        // stabilization epoch. Take it only after consuming the current published answers, since
        // a new transaction retires their scratch.
        if (document.style_computer().style_engine().has_pending_transaction()) {
            auto next_transaction = take_style_engine_transaction(document);
            style_engine_reactions = move(next_transaction.reactions);
        }
        if (style_engine_reactions.is_empty())
            break;
    }

    document.set_has_completed_style_update();
    apply_document_style_invalidation_after_style_change(document, invalidation);
    document.update_animated_style_if_needed();
}

static void apply_targeted_style_invalidation(DOM::Element& element, RequiredInvalidationAfterStyleChange const& invalidation, bool did_change_custom_properties, bool descendant_style_recompute_needed, Optional<StyleNodeID> child_materialized_by_targeted_update)
{
    if (!invalidation.is_none() || did_change_custom_properties)
        Invalidation::invalidate_assigned_slottables_after_slot_style_change(element);
    apply_element_style_invalidation_after_style_change(element, invalidation);

    // NB: This is the inheritance reaction rule applied to one element: what a child reads of its
    //     parent is the inherited half of its style, so a change confined to non-inherited
    //     properties reaches no child. The exceptions are a child that explicitly inherits a
    //     non-inherited property, which is recorded on the element it read it from, and a display
    //     change, which can blockify the children.
    bool const children_may_be_reached = invalidation.inherited_style_changed()
        || invalidation.needs_layout_tree_rebuild()
        || invalidation.recompute_descendant_styles
        || did_change_custom_properties
        || descendant_style_recompute_needed;

    if (children_may_be_reached || (!invalidation.is_none() && element.children_may_depend_on_non_inherited_property_inheritance()))
        record_direct_child_style_engine_inputs(element, child_materialized_by_targeted_update);

    // A shadow tree's children inherit from the host, so they answer the same question against the
    // flag the shadow root carries.
    if (auto shadow_root = element.shadow_root()) {
        if (children_may_be_reached || (!invalidation.is_none() && shadow_root->children_may_depend_on_non_inherited_property_inheritance()))
            record_direct_child_style_engine_inputs(*shadow_root, child_materialized_by_targeted_update);
    }

    apply_document_style_invalidation_after_style_change(element.document(), invalidation);
}

static RequiredInvalidationAfterStyleChange materialize_style_for_targeted_update(DOM::Element& element, bool& did_change_custom_properties)
{
    auto& style_computer = element.document().style_computer();

    if (element.parent()) {
        auto invalidation = element.apply_style_engine_reaction(did_change_custom_properties);
        style_computer.style_engine().consume_recorded_element_style_input_change(element.style_node_id());
        return invalidation;
    }

    StyleEngine::StyleRecordDelta style_record_delta {};
    auto new_style = element.document().style_computer().materialize_style_record({ element }, did_change_custom_properties, nullptr, style_record_delta);
    element.set_computed_style({}, style_record_delta.new_style_record);
    style_computer.style_engine().consume_recorded_element_style_input_change(element.style_node_id());
    return {};
}

static bool update_style_for_element(DOM::Document& document, DOM::AbstractElement const& abstract_element, StyleUpdateMode mode)
{
    StyleValueFFI::rust_style_ffi_complete_style_update_begin();
    ScopeGuard leave_complete_style_update = finish_complete_style_update;
    // Refresh computed properties for an abstract element. An ordinary read first consumes the complete exact
    // reaction batch. A reentrant layout read leaves that transaction untouched and walks the flat-tree inheritance
    // chain, re-cascading from the rootmost stale element on the path back down to the target. Normal mode also
    // re-cascades the target path under display:none ancestors.

    bool embedding_document_layout_was_stale = false;
    if (auto navigable = document.navigable(); navigable && navigable->container() && &navigable->container()->document() != &document) {
        auto& container = *navigable->container();
        auto& embedding_document = container.document();
        update_style_for_element(embedding_document, DOM::AbstractElement { container }, StyleUpdateMode::OnlyIfNeeded);
        embedding_document_layout_was_stale = !embedding_document.layout_is_up_to_date();
        embedding_document.update_layout(DOM::UpdateLayoutReason::ChildDocumentStyleUpdate);
    }

    bool entered_stabilization_epoch = false;
    ScopeGuard end_stabilization_epoch = [&] {
        if (entered_stabilization_epoch)
            document.end_style_stabilization_epoch();
    };

    bool ran_regular_style_update = false;
    if (document.browsing_context()) {
        document.begin_style_stabilization_epoch();
        entered_stabilization_epoch = true;
        document.update_style_computer_viewport_rect();

        if (document.style_computer().style_engine().has_pending_transaction() || document.needs_media_rule_evaluation())
            document.note_style_stabilization_has_style_reactions();

        // Media query evaluation can enqueue normal style invalidations, so do it before deciding what pending
        // invalidation work needs to run.
        if (document.needs_media_rule_evaluation())
            document.evaluate_media_rules_for_style_update();

        auto const can_run_regular_style_update = !document.is_running_update_layout()
            && (!document.has_completed_style_update()
                || document.style_computer().style_engine().has_pending_transaction());
        if (can_run_regular_style_update) {
            update_style(document);
            ran_regular_style_update = true;
        } else {
            document.update_animated_style_if_needed();
            if (!document.is_running_update_layout()
                && document.style_computer().style_engine().has_pending_transaction()) {
                update_style(document);
                ran_regular_style_update = true;
            }
        }
    }

    // Element-backed pseudo-elements read their computed style from the element that backs them (for example,
    // ::details-content reads from the slot inside the details element's UA shadow tree). Close the originating
    // element's transaction before redirecting to the backing element, which can consume feedback from that
    // transaction and is not in the originating element's inheritance chain.
    if (abstract_element.pseudo_element().has_value() && is_element_reference_pseudo_element(*abstract_element.pseudo_element())) {
        if (auto pseudo_element = abstract_element.element().get_pseudo_element(*abstract_element.pseudo_element()); pseudo_element.has_value()) {
            if (auto const* element_reference = as_if<DOM::ElementReferencePseudoElement>(*pseudo_element))
                return update_style_for_element(document, DOM::AbstractElement { element_reference->referenced_element() }, mode);
        }
    }

    if (ran_regular_style_update && mode != StyleUpdateMode::OnlyIfNeeded) {
        auto const computed_values = abstract_element.computed_style();
        if (computed_values && !computed_values->in_display_none_subtree())
            return true;
    }

    // Single walk up the inheritance chain: collect each ancestor and remember the index of the topmost display:none
    // entry seen. Pseudo-element styles are refreshed when the originating element is recomputed, so don't put the pseudo
    // on the path.
    GC::RootVector<GC::Ref<DOM::Element>> inheritance_chain;
    if (!abstract_element.pseudo_element().has_value())
        inheritance_chain.append(const_cast<DOM::Element&>(abstract_element.element()));

    Optional<size_t> topmost_display_none_index;
    Optional<size_t> topmost_element_requiring_style;
    for (auto cursor = abstract_element.element_to_inherit_style_from(); cursor.has_value(); cursor = cursor->element_to_inherit_style_from()) {
        auto& ancestor = const_cast<DOM::Element&>(cursor->element());
        inheritance_chain.append(ancestor);
    }

    for (size_t i = inheritance_chain.size(); i > 0; --i) {
        auto& ancestor = inheritance_chain[i - 1];
        if (!topmost_element_requiring_style.has_value()
            && (document.style_computer().style_engine().has_recorded_element_style_input_change(ancestor->style_node_id())
                || !ancestor->has_style())) {
            topmost_element_requiring_style = i - 1;
        }

        if (auto const values = ancestor->computed_style(); values && values->display().is_none()) {
            topmost_display_none_index = i - 1;
            if (mode == StyleUpdateMode::StopAtDisplayNone && !topmost_element_requiring_style.has_value())
                return false;
        }
    }

    Optional<size_t> topmost_element_to_recompute = topmost_element_requiring_style;
    if (mode == StyleUpdateMode::Normal && topmost_display_none_index.has_value()) {
        if (!topmost_element_to_recompute.has_value() && *topmost_display_none_index > 0)
            topmost_element_to_recompute = *topmost_display_none_index - 1;
    }

    if (!topmost_element_to_recompute.has_value()) {
        if ((mode == StyleUpdateMode::Normal || embedding_document_layout_was_stale) && !inheritance_chain.is_empty())
            topmost_element_to_recompute = 0;
        else
            return abstract_element.has_style();
    }

    bool descendant_style_recompute_needed = false;
    for (size_t i = *topmost_element_to_recompute + 1; i > 0; --i) {
        auto& element = inheritance_chain[i - 1];
        bool did_change_custom_properties = false;
        auto invalidation = materialize_style_for_targeted_update(element, did_change_custom_properties);
        auto child_materialized_by_targeted_update = i > 1
            ? Optional<StyleNodeID> { inheritance_chain[i - 2]->style_node_id() }
            : Optional<StyleNodeID> {};
        apply_targeted_style_invalidation(element, invalidation, did_change_custom_properties, descendant_style_recompute_needed, child_materialized_by_targeted_update);

        descendant_style_recompute_needed |= invalidation.recompute_descendant_styles;

        auto style = element->computed_style();
        VERIFY(style);
        if (style->display().is_none()) {
            if (mode == StyleUpdateMode::StopAtDisplayNone)
                return false;
            descendant_style_recompute_needed = false;
        }

        if (did_change_custom_properties || invalidation.needs_layout_tree_rebuild())
            descendant_style_recompute_needed = true;
    }

    return abstract_element.has_style();
}

}

namespace Web::DOM {

void Document::update_style()
{
    CSS::update_style(*this);
}

bool Document::update_style_for_element(AbstractElement const& abstract_element)
{
    return CSS::update_style_for_element(*this, abstract_element, StyleUpdateMode::Normal);
}

bool Document::update_style_for_element(AbstractElement const& abstract_element, StyleUpdateMode mode)
{
    return CSS::update_style_for_element(*this, abstract_element, mode);
}

}
