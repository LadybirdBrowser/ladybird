/*
 * Copyright (c) 2018-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <AK/ScopeGuard.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/Invalidation/SlotInvalidator.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/PseudoElement.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::CSS {

using StyleUpdateMode = DOM::Document::StyleUpdateMode;

extern "C" void ladybird_utf16_fly_string_unref(size_t);

static void finish_complete_style_update()
{
    auto releases = StyleValueFFI::rust_style_ffi_complete_style_update_end();
    ScopeGuard clear_releases = StyleValueFFI::rust_deferred_cpp_releases_clear;
    for (size_t i = 0; i < releases.fly_string_count; ++i)
        ladybird_utf16_fly_string_unref(releases.fly_strings[i]);
}

enum class DocumentWithoutBrowsingContext {
    Skip,
    Update,
};
// Where a style update's first transaction comes from.
enum class FirstStyleTransaction : u8 {
    // What was recorded since the last transaction.
    Recorded,
    // The transaction that flew beside the event loop, whose reactions are drained against the inputs it was sealed with.
    Flown,
};

template<FirstStyleTransaction = FirstStyleTransaction::Recorded>
static void update_style(Layout::BegunRead const&, DOM::Document&, DocumentWithoutBrowsingContext = DocumentWithoutBrowsingContext::Skip);
static bool update_style_for_element(Layout::BegunRead const&, DOM::Document&, DOM::AbstractElement const&, StyleUpdateMode);
static bool embedding_document_chain_has_no_pending_style_or_layout_work(DOM::Document const&);

static void apply_element_style_invalidation_after_style_change(Layout::BegunRead const& read, DOM::Element& element, RequiredInvalidationAfterStyleChange const& invalidation)
{
    if (invalidation.accumulated_visual_contexts() == AccumulatedVisualContextInvalidation::UpdateValues)
        element.document().schedule_accumulated_visual_context_update(read, element, DOM::Document::AccumulatedVisualContextUpdateScope::Values);
    else if (invalidation.accumulated_visual_contexts() == AccumulatedVisualContextInvalidation::Rebuild)
        element.document().schedule_accumulated_visual_context_update(read, element, DOM::Document::AccumulatedVisualContextUpdateScope::Structure);

    if (invalidation.needs_scroll_container_resnap)
        element.document().schedule_scroll_container_resnap();

    // A frame applied the element's record to its box ahead of this install, and marked the relayout the move asks
    // for, which the frame's layout round, or the host's next one, runs.
    bool const needs_relayout = invalidation.needs_relayout()
        && !element.document().style_computer().style_engine().frame_marked_relayout(element.style_node_id(), element.style_record_identity());

    // Only a full layout pass applies viewport propagation again, so a relayout of an element the viewport takes its
    // overflow, writing mode, or direction from must not finish as a partial relayout of that element.
    bool const element_is_viewport_propagation_source = element.is_viewport_propagation_source();
    if (needs_relayout && element_is_viewport_propagation_source)
        element.document().record_partial_relayout_escape(DOM::PartialRelayoutEscapeReason::ViewportPropagationSourceChangedByStyleChange);

    if (needs_relayout) {
        // A relayout-only style change on an absolutely positioned partial relayout boundary
        // stays confined to it: the box contributes nothing to ancestor layout, and partial
        // relayout re-resolves the boundary's own size and position. A rendered ::backdrop
        // disqualifies the element, because pseudo-element style diffs are merged into the
        // element's invalidation while the ::backdrop box is a sibling of the element's box,
        // outside the subtree a boundary-self relayout covers.
        auto* box = as_if<Layout::Box>(element.unsafe_layout_node(read));
        if (!invalidation.needs_layout_tree_rebuild()
            && !element_is_viewport_propagation_source
            && box
            && box->is_absolutely_positioned()
            && box->is_partial_relayout_boundary()
            && !element.pseudo_element_unsafe_layout_node(read, CSS::PseudoElement::Backdrop)) {
            box->set_needs_own_geometry_update();
            element.set_needs_layout_update(DOM::SetNeedsLayoutReason::StyleChange, Layout::LayoutUpdatePropagation::BoundarySelfOnly);
        } else {
            element.set_needs_layout_update(DOM::SetNeedsLayoutReason::StyleChange);
        }
    }
    if (invalidation.needs_layout_tree_rebuild())
        element.set_needs_layout_tree_rebuild(read, DOM::SetNeedsLayoutTreeUpdateReason::StyleChange, invalidation.layout_tree_rebuild_root());
}

static void apply_document_style_invalidation_after_style_change(DOM::Document& document, RequiredInvalidationAfterStyleChange const& invalidation)
{
    if (!invalidation.needs_repaint())
        return;
    if (invalidation.invalidates_hit_test_display_list())
        document.set_needs_to_record_display_list();
    else
        document.set_needs_to_record_display_list_keeping_hit_test_display_list();
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

static StyleEngineTransaction accept_style_engine_transaction(DOM::Document& document, StyleEngine::PublishedStyleTransaction const& published_transaction)
{
    StyleEngineTransaction transaction;
    auto& style_computer = document.style_computer();
    document.style_invalidation_counters().style_update_submission_microseconds += published_transaction.submission_microseconds;
    document.style_invalidation_counters().style_update_bridge_microseconds += published_transaction.bridge_microseconds;
    if (!published_transaction.reactions.is_empty())
        transaction.published_version = published_transaction.version;
    for (auto const& answer : published_transaction.reactions) {
        // The complete answer remains in Rust transaction scratch under this node. The identity
        // names both the semantic reaction and the payload that consumes it.
        if (!style_computer.element_for_style_node(answer.style_node)) {
            // NB: The element was removed beside the transaction that flew. Its removal is the next transaction's, and
            //     it has no style to install.
            VERIFY(style_computer.style_engine().style_node_arrived_or_retired_beside_flown_transaction(StyleNodeID { answer.style_node }));
            continue;
        }
        transaction.reactions.append(answer);
    }

    // A reaction batch covering more than one sixteenth of the connected elements is dense enough that
    // packing the scope once is cheaper than repeatedly reconstructing cold facts while matching
    // the planned elements.
    transaction.prefers_broad_matching_batch = !published_transaction.is_scoped
        || transaction.reactions.size() * 16 > published_transaction.connected_element_count;
    transaction.only_derived_child_reactions = published_transaction.only_derived_child_reactions;

    return transaction;
}

static StyleEngineTransaction take_style_engine_transaction(Layout::BegunRead const& read, DOM::Document& document)
{
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
    auto setup_microseconds = (MonotonicTime::now() - transaction_setup_started_at).to_truncated_microseconds();
    document.style_invalidation_counters().style_engine_transaction_setup_microseconds += setup_microseconds;
    document.style_invalidation_counters().style_update_submission_microseconds += setup_microseconds;
    auto* root = document.document_element();
    if (!root || root->style_node_id() == 0) {
        style_computer.style_engine().flush();
        return {};
    }

    return accept_style_engine_transaction(document, style_computer.style_engine().take_style_transaction(read, root->style_node_id()));
}

// The style transaction that flew beside the event loop, taken in to drain its reactions.
static StyleEngineTransaction take_flown_style_engine_transaction(Layout::BegunRead const& read, DOM::Document& document)
{
    return accept_style_engine_transaction(document, document.style_computer().style_engine().take_flown_style_transaction(read));
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
        .record_reads = 0,
        .explicitly_inherited_groups = 0,
        .record_damage = 0,
        .owes_an_animation_plan = false,
        .owes_a_transition_step = false,
        .composed_by_the_host = false,
    };
}

// The swapped groups are the parent's base values; a child of a parent holding animated values inherits the
// animated ones, which the engine never sees.
static bool parent_style_has_animated_values(DOM::Element& element)
{
    auto parent = DOM::AbstractElement { element }.element_to_inherit_style_from();
    if (!parent.has_value())
        return false;
    auto parent_style = parent->computed_style();
    return parent_style && parent_style->has_animated_values();
}

// A C++ computation collects an element's animations into the style it computes. For a record the engine derived
// beneath them, the host samples them over the record once it is installed, as an animation update samples them over
// the record an element holds, and publishes what they compose.
static void sample_animations_for_installed_record(Layout::BegunRead const& read, DOM::AbstractElement abstract_element)
{
    auto style_record = abstract_element.style_record_identity();
    if (!style_record)
        return;
    Animations::AnimationUpdateContext::ElementData element_data { style_record, abstract_element.document().style_computer().reconstruct_computed_properties_for_animation(read, style_record) };
    element_data.base_is_current = true;
    Animations::AnimationUpdateContext context;
    context.elements.set(abstract_element, move(element_data));
    context.publish();
}

// Whether the custom-property environment an engine-computed record was published with can be
// installed: the one the element inherits - the parent's inheritable data, which is the parent's
// own unless a registration made some of it non-inherited - or one the engine resolved over it.
static bool engine_computed_record_environment_is_installable(Layout::BegunRead const& read, DOM::Element& element, StyleRecordID style_record)
{
    bool installable = false;
    (void)element.custom_property_environment_of_engine_record(read, style_record, installable);
    return installable;
}

static RefPtr<CustomPropertyData const> custom_property_environment_base(DOM::Element const& element, RefPtr<CustomPropertyData const> data)
{
    if (data && data->is_animation_overlay_for({ element }))
        return data->parent();
    return data;
}

// An element that has to compute again is recorded with a recompute reaction alone: its descendants
// are the move's, or that computation's, to reach. (The engine fans an inherited custom-properties
// reaction out to every child of an applied reaction.)
static void record_environment_move_recompute(StyleEngine& style_engine, DOM::Element& element)
{
    style_engine.record_derived_element_style_input_change(element.style_node_id(), StyleEngine::RecomputeStyle);
}

// The environments a moved element's pseudo-elements held: one that held the element's own takes the
// moved one, and one that held what the element hands down takes what the moved one hands down. A
// pseudo-element that resolved its own computes the element again.
static void move_pseudo_element_environments(Layout::BegunRead const& read, DOM::Document& document, DOM::Element& element, CustomPropertyData const* existing, CustomPropertyData const* existing_inheritable, RefPtr<CustomPropertyData const> const& moved)
{
    auto moved_inheritable = moved ? moved->inheritable(read, document) : nullptr;
    auto& style_engine = document.style_computer().style_engine();
    for (auto kind = 0; kind < to_underlying(PseudoElement::KnownPseudoElementCount); ++kind) {
        auto pseudo_element = static_cast<PseudoElement>(kind);
        auto pseudo_data = element.custom_property_data(pseudo_element);
        if (!pseudo_data)
            continue;
        if (pseudo_data.ptr() == existing)
            element.set_custom_property_data(pseudo_element, moved);
        else if (pseudo_data.ptr() == existing_inheritable)
            element.set_custom_property_data(pseudo_element, moved_inheritable);
        else
            record_environment_move_recompute(style_engine, element);
    }
}

static void move_custom_property_environment_below(Layout::BegunRead const&, DOM::Document&, DOM::Element&, RefPtr<CustomPropertyData const> const& old_base, RefPtr<CustomPropertyData const> const& new_base, CustomPropertyData const* changed_old_base, CustomPropertyData const* changed_new_base);

// An element declaring custom properties of its own over the environment it inherits, whose
// declared values stand because it reads nothing that changed: they are built again over the moved
// environment, and the move goes on below the element.
static void rebuild_custom_property_environment(Layout::BegunRead const& read, DOM::Document& document, DOM::Element& element, RefPtr<CustomPropertyData const> const& new_parent_inheritable, CustomPropertyData const* changed_old_base, CustomPropertyData const* changed_new_base)
{
    auto existing_base = custom_property_environment_base(element, element.custom_property_data({}));
    VERIFY(existing_base && existing_base->declared_count() > 0);
    if (existing_base->parent().ptr() == new_parent_inheritable.ptr())
        return;
    OrderedHashMap<Utf16FlyString, StyleProperty> own_values;
    size_t declared = 0;
    for (auto const& [name, property] : existing_base->own_values()) {
        if (declared++ >= existing_base->declared_count())
            break;
        own_values.set(name, property);
    }
    RefPtr<CustomPropertyData const> moved = CustomPropertyData::create(move(own_values), new_parent_inheritable);
    auto existing_inheritable = existing_base->inheritable(read, document);
    move_pseudo_element_environments(read, document, element, existing_base.ptr(), existing_inheritable.ptr(), moved);
    element.set_custom_property_data({}, moved);
    element.republish_style_record_environment(read);
    move_custom_property_environment_below(read, document, element, existing_base, moved, changed_old_base, changed_new_base);
}

// An element's custom properties moved. Every styled descendant holds the environment it inherits
// by identity, and the style engine keeps what each holds: it hands the moved environment to the
// descendants that hold the one the element handed down before, with their records, and answers
// what is left here. That is installing those records, the environments of element-backed
// pseudo-elements, which the engine does not keep, and the custom properties a descendant declares
// itself, built again over the moved environment. A descendant whose style reads a name whose value
// differs between `changed_old_base` and `changed_new_base`, or reads the environment another way,
// computes again.
static void move_custom_property_environment_below(Layout::BegunRead const& read, DOM::Document& document, DOM::Element& element, RefPtr<CustomPropertyData const> const& old_base, RefPtr<CustomPropertyData const> const& new_base, CustomPropertyData const* changed_old_base, CustomPropertyData const* changed_new_base)
{
    auto& style_computer = document.style_computer();
    auto& style_engine = style_computer.style_engine();
    auto old_inheritable = old_base ? old_base->inheritable(read, document) : nullptr;
    auto new_inheritable = new_base ? new_base->inheritable(read, document) : nullptr;
    auto named = [](CustomPropertyData const* data) -> StyleEngineFFI::FfiNamedEnvironment {
        return { .identity = data ? data->identity() : 0, .store = data ? data->rust_store() : nullptr };
    };
    StyleEngineFFI::FfiEnvironmentMove const moved {
        .old_base = named(changed_old_base),
        .new_base = named(changed_new_base),
        .old_inheritable = old_inheritable ? old_inheritable->identity() : 0,
        .new_inheritable = named(new_inheritable.ptr()),
        .new_inheritable_data = new_inheritable.ptr(),
        .new_inheritable_declares = new_inheritable && new_inheritable->declared_count() > 0,
    };
    auto answer = StyleEngineFFI::style_engine_move_custom_property_environment(style_engine.host(), &read, element.style_node_id().value(), moved);
    // The answer lives until the engine is next called, which acting on it does.
    Vector<StyleEngineFFI::FfiEnvironmentMoveAction> actions;
    actions.append(answer.actions, answer.count);
    for (auto const& action : actions) {
        auto descendant = style_computer.element_for_style_node(StyleNodeID { action.node });
        VERIFY(descendant);
        switch (action.kind) {
        case StyleEngineFFI::FfiEnvironmentMoveActionKind::Republish:
            for (auto kind = 0; kind < to_underlying(PseudoElement::KnownPseudoElementCount); ++kind) {
                auto pseudo_element = static_cast<PseudoElement>(kind);
                if (is_synthetic_pseudo_element(pseudo_element))
                    continue;
                auto pseudo_data = descendant->custom_property_data(pseudo_element);
                if (!pseudo_data)
                    continue;
                if (pseudo_data->identity() == action.replaced)
                    descendant->set_custom_property_data(pseudo_element, new_inheritable);
                else
                    record_environment_move_recompute(style_engine, *descendant);
            }
            if (action.style_record != descendant->style_record_identity().value())
                descendant->refresh_computed_style({}, StyleRecordID { action.style_record });
            break;
        case StyleEngineFFI::FfiEnvironmentMoveActionKind::Rebuild:
            rebuild_custom_property_environment(read, document, *descendant, new_inheritable, changed_old_base, changed_new_base);
            break;
        case StyleEngineFFI::FfiEnvironmentMoveActionKind::Recompute:
            record_environment_move_recompute(style_engine, *descendant);
            break;
        }
    }
}

static void propagate_custom_property_environment_move(Layout::BegunRead const& read, DOM::Document& document, DOM::Element& origin, RefPtr<CustomPropertyData const> old_origin_data)
{
    // Nothing inherits from an element with nothing below it in the flat tree.
    if (!origin.first_element_child() && !origin.shadow_root() && !is<HTML::HTMLSlotElement>(origin))
        return;
    auto old_origin_base = custom_property_environment_base(origin, move(old_origin_data));
    auto new_origin_base = custom_property_environment_base(origin, origin.custom_property_data({}));
    move_custom_property_environment_below(read, document, origin, old_origin_base, new_origin_base, old_origin_base.ptr(), new_origin_base.ptr());
}

// A record the engine settled for a row it published unsettled, by a demand: the row installs it as one the engine
// computed, with the pseudo-element records settled beside it.
static DOM::Element::EnginePseudoElementRecords take_engine_record(StyleEngine::PublishedStyleDelta& reaction, StyleEngineFFI::FfiEngineComputedRecord const& record)
{
    reaction.new_style_record = record.style_record;
    reaction.uses_substitution = record.uses_substitution;
    reaction.record_reads = record.record_reads;
    reaction.explicitly_inherited_groups = record.explicitly_inherited_groups;
    reaction.owes_an_animation_plan = record.owes_an_animation_plan;
    reaction.owes_a_transition_step = record.owes_a_transition_step;
    reaction.composed_by_the_host = record.composed_by_the_host;
    reaction.damage = StyleEngineFFI::FfiStyleDeltaDamage::Full;
    reaction.gap = StyleEngineFFI::FfiStyleDeltaGap::Computed;
    DOM::Element::EnginePseudoElementRecords pseudo_element_records {};
    for (size_t kind = 0; kind < array_size(record.pseudo_records); ++kind) {
        if ((record.pseudo_records_present >> kind) & 1)
            pseudo_element_records[kind] = StyleRecordID { record.pseudo_records[kind] };
    }
    return pseudo_element_records;
}

// Install the record the engine settled for a row, and the pseudo-element records beside it. A C++ computation applied
// the animation plan it decided beside the record it computed and collected the element's animations, those the plan
// starts among them, into that record; the host composes them over the record the engine settled once it is installed,
// and runs the transition step the row owes.
static RequiredInvalidationAfterStyleChange install_engine_computed_records(Layout::BegunRead const& read, DOM::Element& element, StyleEngine::PublishedStyleDelta const& reaction, DOM::Element::EnginePseudoElementRecords const& pseudo_element_records, DOM::Element::EngineRecordDamages const& engine_record_damages, bool acknowledge, bool& did_change_custom_properties)
{
    auto& document = element.document();
    auto& style_engine = document.style_computer().style_engine();
    // A C++ computation applies a change of the element's display once the record holds the element's animations and
    // its transition step has run, and refreshes its pseudo-elements after its own style; so does the installation of
    // the record the engine settled, below, from the style the element held.
    auto const old_computed_values = element.computed_style();
    // https://drafts.csswg.org/css-transitions-1/#starting
    // The transition step the row owes compares the record the element moved away from with the one it installs, so
    // the old record has to outlive its replacement until the step has read it.
    // NB: Earlier rows can replace the element's composition after this row was planned. Pin the record it holds now,
    //     which is the before-change style for this installation, rather than the row's possibly reclaimed old record.
    StyleRecordPin const before_change { document.style_computer(), reaction.owes_a_transition_step ? element.style_record_identity() : StyleRecordID {} };
    // The record answers any style input the element owes, as the C++ computation it equals would: nothing is left for
    // a later transaction to plan.
    style_engine.consume_recorded_element_style_input_change(reaction.style_node);
    auto invalidation = element.apply_engine_computed_style_record(read, StyleRecordID { reaction.new_style_record }, pseudo_element_records, reaction.uses_substitution, reaction.record_reads, reaction.explicitly_inherited_groups, did_change_custom_properties, DOM::Element::DisplayNoneChange::LeftToCaller, &engine_record_damages);
    if (acknowledge)
        style_engine.acknowledge_engine_computed_record(StyleNodeID { reaction.style_node });
    // The engine asks for the element's children only after the host composed the record. Which rows the host composes
    // is the engine's to say, since it settles the pseudo-elements of every other row beside the record, and every
    // element with animations is among them.
    ASSERT(reaction.composed_by_the_host || !(element.has_relevant_animations() || element.has_associated_animations()));
    if (reaction.composed_by_the_host) {
        DOM::AbstractElement abstract_element { element };
        if (reaction.owes_an_animation_plan)
            document.style_computer().apply_settled_animation_plan(read, abstract_element);
        // https://drafts.csswg.org/css-transitions-2/#defining-before-change-style
        // Sampling the installed record can keep the epoch's before-change style, and the record the element holds by
        // then is the after-change one. A row that owes the step keeps the record it moved away from first.
        if (!!before_change.style_record() && document.is_in_style_stabilization_epoch())
            document.style_computer().record_transition_stabilization_baseline(abstract_element, before_change.style_record());
        if (element.has_relevant_animations() || element.has_associated_animations())
            sample_animations_for_installed_record(read, abstract_element);
    }
    if (!!before_change.style_record())
        invalidation |= document.style_computer().run_transition_step_for_installed_record(read, { element }, before_change.style_record());
    if (old_computed_values)
        element.apply_display_none_change(read, DOM::Element::DisplayNoneState::of(*old_computed_values));
    // The pseudo-elements of a record the host composes inherit the composition, so the engine settles them only now.
    if (reaction.composed_by_the_host)
        invalidation |= element.refresh_pseudo_element_styles_over_composition(read, old_computed_values ? &*old_computed_values : nullptr, did_change_custom_properties);
    return invalidation;
}

// The engine's answer to a targeted demand for the element's record, where the host can install it. The engine
// resolved the record's environment over the parent's own; where the parent's inheritable environment differs, it
// takes back what the demand derived.
static Optional<StyleEngineFFI::FfiEngineComputedRecord> answer_targeted_record_demand(Layout::BegunRead const& read, DOM::Element& element)
{
    auto& style_engine = element.document().style_computer().style_engine();
    auto answer = style_engine.answer_record_demand(read, element.style_node_id(), StyleEngine::RecordDemand::TargetedElement).record;
    if (answer.style_record == 0)
        return {};
    if (!engine_computed_record_environment_is_installable(read, element, StyleRecordID { answer.style_record })) {
        style_engine.abandon_demanded_records(element.style_node_id());
        return {};
    }
    return answer;
}

// Whether an element the element inherits from owes a style input, which the next transaction answers.
static bool inheritance_ancestor_owes_style_input(Layout::BegunRead const& read, DOM::Element& element)
{
    auto const& style_engine = element.document().style_computer().style_engine();
    for (auto ancestor = DOM::AbstractElement { element }.element_to_inherit_style_from(); ancestor.has_value(); ancestor = ancestor->element_to_inherit_style_from()) {
        if (style_engine.has_deferred_element_style_input(read, ancestor->element().style_node_id()))
            return true;
    }
    return false;
}

// The engine declined the row. Nothing else computes styles: the element keeps the record it holds. The engine answers
// no demand below an ancestor that owes a style input, such as one whose custom-property animations published new
// values as an earlier row of the batch installed its record. That ancestor's style moves in the next transaction, so
// the row is owed to it too, which plans the row after the ancestor's.
static RequiredInvalidationAfterStyleChange refuse_style_row(Layout::BegunRead const& read, DOM::Element& element, u8 row_reaction, u8 row_inherited_style_groups)
{
    auto& style_engine = element.document().style_computer().style_engine();
    if (inheritance_ancestor_owes_style_input(read, element)) {
        style_engine.record_derived_element_style_input_change(element.style_node_id(), row_reaction, row_inherited_style_groups);
        return {};
    }
    style_engine.consume_recorded_element_style_input_change(element.style_node_id());
    if (!element.has_style())
        dbgln("StyleEngine: refused the first style of <{}> (style node {})", element.local_name(), element.style_node_id().value());
    return {};
}

// A row the engine did not settle in its transaction, or a targeted update: ask the engine for the element's record
// now, and install it as one the engine computed, moving the element from the record it holds.
static RequiredInvalidationAfterStyleChange apply_engine_record_demand(Layout::BegunRead const& read, DOM::Element& element, u8 row_reaction, u8 row_inherited_style_groups, bool& did_change_custom_properties)
{
    auto answer = answer_targeted_record_demand(read, element);
    if (!answer.has_value())
        return refuse_style_row(read, element, row_reaction, row_inherited_style_groups);
    StyleEngine::PublishedStyleDelta reaction {};
    reaction.style_node = element.style_node_id().value();
    reaction.old_style_record = element.style_record_identity().value();
    auto pseudo_element_records = take_engine_record(reaction, *answer);
    return install_engine_computed_records(read, element, reaction, pseudo_element_records, {}, true, did_change_custom_properties);
}

static RequiredInvalidationAfterStyleChange apply_style_engine_reactions(Layout::BegunRead const& read, DOM::Document& document, Vector<StyleEngine::PublishedStyleDelta> const& reactions)
{
    // Reactions are applied in preorder, so every element's inheritance inputs are ready when it is
    // applied. What an applied element's change means for its (flat-tree) children is the
    // engine's to derive: it reads each application and plans the children as the next
    // transaction of this style update.
    RequiredInvalidationAfterStyleChange transaction_invalidation;
    document.style_computer().style_engine().begin_noting_declaration_changes_during_apply();
    ScopeGuard end_noting_declaration_changes = [&] { document.style_computer().style_engine().end_noting_declaration_changes_during_apply(); };
    {
        for (size_t reaction_index = 0; reaction_index < reactions.size(); ++reaction_index) {
            auto const& published_reaction = reactions[reaction_index];
            // A pseudo-element record installs with its element's, which leads it.
            if (published_reaction.pseudo_kind != NumericLimits<u8>::max())
                continue;
            // An element the engine answered as hidden needs no style until a read or its subtree's
            // reveal asks for one.
            if (published_reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::Hidden)
                continue;
            auto element = document.style_computer().element_for_style_node(published_reaction.style_node);
            if (!element)
                continue;
            auto reaction = published_reaction;

            // The pseudo-element records a demand settled beside the element's record.
            Optional<DOM::Element::EnginePseudoElementRecords> settled_pseudo_element_records;

            // A host that rewrote the element's declarations while an earlier row was applied (a form control restyling
            // its shadow tree as its own style moves) leaves the record the engine computed from the old ones: the row is
            // answered from its demand, over the declarations as they are now.
            if (reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::Computed && document.style_computer().style_engine().declarations_changed_during_apply(StyleNodeID { reaction.style_node })) {
                reaction.gap = StyleEngineFFI::FfiStyleDeltaGap::Materialize;
                reaction.new_style_record = 0;
                reaction.damage = StyleEngineFFI::FfiStyleDeltaDamage::None;
                reaction.reaction |= StyleEngine::RecomputeStyle;
                settled_pseudo_element_records.clear();
            }

            // NB: An earlier row's environment move can republish the record an element holds over the moved environment.
            //     A swap of its inherited groups planned over the record it held before would undo the move: the row is
            //     answered from its demand, over the record the element holds now.
            if (reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::None && element->has_style() && reaction.old_style_record != element->style_record_identity().value()) {
                reaction.gap = StyleEngineFFI::FfiStyleDeltaGap::Materialize;
                reaction.new_style_record = 0;
                reaction.damage = StyleEngineFFI::FfiStyleDeltaDamage::None;
            }

            // A reaction the engine derived for this element while applying an earlier one in
            // this batch joins the element's own reaction where it covers it, which a demand for
            // the element's record always does.
            if (auto absorbed = document.style_computer().style_engine().absorb_element_style_input(
                    read, StyleNodeID { reaction.style_node }, reaction.reaction, reaction.inherited_style_groups,
                    reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::Materialize);
                absorbed != 0) {
                reaction.reaction = static_cast<u8>(absorbed & 0xff);
                reaction.inherited_style_groups = static_cast<u8>(absorbed >> 8);
            }

            // An engine-computed record installs on an element without style as a first record does, its style cleared
            // on entry to display:none included; other record deltas assume the style they move.
            if (!element->has_style()
                && reaction.gap != StyleEngineFFI::FfiStyleDeltaGap::Materialize
                && reaction.gap != StyleEngineFFI::FfiStyleDeltaGap::Computed)
                continue;
            // NB: An inheritance scheduling row carries no computation of its own. If an earlier display:none
            //     reaction cleared its style and no derived input reached it, leave it unstyled until a read or reveal.
            if (reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::Materialize && reaction.reaction == 0 && !element->has_style())
                continue;

            bool const needs_regular_style_recompute = reaction.reaction & (StyleEngine::PublishedStyle | StyleEngine::RecomputeStyle | StyleEngine::RecomputeDescendantStyles | StyleEngine::AncestorBecameVisible);
            bool const needs_custom_property_recompute = reaction.reaction & StyleEngine::InheritedCustomProperties;
            bool const needs_inherited_style_recompute = reaction.reaction & StyleEngine::InheritedStyle;
            // An element declaring custom properties of its own layers them over the environment it
            // inherits, which its cascade decides.
            bool const needs_full_custom_property_recompute = needs_custom_property_recompute
                && (element->style_uses_var_css_function() || element->style_uses_inherit_css_function()
                    || document.style_computer().style_engine().node_declares_custom_properties(read, reaction.style_node));

            bool answered_by_demand = false;
            // A row the engine did not settle in its transaction, and that computes the element's style, is answered
            // from its record demand, as a targeted update of the element is: the record installs as one the engine
            // computed, moving the element from the record it holds.
            if (reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::Materialize && (needs_regular_style_recompute || needs_inherited_style_recompute || needs_full_custom_property_recompute)) {
                if (auto answer = answer_targeted_record_demand(read, *element); answer.has_value()) {
                    reaction.old_style_record = element->style_record_identity().value();
                    reaction.record_damage = 0;
                    settled_pseudo_element_records = take_engine_record(reaction, *answer);
                    answered_by_demand = true;
                }
            }

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

            auto old_custom_property_data = element->custom_property_data({});
            bool const was_unstyled = !element->style_record_identity();
            auto const* previous_box_values = element->style_group<ComputedValues::BoxValues>();
            auto const previous_display = previous_box_values
                ? Optional<Display> { display_from_ffi_display(previous_box_values->display) }
                : Optional<Display> {};
            bool const was_display_none = previous_display.has_value() && previous_display->is_none();
            auto const* previous_inherited_box_values = element->style_group<ComputedValues::InheritedBoxValues>();
            auto const previous_visibility = previous_inherited_box_values
                ? Optional<Visibility> { static_cast<Visibility>(previous_inherited_box_values->visibility) }
                : Optional<Visibility> {};
            bool did_change_custom_properties = false;
            RequiredInvalidationAfterStyleChange invalidation;

            // The engine answered its records with what the moves from the records they name damage.
            DOM::Element::EngineRecordDamages engine_record_damages;
            if (reaction.record_damage & to_underlying(StyleEngineFFI::FfiStyleInvalidationField::EngineComputed))
                engine_record_damages.element = DOM::Element::EngineRecordDamage { StyleRecordID { reaction.old_style_record }, StyleRecordID { reaction.new_style_record }, reaction.record_damage };
            if (reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::None) {
                VERIFY(!needs_regular_style_recompute);
                VERIFY(needs_inherited_style_recompute);
                VERIFY(!needs_custom_property_recompute);
                VERIFY(reaction.pseudo_kind == NumericLimits<u8>::max());
                // The engine swapped the element's inherited groups for its parent's: the record
                // installs as an engine record. The engine refuses the swap to an element that
                // animates, declares transitions, or inherits from an animating parent. A parent
                // this batch installed may have taken animated values from its own ancestors since
                // the engine swapped, which the element inherits in place of the base ones: the
                // row is answered from its demand.
                if (parent_style_has_animated_values(*element))
                    invalidation = apply_engine_record_demand(read, *element, reaction.reaction, reaction.inherited_style_groups, did_change_custom_properties);
                else
                    invalidation = install_engine_computed_records(read, *element, reaction, {}, engine_record_damages, false, did_change_custom_properties);
            } else if (reaction.gap == StyleEngineFFI::FfiStyleDeltaGap::Computed) {
                // The engine computed the new record from this element's moved cascade winners,
                // from its parent's moved inherited style or display, or from its moved
                // inherited custom-property environment.
                VERIFY(needs_regular_style_recompute || needs_inherited_style_recompute || needs_custom_property_recompute);
                VERIFY(reaction.pseudo_kind == NumericLimits<u8>::max());
                auto pseudo_element_records = settled_pseudo_element_records.value_or({});
                // A demand settled the pseudo-elements beside the element's record; the rows the batch published beside
                // the element's moved from a record the demand replaced.
                for (auto next = reaction_index + 1; !answered_by_demand && next < reactions.size() && reactions[next].style_node == published_reaction.style_node && reactions[next].pseudo_kind != NumericLimits<u8>::max(); ++next) {
                    auto const& pseudo_reaction = reactions[next];
                    pseudo_element_records[pseudo_reaction.pseudo_kind] = StyleRecordID { pseudo_reaction.new_style_record };
                    if (pseudo_reaction.record_damage & to_underlying(StyleEngineFFI::FfiStyleInvalidationField::EngineComputed)) {
                        engine_record_damages.pseudo_elements[pseudo_reaction.pseudo_kind] = DOM::Element::EnginePseudoElementRecordDamage {
                            .move = { StyleRecordID { pseudo_reaction.old_style_record }, StyleRecordID { pseudo_reaction.new_style_record }, pseudo_reaction.record_damage },
                            .originating_style_record = StyleRecordID { reaction.new_style_record },
                        };
                    }
                }
                if (!engine_computed_record_environment_is_installable(read, *element, StyleRecordID { reaction.new_style_record })) {
                    // The engine resolved the record's environment over the parent's own, which an earlier row of the
                    // batch moved: the row is answered from a fresh demand.
                    invalidation = apply_engine_record_demand(read, *element, reaction.reaction, reaction.inherited_style_groups, did_change_custom_properties);
                } else {
                    invalidation = install_engine_computed_records(read, *element, reaction, pseudo_element_records, engine_record_damages, true, did_change_custom_properties);
                }
            } else if (needs_regular_style_recompute || needs_inherited_style_recompute || needs_full_custom_property_recompute) {
                // The engine declined the row's demand above.
                invalidation = refuse_style_row(read, *element, reaction.reaction, reaction.inherited_style_groups);
            }
            // A row that owes only a moved inherited environment on an element whose style reads none
            // has nothing left for the host: the engine moved the element's environment, and the
            // record over it, when its parent's moved.

            auto const* current_inherited_box_values = element->style_group<ComputedValues::InheritedBoxValues>();
            if (previous_visibility.has_value() && current_inherited_box_values
                && *previous_visibility != static_cast<Visibility>(current_inherited_box_values->visibility)) {
                document.throttled_animation_visibility_changed();
            }

            apply_element_style_invalidation_after_style_change(read, *element, invalidation);
            transaction_invalidation |= invalidation;

            auto& style_engine = document.style_computer().style_engine();
            auto current_style_record = element->style_record_identity();
            u32 facts = 0;
            // The environment moved: the element's descendants take it here, and the ones that read
            // a moved name are recorded for their own computation. The engine derives no reactions
            // for the move.
            if (did_change_custom_properties)
                propagate_custom_property_environment_move(read, document, *element, old_custom_property_data);
            // A counter-style rebuild moves no style, so it is not what the children react to.
            if (invalidation.style_change_is_none())
                facts |= StyleEngine::InvalidationIsNone;
            if (invalidation.style_change_needs_layout_tree_rebuild())
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
                auto const* current_box_values = element->style_group<ComputedValues::BoxValues>();
                VERIFY(current_box_values);
                if (previous_display.has_value() && *previous_display != display_from_ffi_display(current_box_values->display))
                    facts |= StyleEngine::DisplayChanged;
            } else {
                VERIFY(was_unstyled);
            }
            style_engine.note_style_reaction_applied(reaction.style_node, reaction.reaction, invalidation.inherited_style_groups_changed(), facts);
        }
    }

    return transaction_invalidation;
}

template<FirstStyleTransaction first_transaction>
static void update_style(Layout::BegunRead const& read, DOM::Document& document, [[maybe_unused]] DocumentWithoutBrowsingContext document_without_browsing_context)
{
    // NB: The style update of a read with no transaction in flight is compiled without the drain's steps.
    constexpr bool drains_flown_transaction = first_transaction == FirstStyleTransaction::Flown;
    auto style_update_started_at = MonotonicTime::now();
    auto& timing_counters = document.style_invalidation_counters();
    auto const submission_before = timing_counters.style_update_submission_microseconds;
    auto const bridge_before = timing_counters.style_update_bridge_microseconds;
    auto const apply_before = timing_counters.style_update_apply_microseconds;
    ScopeGuard record_style_update_time = [&] {
        auto whole = (MonotonicTime::now() - style_update_started_at).to_truncated_microseconds();
        auto measured = timing_counters.style_update_submission_microseconds - submission_before
            + timing_counters.style_update_bridge_microseconds - bridge_before
            + timing_counters.style_update_apply_microseconds - apply_before;
        timing_counters.style_update_microseconds += whole;
        // NB: Counters are read only to attribute time, never to choose style work. The
        //     intervals are disjoint; rounding each down leaves fractional time here too.
        timing_counters.style_update_remainder_microseconds += whole - measured;
    };
    StyleValueFFI::rust_style_ffi_complete_style_update_begin();
    ScopeGuard leave_complete_style_update = finish_complete_style_update;

    // NB: The drain of a transaction that flew installs what was computed from the inputs it was sealed with, which
    //     settled the parent document's layout, and is due whatever became of the document since.
    if constexpr (!drains_flown_transaction) {
        // NOTE: If our parent document needs a relayout, we must do that *first*. This is required as it may cause the
        // viewport to change which will can affect media query evaluation and the value of the `vw` unit.
        // OPTIMIZATION: A settled embedding chain has no relayout to do, and finding that out by laying it out publishes
        //               its whole style environment.
        if (auto navigable = document.navigable(); navigable && navigable->container() && &navigable->container()->document() != &document
            && !embedding_document_chain_has_no_pending_style_or_layout_work(document))
            navigable->container()->document().update_layout(DOM::UpdateLayoutReason::ChildDocumentStyleUpdate);

        if (!document.browsing_context() && document_without_browsing_context == DocumentWithoutBrowsingContext::Skip)
            return;

        // NOTE: If this is a document hosting <template> contents, style update is unnecessary.
        if (document.created_for_appropriate_template_contents())
            return;
    }

    [[maybe_unused]] auto submission_started_at = MonotonicTime::now();
    // What publishes by identity below (animations, the elements prepared for style) finds the nodes that connected
    // since the last update under the identities they take here.
    // NB: What connected beside the transaction that flew is the next transaction's.
    if constexpr (!drains_flown_transaction)
        take_in_pending_style_arrivals(document);
    document.style_computer().begin_style_update();
    ScopeGuard end_style_update = [&] {
        document.style_computer().end_style_update();
    };

    document.style_computer().begin_style_record_view_epoch();
    ScopeGuard end_style_record_view_epoch = [&] {
        document.style_computer().end_style_record_view_epoch();
    };

    // NB: What was recorded beside the transaction that flew is the next transaction's.
    if constexpr (!drains_flown_transaction)
        document.synchronize_dirty_style_attributes();

    document.begin_style_stabilization_epoch();
    ScopeGuard end_stabilization_epoch = [&] {
        document.end_style_stabilization_epoch();
    };

    // Fetch the viewport rect once, instead of repeatedly, during style computation.
    document.update_style_computer_viewport_rect();

    if constexpr (!drains_flown_transaction) {
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
        timing_counters.style_update_submission_microseconds += (MonotonicTime::now() - submission_started_at).to_truncated_microseconds();
        if (document.has_completed_style_update()
            && !document.style_computer().style_engine().has_pending_transaction(read)) {
            if (!document.needs_animated_style_update())
                return;
            document.sample_animation_effects_needing_style_update();
            if (!document.style_computer().style_engine().has_pending_transaction(read))
                return;
        }

        // Settle each tree scope's counter-style registry before the engine answers any row: a record
        // naming a counter style names the registry it was computed against, and a shadow tree can
        // define its own. None of it depends on layout.
        (void)document.style_scope().counter_style_environment_identity(read);
        document.for_each_shadow_root([&read](DOM::ShadowRoot& shadow_root) {
            (void)shadow_root.style_scope().counter_style_environment_identity(read);
        });
    }

    // A style flush is a transaction boundary. Everything recorded since the last one crosses into
    // StyleEngine as one flat batch, is normalized there, and is routed into the region its
    // transpose programs reach. A transaction that could not be proven narrower publishes a
    // complete document reaction batch. Only a transaction that cannot complete its answers falls
    // back to document invalidation.
    auto style_engine_transaction = drains_flown_transaction ? take_flown_style_engine_transaction(read, document) : take_style_engine_transaction(read, document);
    // What was written beside the transaction that flew reaches the engine once the drain of its reactions has ended,
    // as the next transaction's input.
    ScopeGuard end_flown_style_drain = [&] {
        if constexpr (drains_flown_transaction)
            document.style_computer().style_engine().end_flown_style_drain();
    };
    ScopeGuard discard_style_engine_transaction_outputs = [&] {
        document.style_computer().style_engine().discard_style_transaction_outputs(read);
    };

    if (!style_engine_transaction.reactions.is_empty())
        document.note_style_stabilization_has_style_reactions();
    // NB: A transaction flies only while the document runs no animation, so one that runs now started beside it: the
    //     next transaction samples it.
    if constexpr (!drains_flown_transaction)
        document.sample_animation_effects_needing_style_update();

    auto style_engine_reactions = move(style_engine_transaction.reactions);
    auto prefers_broad_matching_batch = style_engine_transaction.prefers_broad_matching_batch;
    auto transaction_only_derived_child_reactions = style_engine_transaction.only_derived_child_reactions;
    if (style_engine_reactions.is_empty()
        && document.style_computer().style_engine().has_pending_transaction(read)) {
        auto feedback_transaction = take_style_engine_transaction(read, document);
        style_engine_reactions = move(feedback_transaction.reactions);
        prefers_broad_matching_batch = feedback_transaction.prefers_broad_matching_batch;
        transaction_only_derived_child_reactions = feedback_transaction.only_derived_child_reactions;
    }

    document.build_registered_properties_cache_for_style_update();

    // This pass belongs to the current style change event. The outer stabilization epoch advanced
    // the transition generation once, before any style, animation, or layout feedback ran.
    document.record_style_stabilization_pass(read);

    if (style_engine_reactions.is_empty())
        return;

    bool has_cold_matching_traversal = false;
    if (auto* root = document.document_element(); root && root->style_node_id() != 0) {
        if (prefers_broad_matching_batch) {
            has_cold_matching_traversal = document.style_computer().style_engine().begin_cold_matching_batch(read, root->style_node_id());
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
        auto apply_started_at = MonotonicTime::now();
        ArmedScopeGuard record_apply_time = [&] {
            timing_counters.style_update_apply_microseconds += (MonotonicTime::now() - apply_started_at).to_truncated_microseconds();
        };
        // One more tree generation of the same style change is not a new pass of it.
        if (style_reaction_pass++ > 0 && !transaction_only_derived_child_reactions)
            document.record_style_stabilization_pass(read);

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

        HashTable<StyleNodeID> reactions_of_next_transaction;
        // A reaction can name an element created by editing after its new inheritance parent was
        // inserted. Close the batch over unstyled inheritance prerequisites, which are bounded by
        // the reaction paths rather than discovered by a document traversal. An element the engine
        // answered as hidden needs no style to inherit from.
        for (size_t index = 0; index < style_engine_reactions.size(); ++index) {
            if (style_engine_reactions[index].gap == StyleEngineFFI::FfiStyleDeltaGap::Hidden)
                continue;
            auto element = document.style_computer().element_for_style_node(style_engine_reactions[index].style_node);
            if (!element || !element->is_connected() || &element->document() != &document)
                continue;
            auto const& style_engine = document.style_computer().style_engine();
            auto inherits_from_arrival_beside_flown_transaction = [&] {
                for (auto ancestor = DOM::AbstractElement { *element }.element_to_inherit_style_from(); ancestor.has_value() && !ancestor->has_style(); ancestor = ancestor->element_to_inherit_style_from()) {
                    if (style_engine.style_node_arrived_or_retired_beside_flown_transaction(ancestor->element().style_node_id()))
                        return true;
                }
                return false;
            };
            if (drains_flown_transaction && inherits_from_arrival_beside_flown_transaction()) {
                // NB: The transaction that flew computed the element's style under the ancestors it had as it was
                //     sealed. The arrival of its new one is the next transaction's, which styles it under that.
                reactions_of_next_transaction.set(StyleNodeID { style_engine_reactions[index].style_node });
                continue;
            }
            for (auto ancestor = DOM::AbstractElement { *element }.element_to_inherit_style_from(); ancestor.has_value() && !ancestor->has_style(); ancestor = ancestor->element_to_inherit_style_from()) {
                auto prerequisite = ancestor->element().style_node_id();
                VERIFY(prerequisite != 0);
                if (reaction_set.set(prerequisite) == AK::HashSetResult::InsertedNewEntry) {
                    style_engine_reactions.append(make_materialize_gap_delta(prerequisite, StyleEngine::RecomputeStyle));
                    inheritance_closure.append(prerequisite);
                }
            }
        }
        if (!reactions_of_next_transaction.is_empty()) {
            style_engine_reactions.remove_all_matching([&](auto const& reaction) {
                return reactions_of_next_transaction.contains(StyleNodeID { reaction.style_node });
            });
            for (auto style_node : reactions_of_next_transaction)
                reaction_set.remove(style_node);
        }

        // A published descendant may have an inheritance ancestor in the batch while the nodes
        // between them have no selector reaction of their own. Keep zero-bit scheduling slots for
        // that gap so derived inheritance bits can reach the descendant before its published
        // reaction is consumed, unless the descendant is hidden.
        auto reaction_count_before_inheritance_closure = style_engine_reactions.size();
        Vector<StyleNodeID, 16> inheritance_gap;
        for (size_t index = 0; index < reaction_count_before_inheritance_closure; ++index) {
            if (style_engine_reactions[index].gap == StyleEngineFFI::FfiStyleDeltaGap::Hidden)
                continue;
            auto element = document.style_computer().element_for_style_node(style_engine_reactions[index].style_node);
            if (!element)
                continue;
            inheritance_gap.clear_with_capacity();
            for (auto ancestor = DOM::AbstractElement { *element }.element_to_inherit_style_from(); ancestor.has_value(); ancestor = ancestor->element_to_inherit_style_from()) {
                auto ancestor_style_node = ancestor->element().style_node_id();
                VERIFY(ancestor_style_node != 0);
                // NB: The transaction that flew knows nothing of a node that arrived beside it, nor of the descendants
                //     the node took in. The next transaction styles them under it.
                if (drains_flown_transaction && document.style_computer().style_engine().style_node_arrived_or_retired_beside_flown_transaction(ancestor_style_node))
                    break;
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
            VERIFY(document.style_computer().style_engine().complete_published_match_answers_for_closure(read, inheritance_closure));

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
            document.style_computer().style_engine().sort_style_deltas_for_direct_application(read, applicable_style_engine_reactions);
            auto& counters = document.style_invalidation_counters();
            if (published_reaction_count > 0) {
                ++counters.style_engine_reaction_batch_runs;
                counters.style_engine_reaction_elements += published_reaction_count;
            }
            invalidation |= apply_style_engine_reactions(read, document, applicable_style_engine_reactions);
        }

        timing_counters.style_update_apply_microseconds += (MonotonicTime::now() - apply_started_at).to_truncated_microseconds();
        record_apply_time.disarm();

        // Exact consequences produced while recomputing become the next transaction in this
        // stabilization epoch. Take it only after consuming the current published answers, since
        // a new transaction retires their scratch.
        if (document.style_computer().style_engine().has_pending_transaction(read)) {
            auto next_transaction = take_style_engine_transaction(read, document);
            style_engine_reactions = move(next_transaction.reactions);
            transaction_only_derived_child_reactions = next_transaction.only_derived_child_reactions;
        }
        if (style_engine_reactions.is_empty())
            break;
    }

    document.set_has_completed_style_update();
    apply_document_style_invalidation_after_style_change(document, invalidation);
    if constexpr (!drains_flown_transaction)
        document.sample_animation_effects_needing_style_update();
}

// Records what update_style() records before it takes the document's style transaction, and lets the transaction fly
// beside the event loop where `blocker` is none: the next update_style() takes its reactions in, once it has landed.
static bool let_style_update_fly(Layout::BegunRead const& read, DOM::Document& document, Layout::RustFFI::FfiFlightBlocker blocker)
{
    if (!document.browsing_context() || document.created_for_appropriate_template_contents())
        return false;
    auto* root = document.document_element();
    if (!root || root->style_node_id() == 0)
        return false;
    auto& style_computer = document.style_computer();
    style_computer.begin_style_update();
    ScopeGuard end_style_update = [&] {
        style_computer.end_style_update();
    };
    document.synchronize_dirty_style_attributes();
    document.update_style_computer_viewport_rect();
    style_computer.prepare_elements_for_style_computation();
    if (document.needs_media_rule_evaluation())
        document.evaluate_media_rules_for_style_update();
    record_non_author_stylesheets(document);
    if (!style_computer.style_engine().has_pending_transaction(read))
        return false;
    (void)document.style_scope().counter_style_environment_identity(read);
    document.for_each_shadow_root([&read](DOM::ShadowRoot& shadow_root) {
        (void)shadow_root.style_scope().counter_style_environment_identity(read);
    });
    document.build_registered_properties_cache_for_style_update();
    style_computer.prepare_for_style_engine_transaction();
    return style_computer.style_engine().let_style_transaction_fly(read, root->style_node_id(), blocker);
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
    if (invalidation.style_change_is_none())
        facts |= StyleEngine::InvalidationIsNone;
    if (invalidation.style_change_needs_layout_tree_rebuild())
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
    style_engine.note_style_reaction_applied(element.style_node_id(), reaction, invalidation.inherited_style_groups_changed(), facts);
}

static void apply_targeted_style_invalidation(Layout::BegunRead const& read, DOM::Element& element, RequiredInvalidationAfterStyleChange const& invalidation, bool did_change_custom_properties, bool descendant_style_recompute_needed, bool was_unstyled, bool was_display_none, bool display_changed)
{
    if (!invalidation.is_none() || did_change_custom_properties)
        Invalidation::invalidate_assigned_slottables_after_slot_style_change(element);
    apply_element_style_invalidation_after_style_change(read, element, invalidation);
    note_targeted_style_reaction_applied(element, invalidation, did_change_custom_properties, descendant_style_recompute_needed, was_unstyled, was_display_none, display_changed);
    apply_document_style_invalidation_after_style_change(element.document(), invalidation);
}

// A targeted style update has nothing to do when every source of style work in the document is settled: A full style
// update has completed, no style engine transaction or recorded input is pending, no media rule evaluation is queued,
// and no animated style refresh is due.
static bool document_has_no_pending_style_work(Layout::BegunRead const& read, DOM::Document const& document)
{
    // A rootless flush drains the journal but preserves element style inputs for the first transaction with a document
    // root — so has_pending_transaction() alone would report a settled engine still owing an element its recomputation.
    return document.has_completed_style_update()
        && !document.style_computer().style_engine().has_pending_transaction(read)
        && !document.style_computer().style_engine().has_deferred_element_style_inputs(read)
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
        // The embedding document's state is that document's own read.
        Layout::ForcedReadScope embedding_read { embedding_document, false };
        if (!document_has_no_pending_style_work(embedding_read, embedding_document)
            || !embedding_document.layout_is_up_to_date()
            || !container->has_style())
            return false;
        embedded_document = &embedding_document;
    }
    return true;
}

static bool update_style_for_element(Layout::BegunRead const& read, DOM::Document& document, DOM::AbstractElement const& abstract_element, StyleUpdateMode mode)
{
    if (!abstract_element.element().is_connected())
        return false;
    document.ensure_style_engine_tracks_tree();

    // OPTIMIZATION: When nothing style-related is pending anywhere that could affect this document, the only question
    // left is, if the element's inheritance chain already has style. If it does, the walk below would conclude there's
    // nothing to recompute. So answer that directly — without constructing a style record view for every ancestor.
    // Stopping at display:none, the walk also answers no as soon as it meets a display:none element above every
    // element that has no style yet, so that is answered directly too.
    if ((mode == StyleUpdateMode::OnlyIfNeeded || mode == StyleUpdateMode::StopAtDisplayNone)
        && !abstract_element.pseudo_element().has_value()
        && document_has_no_pending_style_work(read, document)
        && embedding_document_chain_has_no_pending_style_or_layout_work(document)) {
        Optional<size_t> topmost_element_without_style;
        Optional<size_t> topmost_display_none_element;
        size_t depth = 0;
        for (Optional<DOM::AbstractElement> cursor = abstract_element; cursor.has_value(); cursor = cursor->element_to_inherit_style_from(), ++depth) {
            auto const& element = cursor->element();
            if (!element.has_style()) {
                topmost_element_without_style = depth;
                continue;
            }
            if (mode != StyleUpdateMode::StopAtDisplayNone)
                continue;
            auto const* box_values = element.style_group<ComputedValues::BoxValues>();
            if (box_values && display_from_ffi_display(box_values->display).is_none())
                topmost_display_none_element = depth;
        }
        if (topmost_display_none_element.has_value()
            && (!topmost_element_without_style.has_value() || *topmost_display_none_element > *topmost_element_without_style))
            return false;
        if (!topmost_element_without_style.has_value())
            return true;
    }

    take_in_pending_style_arrivals(document);
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
    // OPTIMIZATION: An embedding chain with no style or layout work anywhere has nothing to bring up to date, and a
    //               style and layout update of a settled document still publishes its whole environment to find that
    //               out. A page that focuses elements in a frame asks this once per focus.
    if (auto navigable = document.navigable(); navigable && navigable->container() && &navigable->container()->document() != &document
        && !embedding_document_chain_has_no_pending_style_or_layout_work(document)) {
        auto& container = *navigable->container();
        auto& embedding_document = container.document();
        // The container's style is the embedding document's own read.
        Layout::ForcedReadScope embedding_read { embedding_document, false };
        update_style_for_element(embedding_read, embedding_document, DOM::AbstractElement { container }, StyleUpdateMode::OnlyIfNeeded);
        embedding_document_layout_was_stale = !embedding_document.layout_is_up_to_date();
        embedding_document.update_layout(DOM::UpdateLayoutReason::ChildDocumentStyleUpdate);
    }

    bool entered_stabilization_epoch = false;
    ScopeGuard end_stabilization_epoch = [&] {
        if (entered_stabilization_epoch)
            document.end_style_stabilization_epoch();
    };

    bool ran_regular_style_update = false;
    // NB: A document without a browsing context (for example, one from createHTMLDocument()) has no rendering
    //     opportunities to publish its elements to the style engine. A targeted read runs its transaction here, so
    //     the engine has the facts it matches the read element and its ancestors against.
    if (document.browsing_context() || !document.created_for_appropriate_template_contents()) {
        document.begin_style_stabilization_epoch();
        entered_stabilization_epoch = true;
        document.update_style_computer_viewport_rect();

        if (document.style_computer().style_engine().has_pending_transaction(read) || document.needs_media_rule_evaluation())
            document.note_style_stabilization_has_style_reactions();

        // Media query evaluation can enqueue normal style invalidations, so do it before deciding what pending
        // invalidation work needs to run.
        if (document.needs_media_rule_evaluation())
            document.evaluate_media_rules_for_style_update();

        auto const can_run_regular_style_update = !document.is_running_update_layout()
            && (!document.has_completed_style_update()
                || document.style_computer().style_engine().has_pending_transaction(read));
        if (can_run_regular_style_update) {
            update_style(read, document, DocumentWithoutBrowsingContext::Update);
            ran_regular_style_update = true;
        } else {
            document.sample_animation_effects_needing_style_update();
            if (!document.is_running_update_layout()
                && document.style_computer().style_engine().has_pending_transaction(read)) {
                update_style(read, document, DocumentWithoutBrowsingContext::Update);
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
                return update_style_for_element(read, document, DOM::AbstractElement { element_reference->referenced_element() }, mode);
        }
    }

    if (ran_regular_style_update && mode != StyleUpdateMode::OnlyIfNeeded) {
        auto style_record = abstract_element.style_record_identity();
        if (!!style_record
            && !has_flag(document.style_computer().style_engine().style_record_dependency_flags(read, style_record), StyleRecordDependencyFlag::InDisplayNoneSubtree))
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
            && (document.style_computer().style_engine().has_deferred_element_style_input(read, ancestor->style_node_id())
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
        auto invalidation = apply_engine_record_demand(read, element, StyleEngine::PublishedStyle | StyleEngine::RecomputeStyle, 0, did_change_custom_properties);
        auto const* current_box_values = element->style_group<ComputedValues::BoxValues>();
        bool const display_changed = previous_display.has_value() && current_box_values && *previous_display != display_from_ffi_display(current_box_values->display);
        apply_targeted_style_invalidation(read, element, invalidation, did_change_custom_properties, descendant_style_recompute_needed, was_unstyled, was_display_none, display_changed);

        descendant_style_recompute_needed |= invalidation.recompute_descendant_styles;

        // The engine can refuse a first style, which leaves the rest of the chain nothing to inherit from.
        if (!element->has_style())
            return abstract_element.has_style();
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

Document::HighlightStyleObservability& Document::highlight_style_observability(CSS::PseudoElement pseudo_element)
{
    VERIFY(CSS::is_highlight_pseudo_element(pseudo_element));
    return m_highlight_style_observability[pseudo_element == CSS::PseudoElement::Selection ? 0 : 1];
}

bool Document::highlight_styles_are_observable(CSS::PseudoElement pseudo_element) const
{
    return const_cast<Document&>(*this).highlight_style_observability(pseudo_element).observable;
}

void Document::set_needs_highlight_style_update(CSS::PseudoElement pseudo_element)
{
    highlight_style_observability(pseudo_element).needs_update = true;
}

static void record_highlight_style_inputs(Document& document, Node& root, Range const* range)
{
    auto record_element = [&](Node& node) {
        if (auto* element = as_if<Element>(node); element && element->has_style()) {
            document.style_computer().style_engine().record_derived_element_style_input_change(element->style_node_id(),
                CSS::StyleEngine::PseudoInputsMayHaveChanged);
        }
    };

    // NB: Ancestors supply inherited highlight styles, and text controls paint through
    //     their internal shadow trees. Neither requires visiting unrelated subtrees.
    for (auto* ancestor = root.flat_tree_parent(); ancestor; ancestor = ancestor->flat_tree_parent())
        record_element(*ancestor);
    root.for_each_shadow_including_inclusive_descendant([&](Node& node) {
        if (range && &node.root() == &range->start_container()->root() && !range->intersects_node(node))
            return TraversalDecision::SkipChildrenAndContinue;
        record_element(node);
        return TraversalDecision::Continue;
    });
}

struct HighlightStyleInput {
    GC::Ref<Node> root;
    GC::Ptr<Range> range;
};

static Vector<HighlightStyleInput, 2> selection_style_inputs(Document& document)
{
    Vector<HighlightStyleInput, 2> inputs;
    if (auto selection = document.get_selection(); selection && !selection->is_collapsed()) {
        auto range = selection->range();
        inputs.append({ range->common_ancestor_container(), range });
    }
    if (auto* text_control = as_if<HTML::FormAssociatedTextControlElement>(document.focused_area().ptr()); text_control && text_control->selection_start() != text_control->selection_end())
        inputs.append({ *document.focused_area(), nullptr });
    return inputs;
}

static Vector<HighlightStyleInput, 2> search_text_style_inputs(Document& document)
{
    Vector<HighlightStyleInput, 2> inputs;
    if (auto active_match = document.find_in_page_active_match(); active_match && !active_match->collapsed())
        inputs.append({ active_match->common_ancestor_container(), active_match });
    return inputs;
}

void Document::update_highlight_style_observability(CSS::PseudoElement pseudo_element)
{
    // NB: Editing commands temporarily select content to restore its formatting. Style reads
    //     during the action must not activate selection styles throughout the document for these
    //     intermediate ranges. Observe the final selection after the action, including in input
    //     event handlers. Explicit ::selection queries can still compute their style on demand.
    if (pseudo_element == CSS::PseudoElement::Selection && m_running_editing_command_action)
        return;

    auto inputs = pseudo_element == CSS::PseudoElement::Selection ? selection_style_inputs(*this) : search_text_style_inputs(*this);
    auto& state = highlight_style_observability(pseudo_element);
    bool observable = !inputs.is_empty();
    if (observable == state.observable && !state.needs_update)
        return;
    state = { .observable = observable, .needs_update = false };
    style_computer().style_engine().set_pseudo_element_style_deferred(to_underlying(pseudo_element), !observable);
    for (auto const& input : inputs)
        record_highlight_style_inputs(*this, input.root, input.range.ptr());
}

void Document::update_highlight_style_observability()
{
    update_highlight_style_observability(CSS::PseudoElement::Selection);
    update_highlight_style_observability(CSS::PseudoElement::SearchText);
}

void Document::drain_style_transaction_that_flew(Layout::BegunRead const& read)
{
    m_has_flown_style_transaction = false;
    CSS::update_style<CSS::FirstStyleTransaction::Flown>(read, *this);
}

void Document::update_style()
{
    // The host's own read: the style transaction that flew lands for it.
    Layout::ForcedReadScope read { style_computer().style_engine().render_document(), false };
    drain_flown_style_transaction(read);
    update_highlight_style_observability();
    CSS::update_style(read, *this);
}

bool Document::let_style_update_fly(Layout::RustFFI::FfiFlightBlocker blocker)
{
    if (blocker != Layout::RustFFI::FfiFlightBlocker::None || m_has_flown_style_transaction)
        return false;
    // Gathering what the transaction flies with, and sealing the layout round that flies after it, is the host's own
    // read of the render state, which no frame flies beside yet.
    Layout::ForcedReadScope read { *this, false };
    update_highlight_style_observability();
    m_has_flown_style_transaction = CSS::let_style_update_fly(read, *this, blocker);
    return m_has_flown_style_transaction;
}

bool Document::update_style_for_element(AbstractElement const& abstract_element)
{
    Layout::ForcedReadScope read { *this, false };
    return update_style_for_element(abstract_element, StyleUpdateMode::Normal);
}

bool Document::update_style_for_element(AbstractElement const& abstract_element, StyleUpdateMode mode)
{
    // A script API reads the element's style: its waits for the render state are one forced read.
    Layout::ForcedReadScope read { style_computer().style_engine().render_document(), true };
    drain_flown_style_transaction(read);
    update_highlight_style_observability();
    flush_throttled_animation_style_update_for_node(abstract_element.element());
    return CSS::update_style_for_element(read, *this, abstract_element, mode);
}

}
