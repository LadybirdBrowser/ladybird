/*
 * Copyright (c) 2018-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Debug.h>
#include <AK/Format.h>
#include <AK/HashTable.h>
#include <AK/ScopeGuard.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/MainEffects.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleUpdateInputs.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/PseudoElement.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/SpellingMarkers.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::CSS {

using StyleUpdateMode = DOM::Document::StyleUpdateMode;

extern "C" void ladybird_utf16_fly_string_unref(size_t);

// The time since `started_at`, for the timing counters of style updates.
static u64 microseconds_since(MonotonicTime started_at)
{
    return (MonotonicTime::now() - started_at).to_microseconds();
}

// One style update of a document: its style computation, the style record views it reads, and the strings the engine
// lets go of meanwhile, whose release waits for the update to end.
class StyleUpdateScope {
    AK_MAKE_NONCOPYABLE(StyleUpdateScope);
    AK_MAKE_NONMOVABLE(StyleUpdateScope);

public:
    explicit StyleUpdateScope(StyleComputer const& style_computer)
        : m_style_computer(style_computer)
    {
        StyleValueFFI::rust_style_ffi_complete_style_update_begin();
        m_style_computer->begin_style_update();
        m_style_computer->begin_style_record_view_epoch();
    }

    ~StyleUpdateScope()
    {
        m_style_computer->end_style_record_view_epoch();
        m_style_computer->end_style_update();
        auto releases = StyleValueFFI::rust_style_ffi_complete_style_update_end();
        ScopeGuard clear_releases = StyleValueFFI::rust_deferred_cpp_releases_clear;
        for (size_t i = 0; i < releases.fly_string_count; ++i)
            ladybird_utf16_fly_string_unref(releases.fly_strings[i]);
    }

private:
    GC::Ref<StyleComputer const> m_style_computer;
};

// The application of style reactions the style engine decides to a document's elements, and the invalidation of the
// document they accumulate.
struct StyleReactionApplication {
    Layout::BegunRead const& read;
    GC::Ref<DOM::Document> document;
    RequiredInvalidationAfterStyleChange invalidation {};
};

enum class DocumentWithoutBrowsingContext {
    Skip,
    Update,
};
// Where a style update's first transaction comes from: what was recorded since the last transaction, whose inputs the
// update brings up to date first, unless the document has no browsing context to render it...
struct RecordedStyleUpdate {
    DocumentWithoutBrowsingContext document_without_browsing_context { DocumentWithoutBrowsingContext::Skip };
};
// ...or the transaction that flew beside the event loop, whose reactions are drained against the inputs it was sealed
// with.
struct FlownStyleUpdate {
    StyleUpdateInputs sealed_inputs;
};

template<typename FirstTransaction = RecordedStyleUpdate>
static void update_style(Layout::BegunRead const&, DOM::Document&, FirstTransaction = {});
static bool update_style_for_element(Layout::BegunRead const&, DOM::Document&, DOM::AbstractElement const&, StyleUpdateMode);
static bool embedding_document_chain_has_no_pending_style_or_layout_work(DOM::Document const&);

static void apply_element_style_invalidation_after_style_change(Layout::BegunRead const& read, DOM::Element& element, RequiredInvalidationAfterStyleChange const& invalidation)
{
    if (invalidation.accumulated_visual_contexts() == AccumulatedVisualContextInvalidation::UpdateValues)
        element.document().schedule_accumulated_visual_context_update(read, element, DOM::Document::AccumulatedVisualContextUpdateScope::Values);
    else if (invalidation.accumulated_visual_contexts() == AccumulatedVisualContextInvalidation::Rebuild)
        element.document().schedule_accumulated_visual_context_update(read, element, DOM::Document::AccumulatedVisualContextUpdateScope::Structure);

    if (invalidation.needs_scroll_container_resnap) {
        element.document().schedule_scroll_container_resnap();
    }

    // The render state installed the element's record in its box ahead of this install, and marked the relayout, or the
    // layout tree rebuild, the move asks for, which a frame's layout round, or the host's next one, runs.
    bool const installed_by_render_state = StyleEngineFFI::style_engine_render_state_installed(element.document().style_computer().style_engine().host(), element.style_node_id().value(), element.style_record_identity().value());
    // NB: A read of the style of an element below one that is not displayed installs it with no box to move, and leaves
    //     layout as it was for what reads it next.
    if (auto parent = element.element_to_inherit_style_from({}); !installed_by_render_state && parent && has_flag(parent->installed_style().dependency_flags(), StyleRecordDependencyFlag::InDisplayNoneSubtree) && !element.unsafe_layout_node(read))
        return;
    if (auto* arena = element.document().layout_node_arena_if_created(); arena && invalidation.needs_relayout() && !installed_by_render_state) {
        dbgln_if(UPDATE_LAYOUT_DEBUG, "NEED LAYOUT {}", DOM::to_string(DOM::SetNeedsLayoutReason::StyleChange));
        Layout::RustFFI::render_state_relayout_element_after_style_change(arena->host(), element.style_node_id().value(), invalidation.needs_layout_tree_rebuild());
        Painting::request_document_repaint(element.document(), InvalidateDisplayList::No);
    }
    if (invalidation.needs_layout_tree_rebuild()) {
        if (installed_by_render_state) {
            // NB: The layout tree build the render state marked is a rendering update's to run.
            Painting::request_document_repaint(element.document(), InvalidateDisplayList::No);
        } else {
            element.set_needs_layout_tree_rebuild(read, DOM::SetNeedsLayoutTreeUpdateReason::StyleChange, invalidation.layout_tree_rebuild_root());
        }
    } else if (invalidation.white_space_collapse_changed) {
        element.set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::WhitespaceTextBoxChange);
    }
}

static void apply_document_style_invalidation_after_style_change(DOM::Document& document, RequiredInvalidationAfterStyleChange const& invalidation)
{
    if (invalidation.cursor_changed)
        document.set_cursor_may_be_stale();
    if (!invalidation.needs_repaint())
        return;
    if (invalidation.invalidates_hit_test_display_list())
        document.set_needs_to_record_display_list();
    else
        document.set_needs_to_record_display_list_keeping_hit_test_display_list();
}

// One element's computed style answers for another only while the inputs it was keyed on still mean what they meant. A
// transaction boundary is exactly where they stop doing so: a declaration keyed on by identity may have been edited,
// and a sheet may have come or gone. The engine resolves custom properties against the registrations in force, and an
// @property rule registers through this cache. A registration change moves what every environment an element holds
// hands down, which the engine is told again before the transaction derives records under them.
static void prepare_for_style_engine_transaction(Layout::BegunRead const& read, DOM::Document& document)
{
    document.build_registered_properties_cache_for_style_update();
    document.republish_custom_property_environments_for_style_update(read);
    document.style_computer().sweep_custom_property_environments();
}

// Runs the next style update of this update_style() on the document's render state: the writes recorded since the last
// one, or the update already computed beside the event loop where `flown`. The engine restyles what those writes can
// reach, and the application takes the rows it answers.
void StyleEngineFFI::web_css_run_style_update(StyleReactionApplication* application, bool flown)
{
    auto const& read = application->read;
    auto& document = *application->document;
    auto& style_engine = document.style_computer().style_engine();
    auto& counters = document.style_invalidation_counters();
    StyleEngine::StyleUpdateTimings transaction {};
    if (flown) {
        transaction = style_engine.drain_flown_style_update(read);
    } else {
        auto transaction_setup_started_at = MonotonicTime::now();
        ++counters.style_engine_transaction_setups;
        prepare_for_style_engine_transaction(read, document);
        auto setup_microseconds = microseconds_since(transaction_setup_started_at);
        counters.style_engine_transaction_setup_microseconds += setup_microseconds;
        counters.style_update_submission_microseconds += setup_microseconds;
        auto* root = document.document_element();
        if (!root || root->style_node_id() == 0) {
            style_engine.flush();
            return;
        }
        transaction = style_engine.run_style_update(read, root->style_node_id());
    }
    counters.style_update_submission_microseconds += transaction.submission_microseconds;
    counters.style_update_bridge_microseconds += transaction.bridge_microseconds;
}

bool StyleEngineFFI::web_css_has_pending_style_update(StyleReactionApplication* application)
{
    auto& style_engine = application->document->style_computer().style_engine();
    style_engine.note_waiting_arrivals_if_keys_moved(application->read);
    return style_engine.has_pending_transaction(application->read);
}

void StyleEngineFFI::web_css_note_style_update_has_reactions(StyleReactionApplication* application)
{
    application->document->note_style_stabilization_has_style_reactions();
}

void StyleEngineFFI::web_css_sample_animations_for_style_update(StyleReactionApplication* application)
{
    application->document->sample_animation_effects_needing_style_update();
}

// A pass of the current style change event. The outer stabilization epoch advanced the transition generation once,
// before any style, animation, or layout feedback ran.
void StyleEngineFFI::web_css_begin_style_update_pass(StyleReactionApplication* application, bool first)
{
    if (first)
        application->document->build_registered_properties_cache_for_style_update();
    application->document->record_style_stabilization_pass(application->read);
}

// The host samples an element's animations over the record the engine computed once it is installed, as an animation
// update samples them over the record an element holds, and publishes what they compose.
static void sample_animations_for_installed_record(DOM::AbstractElement abstract_element)
{
    auto const& style = abstract_element.installed_style();
    if (!style)
        return;
    Animations::AnimationUpdateContext::ElementData element_data { style, abstract_element.document().style_computer().reconstruct_computed_properties_for_animation(style) };
    element_data.base_is_current = true;
    Animations::AnimationUpdateContext context;
    context.elements.set(abstract_element, move(element_data));
    context.publish();
}

// No style update samples the animations of an element that is not displayed, nor those of its pseudo-elements. A read
// below it samples them for itself, over the styles they hold.
void StyleEngineFFI::web_css_sample_undisplayed_animations_for_read(StyleReactionApplication* application, DOM::Element* element)
{
    auto& style_computer = application->document->style_computer();
    auto sample = [&](DOM::AbstractElement abstract_element) {
        auto const& style = abstract_element.installed_style();
        if (!style || (!style_computer.has_animations_to_compose(abstract_element) && !style.view().animated_overlay && !abstract_element.custom_property_data_is_animation_overlay()))
            return;
        if (element->has_inclusive_ancestor_with_display_none_ignoring_animations())
            sample_animations_for_installed_record(abstract_element);
    };
    sample(DOM::AbstractElement { *element });
    for (auto pseudo_element : DOM::Element::computed_pseudo_elements)
        sample(DOM::AbstractElement { *element, pseudo_element });
}

// What the style engine reads of an element it applies a reaction to.
static StyleEngineFFI::FfiReactionElement describe_reaction_element(DOM::Document const& document, DOM::Element* element)
{
    if (!element)
        return {};
    auto const* box_values = element->style_group<ComputedValues::BoxValues>();
    return {
        .element = element,
        .style_node = element->style_node_id().value(),
        .connected = element->is_connected() && &element->document() == &document,
        .has_style = element->has_style(),
        .style_record = element->style_record_identity().value(),
        .display_none = box_values && display_from_ffi_display(box_values->display).is_none(),
    };
}

StyleEngineFFI::FfiReactionElement StyleEngineFFI::web_css_style_reaction_element(StyleReactionApplication* application, u32 style_node)
{
    return describe_reaction_element(*application->document, application->document->style_computer().element_for_style_node(style_node).ptr());
}

StyleEngineFFI::FfiReactionElement StyleEngineFFI::web_css_style_inheritance_parent(StyleReactionApplication* application, DOM::Element* element)
{
    return describe_reaction_element(*application->document, const_cast<DOM::Element*>(element->element_to_inherit_style_from({}).ptr()));
}

// Swaps in the styles of a style installation, which the render side installed in the layout arena, and performs
// the main-side effects of the elements whose styles move where they have any. Both are in tree order, and each
// element's effects follow its swaps, which they name: an element's effects can clear the styles of its descendants,
// whose swaps come after them.
void StyleEngineFFI::web_css_install_styles(StyleReactionApplication* application, StyleSwap const* swaps, size_t swap_count, InstalledElementEffects const* effects, size_t effect_count, u32 document_damage)
{
    auto const& read = application->read;
    auto& document = *application->document;
    auto& style_computer = document.style_computer();
    auto element_for = [&](u32 style_node) -> DOM::Element* {
        auto element = style_computer.element_for_style_node(StyleNodeID { style_node });
        if (!element || !element->is_connected() || &element->document() != &document)
            return nullptr;
        return element.ptr();
    };
    style_computer.begin_style_installation();
    ScopeGuard end_style_installation = [&] { style_computer.end_style_installation(); };

    ReadonlySpan<StyleSwap> all_swaps { swaps, swap_count };
    size_t next_swap = 0;
    // Swaps in the styles up to the swap `end`, each element's at once.
    auto swap_in_styles_until = [&](size_t end) {
        VERIFY(end <= all_swaps.size());
        while (next_swap < end) {
            auto const style_node = all_swaps[next_swap].style_node;
            auto* element = element_for(style_node);
            auto const held_before = element && element->has_style() ? element->installed_style() : InstalledStyle {};
            for (; next_swap < end && all_swaps[next_swap].style_node == style_node; ++next_swap) {
                if (!element)
                    continue;
                auto const& swap = all_swaps[next_swap];
                Optional<PseudoElement> pseudo_element;
                if (swap.slot != NumericLimits<u8>::max())
                    pseudo_element = static_cast<PseudoElement>(swap.slot);
                if (pseudo_element == PseudoElement::Highlight) {
                    element->swap_in_custom_highlight_style_record(StyleAtomID { swap.highlight_name }, swap.record);
                    continue;
                }
                // A pseudo-element's transitions may start in a later pass too.
                if (pseudo_element.has_value()) {
                    DOM::AbstractElement abstract_element { *element, pseudo_element };
                    if (auto const held = abstract_element.installed_style())
                        style_computer.record_transition_baseline_for_later_passes(abstract_element, held);
                }
                element->swap_in_style_record(read, pseudo_element, swap.record);
            }
            // A later pass of this style update may start the element's transitions, against the style the element held
            // before the update rather than this record.
            if (held_before && held_before != element->installed_style())
                style_computer.record_transition_baseline_for_later_passes({ *element }, held_before);
        }
    };

    // NB: No element holds children waiting to arrive where the document has none below it.
    auto const reveal_waiting = document.descendant_style_arrival_pending() ? CSS::MainEffects::RevealWaiting : CSS::MainEffects::None;
    for (auto const& element_effects : ReadonlySpan<InstalledElementEffects> { effects, effect_count }) {
        auto main_effects = static_cast<CSS::MainEffects>(element_effects.effects);
        main_effects &= ~CSS::MainEffects::RevealWaiting | reveal_waiting;
        // NB: The swaps of an element with nothing else to do come with the next element's that has.
        if (main_effects == CSS::MainEffects::None && element_effects.pseudo_elements_composed == 0)
            continue;
        swap_in_styles_until(element_effects.swaps_begin);
        auto* element = element_for(element_effects.style_node);
        // https://drafts.csswg.org/css-transitions-1/#starting
        // The transition step compares the style the element held before its swaps with the record they install, so the
        // record it held has to outlive its replacement until the step has read it. A step toward a record that declares
        // no transitions still ends the ones the element runs. An element running transitions has animations, which the
        // host composes.
        bool const owes_a_transition_step = element && element->has_style() && has_flag(main_effects, CSS::MainEffects::Animated)
            && (has_flag(main_effects, CSS::MainEffects::TransitionStep) || element->has_existing_transitions({}));
        InstalledStyle const before_change = owes_a_transition_step ? element->installed_style() : InstalledStyle {};
        // So does that of a pseudo-element.
        Array<InstalledStyle, DOM::Element::computed_pseudo_elements.size()> pseudo_element_before_change {};
        if (element) {
            for (size_t i = 0; i < pseudo_element_before_change.size(); ++i) {
                auto pseudo_element = DOM::Element::computed_pseudo_elements[i];
                if ((element_effects.pseudo_element_transition_steps >> to_underlying(pseudo_element)) & 1)
                    pseudo_element_before_change[i] = element->installed_style(pseudo_element);
            }
        }
        // The effects compare the record the element moves to with the one it moves from, which the installation pins
        // while the host applies it.
        InstalledStyle const old_style { element_effects.old_record };
        swap_in_styles_until(element_effects.swaps_end);

        if (!element || !element->has_style())
            continue;
        element->note_subtree_style_moved();
        auto invalidation = decode_style_invalidation(element_effects.damage);
        // A change of the element's display applies once the record holds the element's animations and its transition
        // step has run.
        Optional<DOM::Element::DisplayNoneState> display_none_before;
        if (has_flag(main_effects, CSS::MainEffects::DisplayNoneFlip) && old_style)
            display_none_before = DOM::Element::DisplayNoneState::of(*style_computer.computed_style_record_view(old_style), old_style);
        element->apply_style_main_effects(read, main_effects, static_cast<CSS::LayoutTreeRebuildRoot>(element_effects.rebuild_root), invalidation, old_style);
        if (has_flag(main_effects, CSS::MainEffects::HostBoxFacts))
            apply_element_style_invalidation_after_style_change(read, *element, invalidation);
        // https://drafts.csswg.org/css-transitions-2/#defining-before-change-style
        // The host composes the element's animations over the record it installed, and its pseudo-elements, which
        // inherit the composition, after it.
        if (has_flag(main_effects, CSS::MainEffects::Animated)) {
            DOM::AbstractElement abstract_element { *element };
            if (has_flag(main_effects, CSS::MainEffects::AnimationPlan))
                style_computer.apply_settled_animation_plan(read, abstract_element);
            if (old_style && document.is_in_style_stabilization_epoch())
                style_computer.record_transition_stabilization_baseline(abstract_element, old_style);
            StyleEngineFFI::style_engine_begin_composition_settling(style_computer.style_engine().host(), &read, element->style_node_id().value());
            if (style_computer.has_animations_to_compose(abstract_element))
                sample_animations_for_installed_record(abstract_element);
            else
                style_computer.stop_publishing_animated_custom_properties(abstract_element);
            if (owes_a_transition_step && before_change) {
                auto step_invalidation = style_computer.run_transition_step_for_installed_record(read, abstract_element, before_change);
                apply_element_style_invalidation_after_style_change(read, *element, step_invalidation);
                application->invalidation |= step_invalidation;
            }
            StyleEngineFFI::style_engine_end_composition_settling(style_computer.style_engine().host(), &read);
        }
        // The host composes the records swapped in for the element's pseudo-elements that owe the plan of their
        // animations or the transition step, or whose element has animations, as it composes those it settles.
        if (element_effects.pseudo_elements_composed != 0) {
            application->invalidation |= element->compose_style_pseudo_element_records(read, element_effects.pseudo_elements_composed, element_effects.pseudo_element_transition_steps, pseudo_element_before_change);
        }
        if (display_none_before.has_value()) {
            element->apply_display_none_change(*display_none_before);
            // The animations and transitions the change terminated compose no more over the record.
            if (element->installed_style().view().animated_overlay || element->custom_property_data_is_animation_overlay({}))
                sample_animations_for_installed_record(DOM::AbstractElement { *element });
        }
        // The render side decided whether the element's descendants hold styles while it is hidden.
        if (has_flag(main_effects, CSS::MainEffects::ClearHiddenDescendants))
            element->clear_computed_styles_from_display_none_descendants(read);
        if (has_flag(main_effects, CSS::MainEffects::VisibilityFlip))
            document.throttled_animation_visibility_changed();
    }
    swap_in_styles_until(all_swaps.size());

    auto invalidation = decode_style_invalidation(document_damage);
    // The render side noted the boxes whose visual contexts the moves change, which the document updates ahead of
    // its next paint.
    if (invalidation.accumulated_visual_contexts() != AccumulatedVisualContextInvalidation::None || invalidation.needs_stacking_context_tree_rebuild())
        document.set_needs_accumulated_visual_contexts_update(true);
    if (invalidation.needs_repaint())
        Painting::request_document_repaint(document, invalidation.invalidates_hit_test_display_list() ? InvalidateDisplayList::PaintCommandsAndHitTestList : InvalidateDisplayList::PaintCommands);
    application->invalidation |= invalidation;
}

ContainerLayoutUpToDate lay_out_container_for_style_update(DOM::Document& document)
{
    // NOTE: If our parent document needs a relayout, we must do that *first*. This is required as it may cause the
    // viewport to change which will can affect media query evaluation and the value of the `vw` unit.
    // OPTIMIZATION: A settled embedding chain has no relayout to do, and finding that out by laying it out publishes
    //               its whole style environment.
    if (auto navigable = document.navigable(); navigable && navigable->container() && &navigable->container()->document() != &document
        && !embedding_document_chain_has_no_pending_style_or_layout_work(document))
        navigable->container()->document().update_layout(DOM::UpdateLayoutReason::ChildDocumentStyleUpdate);
    return {};
}

Optional<StyleUpdateInputs> begin_style_update_inputs(Layout::BegunRead const& read, DOM::Document& document, ContainerLayoutUpToDate)
{
    auto submission_started_at = MonotonicTime::now();
    document.synchronize_dirty_style_attributes();

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
    document.style_invalidation_counters().style_update_submission_microseconds += microseconds_since(submission_started_at);
    // Settle the counter-style registry of each tree scope that may have moved before the engine answers any row: a
    // record naming a counter style names the registry it was computed against, and a shadow tree can define its own.
    // None of it depends on layout.
    if (document.counter_style_scopes_need_update())
        document.settle_counter_style_scopes(read);
    if (document.has_completed_style_update()
        && !document.style_computer().style_engine().has_pending_transaction(read)) {
        if (!document.needs_animated_style_update())
            return {};
        document.sample_animation_effects_needing_style_update();
        if (!document.style_computer().style_engine().has_pending_transaction(read))
            return {};
    }

    return StyleUpdateInputs {};
}

static bool document_has_style_work(Layout::BegunRead const&, DOM::Document const&);

template<typename FirstTransaction>
static void update_style(Layout::BegunRead const& read, DOM::Document& document, FirstTransaction first_transaction)
{
    // NB: The style update of a read with no transaction in flight is compiled without the drain's steps.
    constexpr bool drains_flown_transaction = IsSame<FirstTransaction, FlownStyleUpdate>;
    auto style_update_started_at = MonotonicTime::now();
    auto& timing_counters = document.style_invalidation_counters();
    auto const submission_before = timing_counters.style_update_submission_microseconds;
    auto const bridge_before = timing_counters.style_update_bridge_microseconds;
    auto const apply_before = timing_counters.style_update_apply_microseconds;
    ScopeGuard record_style_update_time = [&] {
        auto whole = microseconds_since(style_update_started_at);
        auto measured = timing_counters.style_update_submission_microseconds - submission_before
            + timing_counters.style_update_bridge_microseconds - bridge_before
            + timing_counters.style_update_apply_microseconds - apply_before;
        timing_counters.style_update_microseconds += whole;
        // NB: Counters are read only to attribute time, never to choose style work. The
        //     intervals are disjoint; rounding each down leaves fractional time here too.
        timing_counters.style_update_remainder_microseconds += whole - measured;
    };
    // NB: The drain of a transaction that flew reads the container's layout its seal read.
    [[maybe_unused]] auto container_layout = [&] {
        if constexpr (drains_flown_transaction)
            return Empty {};
        else
            return lay_out_container_for_style_update(document);
    }();
    if constexpr (!drains_flown_transaction) {
        if (!document.browsing_context() && first_transaction.document_without_browsing_context == DocumentWithoutBrowsingContext::Skip)
            return;

        // NOTE: If this is a document hosting <template> contents, style update is unnecessary.
        if (document.created_for_appropriate_template_contents())
            return;

        // NB: A read of an element's style outside any transaction may have resolved fonts against an older table.
        document.style_computer().style_engine().take_in_fonts_resolved_against_older_tables();
    }

    // A document that has no style work returns before anything below begins. Where LIBWEB_STYLE_SELFCHECK is set, it
    // goes on, and the update must find that it has nothing to do: a source of style work that left no trace for the
    // check is a missing flag.
    bool has_no_style_work = false;
    if constexpr (!drains_flown_transaction) {
        has_no_style_work = !document_has_style_work(read, document);
        if (has_no_style_work && !StyleEngineFFI::style_engine_self_check_is_enabled(document.style_computer().style_engine().host()))
            return;
    }

    // What publishes by identity below (animations, the elements prepared for style) finds the nodes that connected
    // since the last update under the identities they take here.
    // NB: What connected beside the transaction that flew is the next transaction's.
    if constexpr (!drains_flown_transaction) {
        take_in_and_publish_pending_style_arrivals(document, HiddenChildArrivals::MayWait, read);
        document.update_highlight_style_observability();
    }
    StyleUpdateScope style_update_scope { document.style_computer() };

    document.begin_style_stabilization_epoch();
    ScopeGuard end_stabilization_epoch = [&] {
        document.end_style_stabilization_epoch();
    };

    // NB: The drain of a transaction that flew installs what was computed from the inputs it was sealed with, and is
    //     due whatever became of the document since: what was recorded beside it is the next transaction's.
    auto inputs = [&] {
        if constexpr (drains_flown_transaction) {
            document.update_style_computer_viewport_rect();
            return Optional<StyleUpdateInputs> { move(first_transaction.sealed_inputs) };
        } else {
            return begin_style_update_inputs(read, document, container_layout);
        }
    }();
    auto const self_check = [&] {
        auto* root = document.document_element();
        StyleEngineFFI::style_engine_self_check(document.style_computer().style_engine().host(), &read, root ? root->style_node_id().value() : 0);
    };
    // NB: An update behind one that flew and left nothing to do is the one that checks it.
    auto const self_check_a_flown_update = [&] {
        if constexpr (!drains_flown_transaction) {
            if (StyleEngineFFI::style_engine_flown_update_awaits_self_check(document.style_computer().style_engine().host()))
                self_check();
        }
    };
    if (!inputs.has_value()) {
        self_check_a_flown_update();
        return;
    }
    if (has_no_style_work) {
        dbgln("Style update found work its document said it had none of");
        VERIFY_NOT_REACHED();
    }

    // The animations the update ticked are sampled before the transaction is taken, as they are ahead of the seal of
    // one that flies: the records it computes inherit their values, and what they feed back joins the transaction.
    if constexpr (!drains_flown_transaction)
        document.sample_animation_effects_needing_style_update();

    // A style flush is a transaction boundary. Everything recorded since the last one crosses into StyleEngine as one
    // batch.
    // What was written beside the transaction that flew reaches the engine once the drain of its reactions has ended,
    // as the next transaction's input.
    ScopeGuard end_flown_style_drain = [&] {
        if constexpr (drains_flown_transaction)
            document.style_computer().style_engine().end_flown_style_drain();
    };
    ScopeGuard discard_style_engine_transaction_outputs = [&] {
        document.style_computer().style_engine().discard_style_update_outputs();
    };
    StyleReactionApplication application { read, document };
    auto applied = StyleEngineFFI::style_engine_update_style(document.style_computer().style_engine().host(), &read, &application, drains_flown_transaction);
    timing_counters.style_engine_reaction_batch_runs += applied.batch_runs;
    timing_counters.style_engine_reaction_elements += applied.reaction_elements;
    timing_counters.style_engine_published_reactions += applied.published_reactions;
    timing_counters.style_update_pass_guard_hits += applied.pass_guard_hits;
    timing_counters.style_update_apply_microseconds += applied.apply_microseconds;
    constexpr auto assigned_slot_changes = drains_flown_transaction ? DOM::Document::AssignedSlotChanges::FlewPast : DOM::Document::AssignedSlotChanges::Recorded;
    document.settle_elements_whose_assigned_slot_changed(assigned_slot_changes, read, [](DOM::Element& element) {
        // The animations a slot change terminated compose no more over the record.
        if (element.installed_style().view().animated_overlay || element.custom_property_data_is_animation_overlay({}))
            sample_animations_for_installed_record(DOM::AbstractElement { element });
    });
    if (!applied.had_reactions) {
        self_check_a_flown_update();
        return;
    }

    document.set_has_completed_style_update();
    apply_document_style_invalidation_after_style_change(document, application.invalidation);
    document.sample_animation_effects_needing_style_update();
    if (auto* arena = document.layout_node_arena_if_created())
        Layout::RustFFI::render_state_refresh_first_letter_styles(arena->host());

    // NB: Where LIBWEB_STYLE_SELFCHECK is set, what the style engine installed is checked against a cold pass once the
    //     update has settled. What was written beside a transaction that flew, such as the fonts that resolve
    //     differently since, reaches the engine only once its drain has ended, as the next update's input: that
    //     update checks. So does the one after an update that stopped at its limit of passes with what they fed back
    //     still pending. An update that stopped with nothing left has settled.
    auto const stopped_with_work_left = applied.pass_guard_hits != 0 && document.style_computer().style_engine().has_pending_transaction(read);
    if (!drains_flown_transaction && !stopped_with_work_left && StyleEngineFFI::style_engine_self_check_is_enabled(document.style_computer().style_engine().host()))
        self_check();
}

// Records what update_style() records before it runs the document's style update, and lets the update fly
// beside the event loop where `blocker` is none: the next update_style() takes its reactions in, once it has landed,
// against the inputs this answers.
static Optional<StyleUpdateInputs> let_style_update_fly(Layout::BegunRead const& read, DOM::Document& document, Layout::RustFFI::FfiFlightBlocker blocker)
{
    if (!document.browsing_context() || document.created_for_appropriate_template_contents())
        return {};
    auto* root = document.document_element();
    if (!root || root->style_node_id() == 0)
        return {};
    auto container_layout = lay_out_container_for_style_update(document);
    auto& style_computer = document.style_computer();
    style_computer.begin_style_update();
    ScopeGuard end_style_update = [&] {
        style_computer.end_style_update();
    };
    auto inputs = begin_style_update_inputs(read, document, container_layout);
    if (!inputs.has_value())
        return {};
    // The animations the update ticked are sampled ahead of the seal, so that their values, and the inherited style
    // they feed back, fly with the transaction.
    document.sample_animation_effects_needing_style_update();
    if (!style_computer.style_engine().has_pending_transaction(read))
        return {};
    prepare_for_style_engine_transaction(read, document);
    if (!style_computer.style_engine().let_style_update_fly(*inputs, root->style_node_id(), blocker))
        return {};
    document.note_style_update_flew_past_assigned_slot_changes();
    return inputs;
}

// The answer of the style engine to a read of an element's style.
static bool answer_style_read(DOM::AbstractElement const& abstract_element, StyleEngineFFI::FfiTargetedStyleAnswer answer)
{
    switch (answer) {
    case StyleEngineFFI::FfiTargetedStyleAnswer::Unstyled:
        return false;
    case StyleEngineFFI::FfiTargetedStyleAnswer::Styled:
        return true;
    case StyleEngineFFI::FfiTargetedStyleAnswer::HeldStyle:
        return abstract_element.has_style();
    case StyleEngineFFI::FfiTargetedStyleAnswer::NeedsStyleUpdate:
        break;
    }
    VERIFY_NOT_REACHED();
}

// Whether take_in_pending_style_arrivals() may find a subtree to take in: one connected since, or children waiting in a
// display:none subtree that the keys they were looked at against moved for.
static bool subtree_arrivals_may_be_due(Layout::BegunRead const& read, DOM::Document const& document)
{
    if (!document.descendant_style_arrival_pending())
        return false;
    auto& style_engine = const_cast<DOM::Document&>(document).style_computer().style_engine();
    return style_engine.has_pending_arrivals()
        || style_engine.waiting_children_checked_against_keys_version() != style_engine.relational_arrival_keys(read).version;
}

// Whether anything is unsettled that a read of a style that an element holds has to wait for. That is all the sources
// of style work in document_has_style_work() but two:
// - What the host does not know the engine would answer, as this asks the engine instead.
// - An element awaiting its first style work: on the chain of elements the read inherits through it holds no style,
//   which sends the read to a style update, and elsewhere it has not told the engine anything a held style could read.
static bool document_has_style_work_a_read_waits_for(Layout::BegunRead const& read, DOM::Document const& document)
{
    auto const& style_computer = document.style_computer();
    auto const& style_engine = style_computer.style_engine();
    // A rootless flush drains the pending inputs but preserves element style inputs for the first transaction with a
    // document root, so has_pending_transaction() alone would report a settled engine still owing an element its
    // recomputation.
    return !document.has_completed_style_update()
        || style_engine.has_recorded_input()
        || style_engine.has_flown_style_update()
        || subtree_arrivals_may_be_due(read, document)
        || document.has_assigned_slot_change_notes()
        || document.has_dirty_style_attributes()
        || document.has_unreported_style_attributes_to_report()
        || document.needs_highlight_style_update()
        || document.needs_media_rule_evaluation()
        || document.needs_animated_style_update()
        || document.counter_style_scopes_need_update()
        || style_computer.viewport_rect_for_style_environment() != document.viewport_rect()
        || !non_author_stylesheets_are_recorded(document)
        || style_engine.has_pending_transaction(read)
        || StyleEngineFFI::style_engine_has_deferred_element_style_inputs(style_engine.host(), &read);
}

// Whether anything the document's style depends on may have moved since its last style update. Every source of style
// work leaves a flag, an entry in a set, or a fact the style engine reports as it is recorded, so the answer takes no
// walk of the document and no job on its render state: a style update that finds none of them has nothing to do, and
// returns before it begins.
static bool document_has_style_work(Layout::BegunRead const& read, DOM::Document const& document)
{
    // NB: Writes queued since the last job may have moved what the engine answers of itself, which only a job on the
    //     render state finds out. They are style work as far as this check knows, and the update that follows asks.
    auto const& style_engine = document.style_computer().style_engine();
    return !StyleEngineFFI::style_engine_knows_engine_facts(style_engine.host())
        || style_engine.has_elements_awaiting_initial_style_work()
        || document_has_style_work_a_read_waits_for(read, document);
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
        Layout::ForcedReadScope embedding_read { embedding_document };
        if (document_has_style_work_a_read_waits_for(embedding_read, embedding_document)
            || !embedding_document.layout_is_up_to_date()
            || !container->has_style())
            return false;
        embedded_document = &embedding_document;
    }
    return true;
}

// The elements an element inherits its style through, from the element itself up to the root. Pseudo-element styles are
// refreshed when the originating element is recomputed, so the chain of a pseudo-element starts at that.
static StyleEngineFFI::FfiReactionElement first_element_of_inheritance_chain(DOM::AbstractElement const& abstract_element)
{
    auto& element = const_cast<DOM::Element&>(abstract_element.element());
    if (abstract_element.pseudo_element().has_value())
        return describe_reaction_element(element.document(), const_cast<DOM::Element*>(element.element_to_inherit_style_from(abstract_element.pseudo_element()).ptr()));
    return describe_reaction_element(element.document(), &element);
}

static bool update_style_for_element(Layout::BegunRead const& read, DOM::Document& document, DOM::AbstractElement const& abstract_element, StyleUpdateMode mode)
{
    if (!abstract_element.element().is_connected())
        return false;
    document.ensure_style_engine_tracks_tree();

    // OPTIMIZATION: When nothing style-related is pending anywhere that could affect this document, nothing below
    //               runs a style update, and the scan of the inheritance chain answers the read directly where the
    //               chain has style throughout, or a display:none element stops the read above every element without.
    if (mode != StyleUpdateMode::Normal
        && !abstract_element.pseudo_element().has_value()
        && abstract_element.element().style_node_id() != 0
        && !document_has_style_work_a_read_waits_for(read, document)
        && embedding_document_chain_has_no_pending_style_or_layout_work(document)) {
        StyleReactionApplication application { read, document };
        auto answer = StyleEngineFFI::style_engine_read_settled_inheritance_chain(document.style_computer().style_engine().host(), &read, &application, first_element_of_inheritance_chain(abstract_element), static_cast<StyleEngineFFI::FfiStyleUpdateMode>(mode));
        if (answer != StyleEngineFFI::FfiTargetedStyleAnswer::NeedsStyleUpdate)
            return answer_style_read(abstract_element, answer);
    }

    // A read of an element in a display:none subtree may be answered by the children waiting to arrive there.
    auto const& read_element = abstract_element.element();
    auto read_may_need_waiting_children = document.descendant_style_arrival_pending()
        && read_element.has_inclusive_ancestor_with_display_none_ignoring_animations();
    take_in_and_publish_pending_style_arrivals(document, read_may_need_waiting_children ? HiddenChildArrivals::TakeIn : HiddenChildArrivals::MayWait, read);
    document.update_highlight_style_observability();
    StyleUpdateScope style_update_scope { document.style_computer() };
    // Refresh computed properties for an abstract element. An ordinary read first consumes the complete exact
    // reaction batch. A reentrant layout read leaves that transaction untouched and walks the flat-tree inheritance
    // chain, re-cascading from the rootmost stale element on the path back down to the target.

    bool embedding_document_layout_was_stale = false;
    // OPTIMIZATION: An embedding chain with no style or layout work anywhere has nothing to bring up to date, and a
    //               style and layout update of a settled document still publishes its whole environment to find that
    //               out. A page that focuses elements in a frame asks this once per focus.
    if (auto navigable = document.navigable(); navigable && navigable->container() && &navigable->container()->document() != &document
        && !embedding_document_chain_has_no_pending_style_or_layout_work(document)) {
        auto& container = *navigable->container();
        auto& embedding_document = container.document();
        // The container's style is the embedding document's own read, in which the style update that flew for the
        // embedding document lands first.
        Layout::ForcedReadScope embedding_read { embedding_document };
        embedding_document.drain_flown_style_update(embedding_read);
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
        if (!can_run_regular_style_update)
            document.sample_animation_effects_needing_style_update();
        // NB: A style update stops after a number of passes with what they fed back still pending, as a chain of
        //     custom property animations does that moves one level per pass. The read wants them all.
        auto const stopped_passes = [&] { return document.style_invalidation_counters().style_update_pass_guard_hits; };
        auto stopped_before = stopped_passes();
        bool update = !document.is_running_update_layout()
            && (can_run_regular_style_update || document.style_computer().style_engine().has_pending_transaction(read));
        while (update) {
            update_style(read, document, RecordedStyleUpdate { DocumentWithoutBrowsingContext::Update });
            ran_regular_style_update = true;
            update = stopped_passes() != exchange(stopped_before, stopped_passes())
                && document.style_computer().style_engine().has_pending_transaction(read)
                && !document.is_at_style_stabilization_bound(read);
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
        auto const& style = abstract_element.installed_style();
        // NB: An element moved below a display:none element holds the record it had where it stood.
        if (style && !has_flag(style.dependency_flags(), StyleRecordDependencyFlag::InDisplayNoneSubtree)
            && !abstract_element.element().has_inclusive_ancestor_with_display_none_ignoring_animations())
            return true;
    }

    StyleReactionApplication application { read, document };
    document.style_computer().begin_inheritance_chain_read();
    ScopeGuard end_inheritance_chain_read = [&] { document.style_computer().end_inheritance_chain_read(); };
    auto answer = StyleEngineFFI::style_engine_update_style_for_inheritance_chain(document.style_computer().style_engine().host(), &read, &application, first_element_of_inheritance_chain(abstract_element), static_cast<StyleEngineFFI::FfiStyleUpdateMode>(mode), embedding_document_layout_was_stale);
    apply_document_style_invalidation_after_style_change(document, application.invalidation);
    return answer_style_read(abstract_element, answer);
}

}

namespace Web::DOM {

void Document::drain_style_update_that_flew(Layout::BegunRead const& read)
{
    // The animations dirtied beside the transaction are the next style update's, as are the writes made beside it: the
    // drain samples those its own reactions create.
    auto effects_beside = exchange(m_effects_needing_animated_style_update, {});
    bool needed_beside = exchange(m_needs_animated_style_update, false);
    bool forced_beside = exchange(m_force_throttled_animation_style_update, false);
    CSS::update_style(read, *this, CSS::FlownStyleUpdate { m_flown_style_update_inputs.release_value() });
    for (auto& effect : effects_beside)
        m_effects_needing_animated_style_update.set(effect);
    m_needs_animated_style_update |= needed_beside;
    m_force_throttled_animation_style_update |= forced_beside;
}

void Document::update_style()
{
    Layout::ForcedReadScope read { style_computer().style_engine().render_document() };
    drain_flown_style_update(read);
    auto const stopped_passes = [&] { return style_invalidation_counters().style_update_pass_guard_hits; };
    auto stopped_before = stopped_passes();
    CSS::update_style(read, *this);
    if (stopped_passes() == stopped_before)
        return;

    // NB: A style update stops after a number of passes with what they fed back still pending, as a chain of custom
    //     property animations does that moves one level per pass. What reads the style next wants them all, such as a
    //     tree build demanding a record, which reads current input only.
    begin_style_stabilization_epoch();
    ScopeGuard end_stabilization_epoch = [&] { end_style_stabilization_epoch(); };
    stopped_before = stopped_passes();
    while (style_computer().style_engine().has_pending_transaction(read) && !is_at_style_stabilization_bound(read)) {
        CSS::update_style(read, *this);
        if (stopped_passes() == exchange(stopped_before, stopped_passes()))
            break;
    }
}

bool Document::let_style_update_fly(Layout::RustFFI::FfiFlightBlocker blocker)
{
    if (blocker != Layout::RustFFI::FfiFlightBlocker::None || m_flown_style_update_inputs.has_value())
        return false;
    Layout::ForcedReadScope read { *this };
    update_highlight_style_observability();
    m_flown_style_update_inputs = CSS::let_style_update_fly(read, *this, blocker);
    return m_flown_style_update_inputs.has_value();
}

bool Document::update_style_for_element(AbstractElement const& abstract_element)
{
    return update_style_for_element(abstract_element, StyleUpdateMode::Normal);
}

bool Document::update_style_for_element(AbstractElement const& abstract_element, StyleUpdateMode mode)
{
    Layout::ForcedReadScope read { style_computer().style_engine().render_document() };
    drain_flown_style_update(read);
    update_highlight_style_observability();
    flush_throttled_animation_style_update_for_node(abstract_element.element());
    return CSS::update_style_for_element(read, *this, abstract_element, mode);
}

}
