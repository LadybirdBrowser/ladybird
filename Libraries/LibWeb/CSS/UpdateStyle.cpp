/*
 * Copyright (c) 2018-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
}

static void update_style(DOM::Document&);
static bool update_style_for_element(DOM::Document&, DOM::AbstractElement const&, StyleUpdateMode);

static void apply_element_style_invalidation_after_style_change(DOM::Element& element, RequiredInvalidationAfterStyleChange const& invalidation)
{
    if (invalidation.accumulated_visual_contexts() == AccumulatedVisualContextInvalidation::UpdateValues)
        element.document().schedule_accumulated_visual_context_update(element, DOM::Document::AccumulatedVisualContextUpdateScope::Values);
    else if (invalidation.accumulated_visual_contexts() == AccumulatedVisualContextInvalidation::Rebuild)
        element.document().schedule_accumulated_visual_context_update(element, DOM::Document::AccumulatedVisualContextUpdateScope::Structure);

    if (invalidation.needs_scrollable_overflow_recalculation())
        element.document().schedule_scrollable_overflow_recalculation(element);

    if (invalidation.needs_scroll_container_resnap)
        element.document().schedule_scroll_container_resnap();

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
    // The transaction continues the style change whose reactions were applied last, one tree
    // generation further, rather than answering new inputs.
    bool only_derived_child_reactions { false };
};

static StyleEngineTransaction take_style_engine_transaction(DOM::Document& document)
{
    StyleEngineTransaction transaction;
    auto& style_computer = document.style_computer();
    // One element's computed style answers for another only while the inputs it was keyed on still
    // mean what they meant. A transaction boundary is exactly where they stop doing so: a
    // declaration keyed on by identity may have been edited, and a sheet may have come or gone.
    auto transaction_setup_started_at = MonotonicTime::now();
    ++document.style_invalidation_counters().style_engine_transaction_setups;
    // The engine resolves custom properties against the registrations in force, and an
    // @property rule registers through this cache.
    document.build_registered_properties_cache_for_style_update();
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
    transaction.only_derived_child_reactions = published_transaction.only_derived_child_reactions;

    return transaction;
}

static StyleEngine::PublishedStyleDelta make_materialize_gap_delta(StyleNodeID style_node, u8 reaction, u8 inherited_style_groups = 0)
{
    return {
        .style_node = style_node.value(),
        .match_answer = 0,
        .old_style_record = 0,
        .new_style_record = 0,
        .damage = StyleEngineFFI::FfiStyleDeltaDamage::None,
        .reaction = reaction,
        .inherited_style_groups = inherited_style_groups,
        .pseudo_kind = NumericLimits<u8>::max(),
        .gap = StyleEngineFFI::FfiStyleDeltaGap::Materialize,
        .uses_substitution = false,
    };
}

// The static inherited-group swap answers a pure inherited-style reaction without recomputing the element. That
// is only sound while the element's computed style is a pure function of its cascade inputs and the swapped
// groups: an element with animations may resolve keyframe values (`inherit`, neutral keyframes) against the
// parent's style, and an element with transitions or transition-property entries must compare before-change and
// after-change styles at every style change event. These are the conditions under which
// Element::apply_style_engine_reaction declines its own inherited-style group swap.
static bool element_style_depends_on_more_than_the_inherited_groups(DOM::Element& element)
{
    if (element.has_relevant_animations()
        || element.has_css_defined_animations()
        || !element.property_ids_with_existing_transitions({}).is_empty()
        || !element.property_ids_with_matching_transition_property_entry({}).is_empty())
        return true;
    // The swapped groups are the parent's base values; a child of an animating parent inherits
    // the animated ones, which the engine never sees.
    if (auto parent = DOM::AbstractElement { element }.element_to_inherit_style_from(); parent.has_value()) {
        if (auto parent_style = parent->computed_style(); parent_style && parent_style->has_animated_values())
            return true;
    }
    return false;
}

// Whether the custom-property environment an engine-computed record was published with can be
// installed: the one the element inherits - the parent's inheritable data, which is the parent's
// own unless a registration made some of it non-inherited - or one the engine resolved over it.
static bool engine_computed_record_environment_is_installable(DOM::Element& element, StyleRecordID style_record)
{
    bool installable = false;
    (void)element.custom_property_environment_of_engine_record(style_record, installable);
    return installable;
}

// Under verification, the environment the engine resolved for a record must hold, name for name,
// what the C++ computation installed on the element.
static void verify_engine_computed_record_environment(DOM::Element& element, StyleRecordID style_record)
{
    auto& style_computer = element.document().style_computer();
    auto identity = style_computer.style_engine().style_record_custom_property_environment(style_record);
    if (!StyleEngine::is_engine_custom_property_environment(identity))
        return;
    auto actual = element.custom_property_data({});
    if (actual && actual->is_animation_overlay())
        actual = actual->parent();
    bool installable = false;
    auto expected = element.custom_property_environment_of_engine_record(style_record, installable);
    VERIFY(installable && expected);
    auto value_text = [](StyleProperty const* property) -> Optional<Utf16String> {
        if (!property)
            return {};
        return property->value->to_utf16_string(SerializationMode::Normal);
    };
    auto check = [&](Utf16FlyString const& name) {
        auto const* expected_property = expected->get(name);
        auto const* actual_property = actual ? actual->get(name) : nullptr;
        VERIFY(value_text(expected_property) == value_text(actual_property));
        VERIFY(!expected_property || !actual_property || expected_property->important == actual_property->important);
    };
    expected->for_each_property([&](Utf16FlyString const& name, StyleProperty const&) { check(name); });
    if (actual)
        actual->for_each_property([&](Utf16FlyString const& name, StyleProperty const&) { check(name); });
}

static RequiredInvalidationAfterStyleChange apply_style_engine_reactions(DOM::Document& document, Vector<StyleEngine::PublishedStyleDelta> const& reactions)
{
    // Reactions are applied in preorder, so every element's inheritance inputs are ready when it is
    // applied. What an applied element's change means for its (flat-tree) children is the
    // engine's to derive: it reads each application and plans the children as the next
    // transaction of this style update.
    RequiredInvalidationAfterStyleChange transaction_invalidation;
    {
        for (size_t reaction_index = 0; reaction_index < reactions.size(); ++reaction_index) {
            auto const& published_reaction = reactions[reaction_index];
            // A pseudo-element record installs with its element's, which leads it.
            if (published_reaction.pseudo_kind != NumericLimits<u8>::max())
                continue;
            auto element = document.style_computer().element_for_style_node(published_reaction.style_node);
            if (!element)
                continue;
            // A reaction the engine derived for this element while applying an earlier one in
            // this batch joins the element's own reaction where it covers it.
            auto reaction = published_reaction;
            if (auto absorbed = document.style_computer().style_engine().absorb_element_style_input(
                    StyleNodeID { reaction.style_node }, reaction.reaction, reaction.inherited_style_groups,
                    reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::Materialize);
                absorbed != 0) {
                reaction.reaction = static_cast<u8>(absorbed & 0xff);
                reaction.inherited_style_groups = static_cast<u8>(absorbed >> 8);
            }

            if (reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::RetryAfterAncestor) {
                if (auto retried = element->namespace_uri() == Namespace::HTML
                        ? document.style_computer().style_engine().retry_engine_record_after_ancestor(reaction.style_node)
                        : StyleEngineFFI::FfiEngineComputedRecord {};
                    retried.style_record != 0) {
                    reaction.new_style_record = retried.style_record;
                    reaction.uses_substitution = retried.uses_substitution;
                    reaction.damage = StyleEngineFFI::FfiStyleDeltaDamage::Full;
                    reaction.gap = StyleEngineFFI::FfiStyleDeltaGap::Computed;
                } else {
                    reaction.gap = StyleEngineFFI::FfiStyleDeltaGap::Materialize;
                }
            }

            // An engine-computed first record installs on an element without style; other record
            // deltas assume the style they move.
            if (!element->has_style()
                && reaction.gap != StyleEngineFFI::FfiStyleDeltaGap::Materialize
                && !(reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::Computed && reaction.old_style_record == 0))
                continue;
            // An earlier display:none reaction in this batch can clear the style of a materialization gap after the
            // inheritance closure was built. The gap must then rematerialize rather than letting its descendants
            // compute against a missing inheritance parent.
            if (reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::Materialize && reaction.reaction == 0 && !element->has_style())
                reaction.reaction = StyleEngine::RecomputeStyle;

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
                // An engine-computed record is the engine's current answer for the element, which
                // may have skipped a delta C++ never installed; it is applied against whatever the
                // element holds now.
                VERIFY(reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::Computed || reaction.old_style_record == element->style_record_identity().value());
                VERIFY(reaction.new_style_record != 0);
                VERIFY(reaction.damage == StyleEngineFFI::FfiStyleDeltaDamage::Full);
            }

            auto previous_style_record = element->style_record_identity();
            bool const was_unstyled = !previous_style_record;
            auto const* previous_box_values = element->style_group<ComputedValues::BoxValues>();
            auto const previous_display = previous_box_values
                ? Optional<Display> { display_from_ffi_display(previous_box_values->display) }
                : Optional<Display> {};
            bool const was_display_none = previous_display.has_value() && previous_display->is_none();
            auto const* previous_inherited_box_values = element->style_group<ComputedValues::InheritedBoxValues>();
            auto const previous_visibility = previous_inherited_box_values
                ? Optional<Visibility> { static_cast<Visibility>(previous_inherited_box_values->visibility) }
                : Optional<Visibility> {};
            bool const needs_regular_style_recompute = reaction.reaction & (StyleEngine::PublishedStyle | StyleEngine::RecomputeStyle | StyleEngine::RecomputeDescendantStyles | StyleEngine::AncestorBecameVisible);
            bool const needs_custom_property_recompute = reaction.reaction & StyleEngine::InheritedCustomProperties;
            bool const needs_inherited_style_recompute = reaction.reaction & StyleEngine::InheritedStyle;
            bool did_change_custom_properties = false;
            RequiredInvalidationAfterStyleChange invalidation;

            // An element declaring custom properties of its own layers them over the environment it
            // inherits, which its cascade decides.
            bool const cascade_declares_custom_properties = document.style_computer().style_engine().node_declares_custom_properties(reaction.style_node);
            bool const needs_full_custom_property_recompute = needs_custom_property_recompute && (element->style_uses_var_css_function() || element->style_uses_inherit_css_function() || cascade_declares_custom_properties);
            static bool const verify_engine_computed_records = getenv("LIBWEB_VERIFY_STYLE_RECORD_PATCH") != nullptr;
            // The engine settled the element's record, and the pseudo-element records beside it:
            // C++ installs them. Under verification the ordinary computation runs instead and its
            // records must equal the engine's by value.
            auto apply_engine_computed_records = [&](DOM::Element::EnginePseudoElementRecords const& pseudo_element_records, bool acknowledge) {
                auto& style_engine = document.style_computer().style_engine();
                if (verify_engine_computed_records) {
                    auto authoritative_custom_property_data = element->custom_property_data({});
                    if (authoritative_custom_property_data && authoritative_custom_property_data->is_animation_overlay())
                        authoritative_custom_property_data = authoritative_custom_property_data->parent();
                    auto const authoritative_custom_property_environment = authoritative_custom_property_data
                        ? authoritative_custom_property_data->identity()
                        : 0;
                    auto const production_packed = !!previous_style_record
                        ? style_engine.compare_style_records(StyleRecordID { reaction.new_style_record }, previous_style_record, true, false)
                        : to_underlying(StyleEngineFFI::FfiStyleInvalidationField::AnyComputedValueChanged);
                    auto& counters = document.style_invalidation_counters();
                    auto const counters_before_verification = counters;
                    style_engine.begin_computed_record_verification();
                    ScopeGuard end_computed_record_verification = [&] { style_engine.end_computed_record_verification(); };
                    DOM::Element::EnginePseudoElementRecords previous_pseudo_element_records;
                    for (size_t kind = 0; kind < previous_pseudo_element_records.size(); ++kind)
                        previous_pseudo_element_records[kind] = element->style_record_identity(static_cast<PseudoElement>(kind));
                    bool const production_computed_value_changed = production_packed & to_underlying(StyleEngineFFI::FfiStyleInvalidationField::AnyComputedValueChanged)
                        && !style_engine.style_records_match_for_verification(reaction.style_node, NumericLimits<u8>::max(), StyleRecordID { reaction.new_style_record }, previous_style_record);
                    style_engine.consume_recorded_element_style_input_change(reaction.style_node);
                    bool verification_did_change_custom_properties = false;
                    invalidation = element->apply_style_engine_reaction(verification_did_change_custom_properties, DOM::Element::StyleRecomputeMode::Verification);
                    bool verification_pseudo_record_changed = false;
                    for (size_t kind = 0; kind < previous_pseudo_element_records.size(); ++kind) {
                        if (element->style_record_identity(static_cast<PseudoElement>(kind)) != previous_pseudo_element_records[kind]) {
                            verification_pseudo_record_changed = true;
                            break;
                        }
                    }
                    auto packed = style_engine.compare_style_records(StyleRecordID { reaction.new_style_record }, element->style_record_identity(), true, false);
                    VERIFY(!(packed & to_underlying(StyleEngineFFI::FfiStyleInvalidationField::AnyComputedValueChanged))
                        || style_engine.style_records_match_for_verification(reaction.style_node, NumericLimits<u8>::max(), StyleRecordID { reaction.new_style_record }, element->style_record_identity()));
                    // A custom-property environment reaction can jump over ancestors whose computed
                    // values did not change. Their engine environments are authoritative, but the
                    // legacy verification pass never materialized them, so it has no independent
                    // environment to compare here. The computed record, including every var()
                    // substitution, is still verified above.
                    bool const legacy_environment_is_complete = !needs_custom_property_recompute;
                    if (legacy_environment_is_complete)
                        verify_engine_computed_record_environment(*element, StyleRecordID { reaction.new_style_record });
                    for (size_t kind = 0; kind < pseudo_element_records.size(); ++kind) {
                        auto const& engine_record = pseudo_element_records[kind];
                        if (!engine_record.has_value())
                            continue;
                        auto installed = element->style_record_identity(static_cast<PseudoElement>(kind));
                        if (!*engine_record) {
                            VERIFY(!installed);
                            continue;
                        }
                        VERIFY(!!installed);
                        auto pseudo_packed = style_engine.compare_style_records(*engine_record, installed, true, false);
                        VERIFY(!(pseudo_packed & to_underlying(StyleEngineFFI::FfiStyleInvalidationField::AnyComputedValueChanged))
                            || style_engine.style_records_match_for_verification(reaction.style_node, kind, *engine_record, installed));
                    }
                    // A skipped ancestor can make the engine record temporarily unattachable only
                    // because the verification pass installed a legacy environment on that
                    // ancestor. Keep the old authoritative record in that case; a production pass
                    // never creates this mixed environment chain.
                    bool const engine_record_is_installable = engine_computed_record_environment_is_installable(*element, StyleRecordID { reaction.new_style_record });
                    auto const verification_invalidation = invalidation;
                    counters = counters_before_verification;
                    auto const computed_style_changes_before_application = counters.element_computed_style_changes;
                    if (engine_record_is_installable) {
                        invalidation = element->apply_engine_computed_style_record(StyleRecordID { reaction.new_style_record }, pseudo_element_records, reaction.uses_substitution, did_change_custom_properties);
                        // The reference pass already installed equal values, so applying the engine
                        // record may be a no-op. Preserve the invalidation it proved the originating
                        // record or pseudo-element transitions need.
                        if (verification_pseudo_record_changed)
                            invalidation |= verification_invalidation;
                        else if (invalidation.is_none() && !!previous_style_record && production_computed_value_changed)
                            invalidation = verification_invalidation;
                        if (production_computed_value_changed
                            && counters.element_computed_style_changes == computed_style_changes_before_application)
                            ++counters.element_computed_style_changes;
                    }
                    // The legacy pass may already have installed the target environment, masking
                    // the change that the production application would report to child planning.
                    did_change_custom_properties |= authoritative_custom_property_environment
                        != style_engine.style_record_custom_property_environment(StyleRecordID { reaction.new_style_record });
                    // The reference computation recorded the current C++ inputs against its
                    // temporary interned record. The authoritative record is equal by value, so
                    // bind those inputs to it before the temporary verification pin is released.
                    if (auto* style_input_record = element->style_input_record()) {
                        style_input_record->computed_style_record = engine_record_is_installable
                            ? StyleRecordID { reaction.new_style_record }
                            : element->style_record_identity();
                        style_input_record->bind_next_published_style = false;
                    }
                    for (size_t kind = 0; kind < previous_pseudo_element_records.size(); ++kind) {
                        auto pseudo_element = static_cast<PseudoElement>(kind);
                        if (is_synthetic_pseudo_element(pseudo_element)
                            && pseudo_element != PseudoElement::Backdrop
                            && !pseudo_element_records[kind].has_value())
                            element->set_computed_style(pseudo_element, *previous_pseudo_element_records[kind]);
                    }
                } else {
                    // A first record answers the element's recorded arrival; nothing is left for a
                    // later transaction to plan.
                    if (!element->has_style())
                        style_engine.consume_recorded_element_style_input_change(reaction.style_node);
                    invalidation = element->apply_engine_computed_style_record(StyleRecordID { reaction.new_style_record }, pseudo_element_records, reaction.uses_substitution, did_change_custom_properties);
                }
                if (acknowledge)
                    style_engine.acknowledge_engine_computed_record(StyleNodeID { reaction.style_node });
            };
            if (reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::None) {
                VERIFY(!needs_regular_style_recompute);
                VERIFY(needs_inherited_style_recompute);
                VERIFY(!needs_custom_property_recompute);
                VERIFY(reaction.pseudo_kind == NumericLimits<u8>::max());
                // The engine swapped the element's inherited groups for its parent's: the record
                // installs as an engine record.
                if (element_style_depends_on_more_than_the_inherited_groups(*element))
                    invalidation = element->apply_style_engine_reaction(did_change_custom_properties);
                else
                    apply_engine_computed_records({}, false);
            } else if (reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::Computed) {
                // The engine computed the new record from this element's moved cascade winners,
                // from its parent's moved inherited style or display, or from its moved
                // inherited custom-property environment.
                VERIFY(needs_regular_style_recompute || needs_inherited_style_recompute || needs_custom_property_recompute);
                VERIFY(reaction.pseudo_kind == NumericLimits<u8>::max());
                DOM::Element::EnginePseudoElementRecords pseudo_element_records {};
                for (auto next = reaction_index + 1; next < reactions.size() && reactions[next].style_node == published_reaction.style_node && reactions[next].pseudo_kind != NumericLimits<u8>::max(); ++next)
                    pseudo_element_records[reactions[next].pseudo_kind] = StyleRecordID { reactions[next].new_style_record };
                if (!engine_computed_record_environment_is_installable(*element, StyleRecordID { reaction.new_style_record })) {
                    // The engine resolved the record's environment over the parent's own; when the
                    // parent's inheritable environment differs, C++ computes the style.
                    document.style_computer().style_engine().consume_recorded_element_style_input_change(reaction.style_node);
                    invalidation = element->apply_style_engine_reaction(did_change_custom_properties);
                } else {
                    apply_engine_computed_records(pseudo_element_records, true);
                }
            } else if (needs_regular_style_recompute || needs_inherited_style_recompute || needs_full_custom_property_recompute) {
                if (needs_regular_style_recompute)
                    document.style_computer().style_engine().consume_recorded_element_style_input_change(reaction.style_node);
                invalidation = element->apply_style_engine_reaction(did_change_custom_properties);
            } else if (needs_custom_property_recompute && element->refresh_inherited_custom_property_data()) {
                did_change_custom_properties = true;
                element->republish_style_record_environment();
                element->invalidate_descendant_styles_depending_on_style_container_query();
            }

            auto const* current_inherited_box_values = element->style_group<ComputedValues::InheritedBoxValues>();
            if (previous_visibility.has_value() && current_inherited_box_values
                && *previous_visibility != static_cast<Visibility>(current_inherited_box_values->visibility)) {
                document.throttled_animation_visibility_changed();
            }

            apply_element_style_invalidation_after_style_change(*element, invalidation);
            transaction_invalidation |= invalidation;

            auto& style_engine = document.style_computer().style_engine();
            auto current_style_record = element->style_record_identity();
            u32 facts = 0;
            if (did_change_custom_properties)
                facts |= StyleEngine::DidChangeCustomProperties;
            if (invalidation.is_none())
                facts |= StyleEngine::InvalidationIsNone;
            if (invalidation.needs_layout_tree_rebuild())
                facts |= StyleEngine::NeedsLayoutTreeRebuild;
            if (invalidation.recompute_descendant_styles)
                facts |= StyleEngine::RecomputeDescendants;
            if (element->children_explicitly_inherited_non_inherited_style_groups() != 0)
                facts |= StyleEngine::ChildrenExplicitlyInherit;
            if (auto shadow_root = element->shadow_root(); shadow_root && shadow_root->children_explicitly_inherited_non_inherited_style_groups() != 0)
                facts |= StyleEngine::ShadowChildrenExplicitlyInherit;
            if (was_unstyled)
                facts |= StyleEngine::WasUnstyled;
            if (was_display_none)
                facts |= StyleEngine::WasDisplayNone;
            // A descendant whose style was cleared on entry to display:none can receive a reaction which only
            // updates style-engine bookkeeping, such as a synthetic pseudo-element reaction. Keep the DOM style
            // unmaterialized until a CSSOM read or the ancestor becomes visible.
            if (!!current_style_record) {
                facts |= StyleEngine::HasStyle;
                auto const* current_box_values = element->style_group<ComputedValues::BoxValues>();
                VERIFY(current_box_values);
                if (display_from_ffi_display(current_box_values->display).is_none())
                    facts |= StyleEngine::IsDisplayNone;
                if (previous_display.has_value() && *previous_display != display_from_ffi_display(current_box_values->display))
                    facts |= StyleEngine::DisplayChanged;
                if (style_engine.style_record_dependency_flags(current_style_record) & to_underlying(StyleRecordDependencyFlag::InDisplayNoneSubtree))
                    facts |= StyleEngine::InDisplayNoneSubtree;
            } else {
                VERIFY(was_unstyled);
            }
            style_engine.note_style_reaction_applied(reaction.style_node, reaction.reaction, invalidation.inherited_style_groups_changed(), facts);
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

    document.style_computer().begin_style_update();
    ScopeGuard end_style_update = [&] {
        document.style_computer().end_style_update();
    };

    document.style_computer().begin_style_record_view_epoch();
    ScopeGuard end_style_record_view_epoch = [&] {
        document.style_computer().end_style_record_view_epoch();
    };

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
        document.sample_animation_effects_needing_style_update();
        if (!document.style_computer().style_engine().has_pending_transaction())
            return;
    }

    // A style flush is a transaction boundary. Everything recorded since the last one crosses into
    // StyleEngine as one flat batch, is normalized there, and is routed into the region its
    // transpose programs reach. A transaction that could not be proven narrower publishes a
    // complete document reaction batch. Only a transaction that cannot complete its answers falls
    // back to document invalidation.
    auto style_engine_transaction = take_style_engine_transaction(document);
    ScopeGuard discard_style_engine_transaction_outputs = [&] {
        document.style_computer().style_engine().discard_style_transaction_outputs();
    };

    if (!style_engine_transaction.reactions.is_empty())
        document.note_style_stabilization_has_style_reactions();
    document.sample_animation_effects_needing_style_update();

    auto style_engine_reactions = move(style_engine_transaction.reactions);
    auto prefers_broad_matching_batch = style_engine_transaction.prefers_broad_matching_batch;
    auto transaction_only_derived_child_reactions = style_engine_transaction.only_derived_child_reactions;
    if (style_engine_reactions.is_empty()
        && document.style_computer().style_engine().has_pending_transaction()) {
        auto feedback_transaction = take_style_engine_transaction(document);
        style_engine_reactions = move(feedback_transaction.reactions);
        prefers_broad_matching_batch = feedback_transaction.prefers_broad_matching_batch;
        transaction_only_derived_child_reactions = feedback_transaction.only_derived_child_reactions;
    }

    document.build_registered_properties_cache_for_style_update();

    // This pass belongs to the current style change event. The outer stabilization epoch advanced
    // the transition generation once, before any style, animation, or layout feedback ran.
    document.record_style_stabilization_pass();

    if (style_engine_reactions.is_empty())
        return;

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
        // One more tree generation of the same style change is not a new pass of it.
        if (style_reaction_pass++ > 0 && !transaction_only_derived_child_reactions)
            document.record_style_stabilization_pass();

        size_t published_reaction_count = 0;
        for (auto const& reaction : style_engine_reactions) {
            if (reaction.reaction & StyleEngine::PublishedStyle)
                ++published_reaction_count;
        }
        if (published_reaction_count > 0 && !transaction_only_derived_child_reactions) {
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
                    style_engine_reactions.append(make_materialize_gap_delta(prerequisite, StyleEngine::RecomputeStyle));
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
                            style_engine_reactions.append(make_materialize_gap_delta(style_node, 0));
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
            // Apply each inheritance branch contiguously in preorder. Besides making every parent
            // ready before its descendants, this lets a parent's derived reaction merge into an
            // unconsumed child reaction in the same batch.
            document.style_computer().style_engine().sort_style_deltas_for_direct_application(applicable_style_engine_reactions);
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
            transaction_only_derived_child_reactions = next_transaction.only_derived_child_reactions;
        }
        if (style_engine_reactions.is_empty())
            break;
    }

    document.set_has_completed_style_update();
    apply_document_style_invalidation_after_style_change(document, invalidation);
    document.sample_animation_effects_needing_style_update();
}

// What a targeted materialization of one element found, reported to the engine the way a reaction
// pass reports it, so the element's children get the same derived reactions either way.
static void note_targeted_style_reaction_applied(DOM::Element& element, RequiredInvalidationAfterStyleChange const& invalidation, bool did_change_custom_properties, bool descendant_style_recompute_needed, bool was_unstyled, bool was_display_none, bool display_changed)
{
    auto& style_engine = element.document().style_computer().style_engine();
    u8 reaction = StyleEngine::PublishedStyle | StyleEngine::RecomputeStyle;
    if (descendant_style_recompute_needed)
        reaction |= StyleEngine::RecomputeDescendantStyles;
    u32 facts = 0;
    if (did_change_custom_properties)
        facts |= StyleEngine::DidChangeCustomProperties;
    if (invalidation.is_none())
        facts |= StyleEngine::InvalidationIsNone;
    if (invalidation.needs_layout_tree_rebuild())
        facts |= StyleEngine::NeedsLayoutTreeRebuild;
    if (invalidation.recompute_descendant_styles)
        facts |= StyleEngine::RecomputeDescendants;
    if (element.children_explicitly_inherited_non_inherited_style_groups() != 0)
        facts |= StyleEngine::ChildrenExplicitlyInherit;
    if (auto shadow_root = element.shadow_root(); shadow_root && shadow_root->children_explicitly_inherited_non_inherited_style_groups() != 0)
        facts |= StyleEngine::ShadowChildrenExplicitlyInherit;
    if (was_unstyled)
        facts |= StyleEngine::WasUnstyled;
    if (was_display_none)
        facts |= StyleEngine::WasDisplayNone;
    if (display_changed)
        facts |= StyleEngine::DisplayChanged;
    auto style_record = element.style_record_identity();
    if (!!style_record) {
        facts |= StyleEngine::HasStyle;
        auto const* box_values = element.style_group<ComputedValues::BoxValues>();
        VERIFY(box_values);
        if (display_from_ffi_display(box_values->display).is_none())
            facts |= StyleEngine::IsDisplayNone;
        if (style_engine.style_record_dependency_flags(style_record) & to_underlying(StyleRecordDependencyFlag::InDisplayNoneSubtree))
            facts |= StyleEngine::InDisplayNoneSubtree;
    }
    style_engine.note_style_reaction_applied(element.style_node_id(), reaction, invalidation.inherited_style_groups_changed(), facts);
}

static void apply_targeted_style_invalidation(DOM::Element& element, RequiredInvalidationAfterStyleChange const& invalidation, bool did_change_custom_properties, bool descendant_style_recompute_needed, bool was_unstyled, bool was_display_none, bool display_changed)
{
    if (!invalidation.is_none() || did_change_custom_properties)
        Invalidation::invalidate_assigned_slottables_after_slot_style_change(element);
    apply_element_style_invalidation_after_style_change(element, invalidation);
    note_targeted_style_reaction_applied(element, invalidation, did_change_custom_properties, descendant_style_recompute_needed, was_unstyled, was_display_none, display_changed);
    apply_document_style_invalidation_after_style_change(element.document(), invalidation);
}

static RequiredInvalidationAfterStyleChange materialize_style_for_targeted_update(DOM::Element& element, bool& did_change_custom_properties)
{
    auto& style_computer = element.document().style_computer();

    style_computer.style_engine().consume_recorded_element_style_input_change(element.style_node_id());

    if (element.parent())
        return element.apply_style_engine_reaction(did_change_custom_properties);

    StyleEngine::StyleRecordDelta style_record_delta {};
    auto new_style = element.document().style_computer().materialize_style_record({ element }, did_change_custom_properties, nullptr, style_record_delta);
    element.set_computed_style({}, style_record_delta.new_style_record);
    return {};
}

// A targeted style update has nothing to do when every source of style work in the document is settled: A full style
// update has completed, no style engine transaction or recorded input is pending, no media rule evaluation is queued,
// and no animated style refresh is due.
static bool document_has_no_pending_style_work(DOM::Document const& document)
{
    // A rootless flush drains the journal but preserves element style inputs for the first transaction with a document
    // root — so has_pending_transaction() alone would report a settled engine still owing an element its recomputation.
    return document.has_completed_style_update()
        && !document.style_computer().style_engine().has_pending_transaction()
        && !document.style_computer().style_engine().has_deferred_element_style_inputs()
        && !document.needs_media_rule_evaluation()
        && !document.needs_animated_style_update();
}

// Whether every embedding document up the container chain needs no style or layout work — so, bringing the embedding
// chain up to date couldn't invalidate anything in this document.
static bool embedding_document_chain_has_no_pending_style_or_layout_work(DOM::Document const& document)
{
    auto const* embedded_document = &document;
    while (auto navigable = embedded_document->navigable()) {
        auto container = navigable->container();
        if (!container || &container->document() == embedded_document)
            return true;
        auto& embedding_document = container->document();
        if (!document_has_no_pending_style_work(embedding_document)
            || !embedding_document.layout_is_up_to_date()
            || !container->has_style())
            return false;
        embedded_document = &embedding_document;
    }
    return true;
}

static bool update_style_for_element(DOM::Document& document, DOM::AbstractElement const& abstract_element, StyleUpdateMode mode)
{
    if (!abstract_element.element().is_connected())
        return false;

    // OPTIMIZATION: When nothing style-related is pending anywhere that could affect this document, the only question
    // left is, if the element's inheritance chain already has style. If it does, the walk below would conclude there's
    // nothing to recompute. So answer that directly — without constructing a style record view for every ancestor.
    if (mode == StyleUpdateMode::OnlyIfNeeded
        && !abstract_element.pseudo_element().has_value()
        && document_has_no_pending_style_work(document)
        && embedding_document_chain_has_no_pending_style_or_layout_work(document)) {
        bool inheritance_chain_has_style = abstract_element.element().has_style();
        for (auto cursor = abstract_element.element_to_inherit_style_from(); inheritance_chain_has_style && cursor.has_value(); cursor = cursor->element_to_inherit_style_from())
            inheritance_chain_has_style = cursor->element().has_style();
        if (inheritance_chain_has_style)
            return true;
    }

    document.style_computer().begin_style_update();
    ScopeGuard end_style_update = [&] {
        document.style_computer().end_style_update();
    };

    document.style_computer().begin_style_record_view_epoch();
    ScopeGuard end_style_record_view_epoch = [&] {
        document.style_computer().end_style_record_view_epoch();
    };

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
            document.sample_animation_effects_needing_style_update();
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
        auto style_record = abstract_element.style_record_identity();
        if (!!style_record
            && !(document.style_computer().style_engine().style_record_dependency_flags(style_record) & to_underlying(StyleRecordDependencyFlag::InDisplayNoneSubtree)))
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
                || document.style_computer().style_engine().has_deferred_element_style_input(ancestor->style_node_id())
                || !ancestor->has_style())) {
            topmost_element_requiring_style = i - 1;
        }

        auto const* box_values = ancestor->style_group<ComputedValues::BoxValues>();
        if (box_values && display_from_ffi_display(box_values->display).is_none()) {
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
        bool const was_unstyled = !element->has_style();
        auto const* previous_box_values = element->style_group<ComputedValues::BoxValues>();
        bool const was_display_none = previous_box_values && display_from_ffi_display(previous_box_values->display).is_none();
        auto const previous_display = previous_box_values ? Optional<Display> { display_from_ffi_display(previous_box_values->display) } : Optional<Display> {};
        auto invalidation = materialize_style_for_targeted_update(element, did_change_custom_properties);
        if (document.style_computer().style_engine().has_recorded_element_style_input_change(element->style_node_id())) {
            bool changed_custom_properties_again = false;
            invalidation |= materialize_style_for_targeted_update(element, changed_custom_properties_again);
            did_change_custom_properties |= changed_custom_properties_again;
        }
        auto const* current_box_values = element->style_group<ComputedValues::BoxValues>();
        bool const display_changed = previous_display.has_value() && current_box_values && *previous_display != display_from_ffi_display(current_box_values->display);
        apply_targeted_style_invalidation(element, invalidation, did_change_custom_properties, descendant_style_recompute_needed, was_unstyled, was_display_none, display_changed);

        descendant_style_recompute_needed |= invalidation.recompute_descendant_styles;

        VERIFY(element->has_style());
        auto const* box_values = element->style_group<ComputedValues::BoxValues>();
        VERIFY(box_values);
        if (display_from_ffi_display(box_values->display).is_none()) {
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
    flush_throttled_animation_style_update_for_node(abstract_element.element());
    return CSS::update_style_for_element(*this, abstract_element, StyleUpdateMode::Normal);
}

bool Document::update_style_for_element(AbstractElement const& abstract_element, StyleUpdateMode mode)
{
    flush_throttled_animation_style_update_for_node(abstract_element.element());
    return CSS::update_style_for_element(*this, abstract_element, mode);
}

}
