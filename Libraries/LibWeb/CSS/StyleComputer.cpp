/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/BinarySearch.h>
#include <AK/Bitmap.h>
#include <AK/BuiltinWrappers.h>
#include <AK/Debug.h>
#include <AK/Error.h>
#include <AK/Find.h>
#include <AK/FixedBitmap.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonObject.h>
#include <AK/Math.h>
#include <AK/NeverDestroyed.h>
#include <AK/NonnullRawPtr.h>
#include <AK/QuickSort.h>
#include <AK/ScopeGuard.h>
#include <AK/Utf8View.h>
#include <LibGC/Heap.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Animations/ScrollTimeline.h>
#include <LibWeb/Animations/ViewTimeline.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/ContainerQuery.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/FontFace.h>
#include <LibWeb/CSS/FontFaceState.h>
#include <LibWeb/CSS/HypotheticalElement.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/StyleComputeFFI.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineEffectTiming.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PendingSubstitutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/SelectorQuery.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/MathML/MathMLElement.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Page/RenderingStats.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/ValueParserRustFFI.h>
#include <math.h>

namespace Web::CSS {

class Fnv1a64 {
public:
    void add(u64 value)
    {
        m_hash ^= value;
        m_hash *= 0x100000001b3ull;
    }

    u64 value() const { return m_hash; }

private:
    u64 m_hash { 0xcbf29ce484222325ull };
};

GC_DEFINE_ALLOCATOR(StyleComputer);

StyleComputer::StyleComputer(DOM::Document& document)
    : m_document(document)
    , m_default_font_metrics(16, Platform::FontPlugin::the().default_font(16)->pixel_metrics(), normal_line_height(Platform::FontPlugin::the().default_font(16)->pixel_metrics()))
    , m_root_element_font_metrics(m_default_font_metrics)
    , m_style_engine(this)
{
}

void StyleComputer::prepare_for_style_engine_transaction() const
{
    sweep_custom_property_environments();
}

void StyleComputer::begin_style_update() const
{
    ++m_style_update_depth;
    begin_deferred_web_face_loads();
}

void StyleComputer::end_style_update() const
{
    VERIFY(m_style_update_depth > 0);
    // Loading a web face a style selected runs author callbacks and starts a fetch, so it waits for the outermost
    // update to finish.
    ScopeGuard load_deferred_web_faces = [] { end_deferred_web_face_loads(); };
    if (--m_style_update_depth != 0)
        return;
    m_style_update_ffi_media_environment.clear();
    m_style_update_media_environment.clear();
}

Parser::ValueParserFFI::FfiMediaEnvironment const* StyleComputer::ensure_media_environment_for_style_update() const
{
    // NB: Outside a style update there is nothing to clear the cached snapshot, so always take a
    //     fresh one. Every style-computation entry point currently opens a scope, so this is a
    //     defensive path rather than one the engine relies on.
    if (m_style_update_depth == 0 || !m_style_update_media_environment.has_value()) {
        m_style_update_media_environment.emplace(m_document);
        m_style_update_ffi_media_environment = m_style_update_media_environment->ffi_environment();
    }
    return &*m_style_update_ffi_media_environment;
}

ComputedStyleRecordView StyleComputer::computed_style_record_view(InstalledStyle const& style) const
{
    if (!style)
        return {};
    // NB: The view holds the style it reads, so it reads it however long it lives.
    return ComputedStyleRecordView { style };
}

InstalledStyle StyleComputer::install_style(Layout::BegunRead const& read, StyleRecordID style_record) const
{
    if (!style_record)
        return {};
    return InstalledStyle { m_style_engine.host_record(read, style_record) };
}

void StyleComputer::begin_style_installation()
{
    if (m_style_installation_depth++ == 0)
        m_installation_subtree_style_generation = ++m_subtree_style_generation;
}

void StyleComputer::end_style_installation()
{
    VERIFY(m_style_installation_depth > 0);
    if (--m_style_installation_depth == 0)
        m_custom_property_environments_noted_during_installation.clear();
}

u64 StyleComputer::subtree_style_generation_for_move()
{
    if (m_style_installation_depth > 0)
        return m_installation_subtree_style_generation;
    return ++m_subtree_style_generation;
}

bool StyleComputer::custom_property_environment_needs_note(CustomPropertyData const& environment) const
{
    if (m_style_installation_depth == 0)
        return true;
    // NB: The table holds what it names, so no other environment can take an identity it remembers.
    return m_custom_property_environments_noted_during_installation.set(environment.identity(), environment) == HashSetResult::InsertedNewEntry;
}

void StyleComputer::begin_style_record_view_epoch() const
{
    if (m_style_record_view_epoch_depth++ == 0)
        StyleEngineFFI::style_engine_begin_style_record_view_epoch(m_style_engine.host());
}

void StyleComputer::end_style_record_view_epoch() const
{
    VERIFY(m_style_record_view_epoch_depth > 0);
    if (--m_style_record_view_epoch_depth == 0)
        StyleEngineFFI::style_engine_end_style_record_view_epoch(m_style_engine.host());
}

void StyleComputer::register_style_node(StyleNodeID style_node_id, DOM::Node& node)
{
    if (style_node_id == 0)
        return;
    ensure_style_node_slot(style_node_id);
    if (style_node_is_text(style_node_id)) {
        m_text_style_nodes[style_node_index(style_node_id)] = as<DOM::Text>(node);
        return;
    }
    m_element_style_nodes[style_node_index(style_node_id)] = node;
    // NB: An HTML element is neither of the kinds below, which most elements are spared asking.
    if (node.is_html_element())
        return;
    // Registration is where an element's publications keyed by its style node begin: an attribute written before it
    // had one published nothing.
    if (auto* svg_element = as_if<SVG::SVGElement>(node))
        Layout::publish_svg_attribute_facts(*svg_element);
    else if (auto* mathml_element = as_if<MathML::MathMLElement>(node))
        Layout::publish_math_attribute_facts(*mathml_element);
}

static void ensure_slot(auto& nodes, u32 index)
{
    if (index >= nodes.size()) {
        nodes.grow_capacity(index + 1);
        nodes.resize(index + 1);
    }
}

void StyleComputer::ensure_style_node_slot(StyleNodeID style_node_id)
{
    if (style_node_id == 0)
        return;
    if (style_node_is_text(style_node_id))
        ensure_slot(m_text_style_nodes, style_node_index(style_node_id));
    else
        ensure_slot(m_element_style_nodes, style_node_index(style_node_id));
}

void StyleComputer::unregister_style_node(StyleNodeID style_node_id)
{
    if (style_node_id == 0)
        return;
    auto index = style_node_index(style_node_id);
    if (style_node_is_text(style_node_id)) {
        if (index < m_text_style_nodes.size())
            m_text_style_nodes[index] = nullptr;
        return;
    }
    if (index < m_element_style_nodes.size()) {
        m_element_style_nodes[index] = nullptr;
        m_style_nodes_retired_beside_lanes.set(style_node_id);
        StyleEngineFFI::style_engine_consume_element_style_input(m_style_engine.host(), style_node_id);
    }
}

GC::Ptr<DOM::Node> StyleComputer::node_for_lane_style_node(StyleNodeID style_node_id) const
{
    if (m_style_nodes_retired_beside_lanes.contains(style_node_id))
        return nullptr;
    return node_for_style_node(style_node_id);
}

GC::Ptr<DOM::Element> StyleComputer::element_for_lane_style_node(StyleNodeID style_node_id) const
{
    return as_if<DOM::Element>(node_for_lane_style_node(style_node_id).ptr());
}

GC::Ptr<DOM::Element> StyleComputer::element_for_style_node(StyleNodeID style_node_id) const
{
    return as_if<DOM::Element>(node_for_style_node(style_node_id).ptr());
}

GC::Ptr<DOM::Node> StyleComputer::node_for_style_node(StyleNodeID style_node_id) const
{
    if (!style_node_is_text(style_node_id)) {
        if (style_node_id == 0 || style_node_id.value() >= m_element_style_nodes.size())
            return nullptr;
        return m_element_style_nodes[style_node_id.value()];
    }
    auto index = style_node_index(style_node_id);
    if (index >= m_text_style_nodes.size())
        return nullptr;
    return m_text_style_nodes[index];
}

TreeScopeID StyleComputer::allocate_tree_scope(DOM::ShadowRoot& shadow_root)
{
    m_shadow_roots_by_tree_scope.append(shadow_root);
    return TreeScopeID { static_cast<u32>(m_shadow_roots_by_tree_scope.size()) };
}

DOM::ShadowRoot* StyleComputer::shadow_root_for_tree_scope(TreeScopeID tree_scope) const
{
    if (tree_scope == TreeScopeID {} || tree_scope.value() > m_shadow_roots_by_tree_scope.size())
        return nullptr;
    auto shadow_root = m_shadow_roots_by_tree_scope[tree_scope.value() - 1].ptr();
    // An adopted root gives its scope up, and the document it moved to numbers it again, perhaps with this number.
    if (!shadow_root || &shadow_root->document() != &document() || shadow_root->style_engine_tree_scope() != tree_scope)
        return nullptr;
    return shadow_root.ptr();
}

void StyleComputer::prepare_elements_for_style_computation()
{
    for (;;) {
        auto elements = m_style_engine.take_elements_awaiting_first_style_computation();
        if (elements.is_empty())
            break;
        for (auto style_node : elements) {
            auto element = element_for_style_node(style_node);
            if (element && element->is_connected())
                element->prepare_for_style_computation({});
        }
    }
}

void StyleComputer::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    m_style_engine.visit_edges(visitor);
    visitor.visit(m_element_style_nodes);
    visitor.visit(m_text_style_nodes);
    // NB: Source sheets are weak references; their owners trace them.
    visitor.ignore(m_style_engine_sheet_sources);
    for (auto const& entry : m_non_author_style_sheets)
        visitor.visit(entry.sheet);
    for (auto const& entry : m_constructed_sheet_ids)
        visitor.visit(entry.key);
    for (auto const& entry : m_shared_compiled_style_sheets) {
        if (entry.value)
            entry.value->contents().visit_edges(visitor);
    }

    for (auto const& state : m_provisional_transition_states) {
        visitor.visit(state.element);
        visitor.visit(state.committed_transition);
        visitor.visit(state.proposed_transition);
    }
}

void StyleComputer::begin_transition_stabilization_epoch()
{
    VERIFY(m_provisional_transition_states.is_empty());
    VERIFY(m_provisional_transition_state_indices.is_empty());
    VERIFY(m_provisional_transition_state_indices_by_target.is_empty());
}

void StyleComputer::record_transition_stabilization_baseline(DOM::AbstractElement abstract_element, InstalledStyle const& before_change_style) const
{
    auto style_node_id = abstract_element.element().style_node_id();
    if (style_node_id == 0)
        return;
    // Few epochs record a baseline, so only one that does asks for or releases them.
    m_transition_baselines_recorded = true;
    StyleEngineFFI::style_engine_record_transition_baseline(m_style_engine.host(), style_node_id, pseudo_element_to_ffi(abstract_element.pseudo_element()), before_change_style.record_pointer());
}

// https://drafts.csswg.org/css-transitions-2/#defining-before-change-style
// A later pass of this stabilization epoch can give the element a transition it does not declare yet, which is decided
// against the style the element moved away from in this one. Size container queries and the feedback epoch run such
// passes.
void StyleComputer::record_transition_baseline_for_later_passes(DOM::AbstractElement abstract_element, InstalledStyle const& before_change_style) const
{
    if (abstract_element.style_scope().rule_cache().has_size_container_queries || document().is_in_style_stabilization_feedback_epoch())
        record_transition_stabilization_baseline(abstract_element, before_change_style);
}

// A provisionally started transition already contributed to the style published by the pass that
// started it, but it is not associated with its target until the stabilization epoch commits. An
// animated style update running before that commit has to collect it anyway, or it rebuilds the
// target's style without the transition's value and clobbers it.
void StyleComputer::for_each_provisional_transition_effect(DOM::AbstractElement const& abstract_element, Function<void(Animations::KeyframeEffect&)> const& callback) const
{
    for (auto const& state : m_provisional_transition_states) {
        if (state.element.ptr() != &abstract_element.element() || state.pseudo_element != abstract_element.pseudo_element())
            continue;
        if (!state.proposed_transition)
            continue;
        if (auto effect = state.proposed_transition->effect(); effect && effect->is_keyframe_effect())
            callback(static_cast<Animations::KeyframeEffect&>(*effect));
    }
}

void StyleComputer::commit_transition_stabilization_epoch()
{
    for (auto const& state : m_provisional_transition_states) {
        VERIFY(state.element);
        auto& element = *state.element;
        auto remove_committed_transition = [&] {
            if (element.property_transition(state.pseudo_element, state.property) == state.committed_transition)
                element.remove_transition(state.pseudo_element, state.property);
        };
        auto cancel_and_remove_committed_transition = [&] {
            VERIFY(state.committed_transition);
            state.committed_transition->cancel();
            remove_committed_transition();
        };
        auto commit_proposed_transition = [&] {
            VERIFY(state.proposed_transition);
            state.proposed_transition->commit_provisional_transition();
            ++document().style_invalidation_counters().committed_transitions_started;
        };

        switch (state.action) {
            using enum StyleValueFFI::FfiTransitionActionKind;
        case None:
            continue;
        case Remove:
            remove_committed_transition();
            break;
        case Cancel:
            VERIFY(state.committed_transition);
            state.committed_transition->cancel();
            break;
        case Start:
            commit_proposed_transition();
            break;
        case RemoveAndStart:
            remove_committed_transition();
            commit_proposed_transition();
            break;
        case CancelRemoveAndStart:
            cancel_and_remove_committed_transition();
            commit_proposed_transition();
            break;
        }
        ++document().style_invalidation_counters().committed_transition_actions;
    }
    m_provisional_transition_states.clear();
    m_provisional_transition_state_indices.clear();
    m_provisional_transition_state_indices_by_target.clear();
    if (exchange(m_transition_baselines_recorded, false))
        StyleEngineFFI::style_engine_release_transition_baselines(m_style_engine.host());
}

template<size_t length>
static constexpr Utf16View utf16_view(char16_t const (&string)[length])
{
    return { string, length - 1 };
}

Optional<Utf16String> StyleComputer::user_agent_style_sheet_source(Utf16View name)
{
    extern String const& default_stylesheet_source;
    extern String const& quirks_mode_stylesheet_source;
    extern String const& mathml_stylesheet_source;
    extern String const& svg_stylesheet_source;

    if (name == utf16_view(u"CSS/Default.css"))
        return Utf16String::from_utf8(default_stylesheet_source);
    if (name == utf16_view(u"CSS/QuirksMode.css"))
        return Utf16String::from_utf8(quirks_mode_stylesheet_source);
    if (name == utf16_view(u"MathML/Default.css"))
        return Utf16String::from_utf8(mathml_stylesheet_source);
    if (name == utf16_view(u"SVG/Default.css"))
        return Utf16String::from_utf8(svg_stylesheet_source);
    return {};
}

struct ResolvedScope {
    GC::Ptr<DOM::Element const> root;
    size_t proximity { NumericLimits<size_t>::max() };
};

static RefPtr<CustomPropertyData const> inheritable_custom_property_data(Layout::BegunRead const& read, DOM::AbstractElement abstract_element)
{
    auto data = abstract_element.custom_property_data();
    if (!data)
        return nullptr;
    return data->inheritable(read, abstract_element.document());
}

// What one element's animation sample hands the style engine, which lives until the sample is finished.
struct StyleComputer::AnimationSample {
    AK_ALLOC_WITH_KMALLOC;

    AnimationSample(GC::Ref<StyleComputer const> style_computer, DOM::AbstractElement abstract_element, ComputedStyleWorkingSet& computed_properties)
        : context { style_computer, abstract_element, computed_properties }
    {
    }

    struct Context {
        GC::Ref<StyleComputer const> style_computer;
        DOM::AbstractElement abstract_element;
        ComputedStyleWorkingSet& computed_properties;
    } context;
    Vector<ComputedValuesFFI::FfiSampledAnimationEffect, 1> sampled_effects;
    Vector<Vector<Compositing::RustFFI::FfiLinearEasingPoint>, 1> easing_points;
    Vector<u8> document_supported_color_scheme_codes;
    // The custom-property environments whose stores the input names.
    RefPtr<CustomPropertyData const> custom_property_data;
    RefPtr<CustomPropertyData const> base_custom_property_data;
    RefPtr<CustomPropertyData const> inheritance_custom_property_data;
    ComputedValuesFFI::FfiStyleComputationEnvironment environment {};
    ComputedValuesFFI::FfiHostAnimationSample input {};
};

void StyleComputer::collect_animations_into(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>> effects, ComputedStyleWorkingSet& computed_properties) const
{
    // A refresh samples over the record the element holds, which the working set was reconstructed from.
    collect_animation_effects_into(read, abstract_element, effects, computed_properties);
    finish_animation_refresh(read, abstract_element, computed_properties);
}

void StyleComputer::refresh_animations_into_each(Layout::BegunRead const& read, ReadonlySpan<AnimationRefreshRequest> requests) const
{
    if (requests.is_empty())
        return;
    // The samples are taken in one call of the style engine, each over the record its element holds.
    Vector<NonnullOwnPtr<AnimationSample>> samples;
    Vector<ComputedValuesFFI::FfiHostAnimationSample> inputs;
    samples.ensure_capacity(requests.size());
    inputs.ensure_capacity(requests.size());
    for (auto const& request : requests) {
        samples.unchecked_append(begin_animation_sample(read, request.abstract_element, request.effects, request.computed_properties));
        inputs.unchecked_append(samples.last()->input);
    }
    Vector<ComputedValuesFFI::FfiHostAnimationSampleResult> results;
    results.resize(inputs.size());
    ComputedValuesFFI::rust_sample_animation_effects_each(inputs.data(), inputs.size(), &read, results.data());
    for (size_t index = 0; index < requests.size(); ++index) {
        auto const& request = requests[index];
        finish_animation_sample(*samples[index], results[index]);
        finish_animation_refresh(read, request.abstract_element, request.computed_properties);
    }
}

void StyleComputer::finish_animation_refresh(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, ComputedStyleWorkingSet& computed_properties) const
{
    publish_animated_custom_properties(computed_properties, abstract_element);
    if (computed_properties.requires_animated_post_compute_adjustments())
        finalize_animated_box_type(read, computed_properties, abstract_element);
}

// The engine decides the timing only where every time is in one unit: a duration, or a percentage of a scroll
// timeline's progress.
ComputedValuesFFI::FfiEffectTiming style_engine_effect_timing(Animations::KeyframeEffect const& effect, Animations::Animation const& animation)
{
    ComputedValuesFFI::FfiEffectTiming timing {};

    timing.is_relevant = animation.is_relevant();
    timing.is_removed = animation.replace_state() == Animations::AnimationReplaceState::Removed;

    Optional<Animations::TimeValue::Type> unit;
    bool one_unit = true;
    auto duration = [&](Animations::TimeValue const& time) {
        one_unit &= time.type == unit.value_or(time.type);
        unit = time.type;
        return time.value;
    };
    auto optional_duration = [&](Optional<Animations::TimeValue> const& time, bool& has_time, double& value) {
        has_time = time.has_value();
        if (time.has_value())
            value = duration(*time);
    };
    if (auto timeline = animation.timeline()) {
        optional_duration(timeline->current_time(), timing.has_timeline_time, timing.timeline_time);
        // A document timeline's time is a timestamp less its origin time, which is what lets a clock tick sample it.
        if (timeline->can_convert_a_timeline_time_to_an_origin_relative_time()) {
            auto origin_time = timeline->convert_a_timeline_time_to_an_origin_relative_time(Animations::TimeValue { Animations::TimeValue::Type::Milliseconds, 0 });
            timing.has_timeline_origin_time = origin_time.has_value();
            timing.timeline_origin_time = origin_time.value_or(0);
        }
        // A scroll timeline's time is the progress of the scroller it follows, which is what lets a clock tick sample
        // it where the compositor has scrolled to.
        if (auto const* scroll_timeline = as_if<Animations::ScrollTimeline>(*timeline); scroll_timeline && scroll_timeline->followed_scroller().has_value()) {
            timing.has_timeline_scroller = true;
            timing.scroll_timeline = scroll_timeline->clock_identity();
        }
        if (auto minimum = timeline->minimum_timeline_time(), maximum = timeline->maximum_timeline_time(); minimum.has_value() && maximum.has_value()) {
            timing.has_timeline_boundaries = true;
            timing.minimum_timeline_time = minimum->value;
            timing.maximum_timeline_time = maximum->value;
        }
    }
    optional_duration(animation.start_time(), timing.has_start_time, timing.start_time);
    optional_duration(animation.hold_time(), timing.has_hold_time, timing.hold_time);
    timing.paused = animation.play_state() == Bindings::AnimationPlayState::Paused;
    timing.playback_rate = animation.playback_rate();
    timing.start_delay = duration(effect.start_delay());
    timing.end_delay = duration(effect.end_delay());
    timing.iteration_duration = duration(effect.iteration_duration());
    timing.iteration_count = effect.iteration_count();
    timing.iteration_start = effect.iteration_start();
    timing.fill_mode = static_cast<u8>(to_underlying(effect.fill_mode()));
    timing.playback_direction = static_cast<u8>(to_underlying(effect.playback_direction()));
    timing.decidable = one_unit && !effect.has_local_time_override_for_observation();
    return timing;
}

void StyleComputer::collect_animation_effects_into(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>> effects, ComputedStyleWorkingSet& computed_properties) const
{
    auto sample = begin_animation_sample(read, abstract_element, effects, computed_properties);
    auto result = ComputedValuesFFI::rust_sample_animation_effects(&sample->input, &read);
    finish_animation_sample(*sample, result);
}

NonnullOwnPtr<StyleComputer::AnimationSample> StyleComputer::begin_animation_sample(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>> effects, ComputedStyleWorkingSet& computed_properties) const
{
    auto sample = make<AnimationSample>(*this, abstract_element, computed_properties);
    // The style engine samples each effect from the description it holds of it, kept current here, right before
    // the element is sampled.
    auto const animation_slot = abstract_element.pseudo_element().map([](auto pseudo_element) { return static_cast<u8>(to_underlying(pseudo_element) + 1); }).value_or(0);
    record_element_animation_effect_descriptions(abstract_element.element(), animation_slot, effects);

    // The effects and their timing are the host's; the key each samples at, and what their keyframes compute to, are
    // the engine's. The host computes the key only for a timing the engine cannot decide.
    auto& sampled_effects = sample->sampled_effects;
    auto& easing_points = sample->easing_points;
    sampled_effects.ensure_capacity(effects.size());
    easing_points.ensure_capacity(effects.size());
    for (auto effect : effects) {
        auto animation = effect->associated_animation();
        if (!animation)
            continue;
        auto timing = style_engine_effect_timing(*effect, *animation);
        Optional<double> current_key;
        if (!timing.decidable) {
            if (auto output_progress = effect->transformed_progress(); output_progress.has_value())
                current_key = clamp(*output_progress * 100.0 * Animations::KeyframeEffect::AnimationKeyFrameKeyScaleFactor, static_cast<double>(NumericLimits<i64>::min()), static_cast<double>(NumericLimits<i64>::max()));
        }
        easing_points.unchecked_append({});
        sampled_effects.unchecked_append({
            .identity = effect->animation_preparation_identity(),
            .generation = effect->animation_preparation_generation(),
            .timing = timing,
            .easing = effect->timing_function().descriptor_with_points_in(easing_points.last()),
            .current_key = current_key,
        });
    }

    // Keyframes substitute against the element's custom properties as they stand; an animated custom property
    // composes over them with the overlay of the sample before peeled off, and a keyframe saying `inherit` takes
    // the environment the element inherits.
    auto& custom_property_data = sample->custom_property_data;
    auto& base_custom_property_data = sample->base_custom_property_data;
    auto& inheritance_custom_property_data = sample->inheritance_custom_property_data;
    custom_property_data = abstract_element.custom_property_data();
    base_custom_property_data = custom_property_data;
    if (base_custom_property_data && abstract_element.custom_property_data_is_animation_overlay())
        base_custom_property_data = base_custom_property_data->parent();
    auto inheritance_parent = abstract_element.element_to_inherit_style_from();
    inheritance_custom_property_data = inheritance_parent.has_value() ? inheritance_parent->custom_property_data() : nullptr;
    bool const element_declares_own_custom_properties = base_custom_property_data
        && !(inheritance_parent.has_value() && inheritable_custom_property_data(read, *inheritance_parent).ptr() == base_custom_property_data.ptr());

    // The document's side of the environment keyframes compute in; the engine fills in the element's.
    auto& document_supported_color_scheme_codes = sample->document_supported_color_scheme_codes;
    auto document_supported_color_schemes = document().supported_color_schemes();
    if (document_supported_color_schemes.has_value()) {
        document_supported_color_scheme_codes.ensure_capacity(document_supported_color_schemes->size());
        for (auto const& scheme : *document_supported_color_schemes)
            document_supported_color_scheme_codes.unchecked_append(to_underlying(preferred_color_scheme_from_string(scheme)));
    }
    auto document_base_url_bytes = document().serialized_base_url().bytes();
    sample->environment = {
        .box_type_input = {},
        .color_scheme_input = {
            .preferred_color_scheme = static_cast<u8>(to_underlying(document().page().preferred_color_scheme())),
            .has_document_supported_schemes = document_supported_color_schemes.has_value(),
            .document_supported_scheme_codes = document_supported_color_scheme_codes.data(),
            .document_supported_scheme_count = document_supported_color_scheme_codes.size(),
        },
        .is_th_element = false,
        .has_new_font_size = false,
        .has_tree_counting_context = false,
        .sibling_count = 0,
        .sibling_index = 0,
        .random_base_values = nullptr,
        .random_base_value_count = 0,
        .document_base_url = document_base_url_bytes.data(),
        .document_base_url_length = document_base_url_bytes.size(),
        .style_sheet_resource_contexts = nullptr,
        .style_sheet_resource_context_count = 0,
        .device_pixels_per_css_pixel = m_document->page().client().device_pixels_per_css_pixel(),
        .initial_font_size_raw = InitialValues::font_size().raw_value(),
        .default_font_size_raw = default_user_font_size().raw_value(),
        .root_effective_zoom = root_element_effective_zoom(),
    };

    using SampleContext = AnimationSample::Context;
    sample->input = {
        .host = m_style_engine.host(),
        .style_node = abstract_element.element().style_node_id().value(),
        .pseudo_kind = abstract_element.pseudo_element().map([](auto pseudo_element) { return static_cast<u8>(to_underlying(pseudo_element)); }).value_or(NumericLimits<u8>::max()),
        .effects = sampled_effects.data(),
        .effect_count = sampled_effects.size(),
        .record = computed_properties.record(),
        .animated_overlay = computed_properties.animated_overlay(Badge<StyleComputer> {}),
        .style_record = abstract_element.installed_style().record_pointer(),
        .custom_property_store = custom_property_data ? custom_property_data->rust_store() : nullptr,
        .base_custom_property_store = base_custom_property_data ? base_custom_property_data->rust_store() : nullptr,
        .inheritance_custom_property_store = inheritance_custom_property_data ? inheritance_custom_property_data->rust_store() : nullptr,
        .element_declares_own_custom_properties = element_declares_own_custom_properties,
        .custom_property_environments = {
            custom_property_data ? custom_property_data->identity() : 0,
            inheritance_custom_property_data ? inheritance_custom_property_data->identity() : 0,
        },
        .custom_property_registration_generation = document().custom_property_registration_generation(),
        .inheritance_parent_record = inheritance_parent.has_value() ? inheritance_parent->installed_style().record_pointer() : nullptr,
        .environment = &sample->environment,
        .element_box_slot = Layout::Node::slot_id(abstract_element.element().unsafe_layout_node(read)).index,
        .callback_context = &sample->context,
        .prepare_overlay_for_mutation = [](void* context) -> void* {
            auto& sample = *static_cast<SampleContext*>(context);
            return sample.computed_properties.prepare_animated_overlay_for_rust_mutation(Badge<StyleComputer> {});
        },
        .length_contexts = [](void* context, u8 container_relative_length_unit_mask, ComputedValuesFFI::FfiAnimationLengthContexts* contexts) {
            auto& sample = *static_cast<SampleContext*>(context);
            // Each context is the sampled element's own, built afresh: a batch builds several elements' contexts in one call.
            auto length_context_for = [&](PropertyID property_id) {
                return to_ffi_length_resolution_context_with_container_bases(
                    sample.style_computer->make_computation_context_for_property(property_id, sample.computed_properties, sample.abstract_element).length_resolution_context,
                    container_relative_length_unit_mask);
            };
            contexts->font = length_context_for(PropertyID::FontFamily);
            contexts->line_height = length_context_for(PropertyID::LineHeight);
            contexts->remaining = length_context_for(PropertyID::Color); },
    };
    return sample;
}

void StyleComputer::finish_animation_sample(AnimationSample& sample, ComputedValuesFFI::FfiHostAnimationSampleResult const& result) const
{
    auto abstract_element = sample.context.abstract_element;
    auto& computed_properties = sample.context.computed_properties;
    // What the substituted values read is what the element's style now depends on, as for a value its cascade
    // substituted.
    auto& element = abstract_element.element();
    if (result.substitution_marks & ComputedValuesFFI::SUBSTITUTION_MARK_ATTR)
        element.set_style_uses_attr_css_function();
    if (result.substitution_marks & ComputedValuesFFI::SUBSTITUTION_MARK_IF)
        element.set_style_uses_if_css_function();
    if (result.substitution_marks & ComputedValuesFFI::SUBSTITUTION_MARK_CUSTOM_FUNCTION)
        element.set_style_uses_custom_function();

    switch (result.outcome) {
    case ComputedValuesFFI::FfiHostAnimationSampleOutcome::Unchanged:
        return;
    case ComputedValuesFFI::FfiHostAnimationSampleOutcome::Cleared:
        computed_properties.clear_animated_properties(Badge<StyleComputer> {});
        return;
    case ComputedValuesFFI::FfiHostAnimationSampleOutcome::Evaluated:
        break;
    }
    computed_properties.set_animated_custom_properties_trigger_restyle(Badge<StyleComputer> {}, result.custom_properties_trigger_restyle);
    if (result.depends_on_viewport_metrics)
        computed_properties.set_depends_on_viewport_metrics();
    if (result.font_metrics_depend_on_viewport_metrics)
        computed_properties.set_font_metrics_depend_on_viewport_metrics();
    if (result.uses_tree_counting_function)
        element.set_style_uses_tree_counting_function();
    for (auto const& property : ReadonlySpan<ComputedValuesFFI::FfiAnimatedCustomPropertyResult> { result.animated_custom_properties, result.animated_custom_property_count }) {
        auto value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(property.value)));
        computed_properties.set_animated_custom_property(Badge<StyleComputer> {}, Utf16FlyString::from_raw(property.name), move(value));
    }
    ComputedValuesFFI::rust_release_animated_custom_property_results(result.animated_custom_properties_storage);
    computed_properties.finish_animated_overlay_rust_mutation(Badge<StyleComputer> {});
}

// NB: A dependency cycle among animated values and what the element declares is invalid, which the sample answers. One
//     it cannot name, through a custom function or attr(), or an animation that its own values start and end, would
//     feed each pass into the next with no end. In an acyclic chain an element's values move only as what they read
//     does, at most once per value animated on it and above it, so values still moving after that stop where they are.
bool StyleComputer::animated_custom_properties_may_move(DOM::AbstractElement abstract_element, size_t animated_value_count) const
{
    auto& document = abstract_element.document();
    if (!document.is_in_style_stabilization_epoch())
        return true;
    constexpr u64 ordinary_change_limit = 8;
    auto const changes = document.note_animated_custom_properties_moved_in_stabilization_epoch(abstract_element.element().style_node_id());
    if (changes <= ordinary_change_limit)
        return true;
    u64 animated_above = animated_value_count;
    for (auto ancestor = abstract_element.element_to_inherit_style_from(); ancestor.has_value(); ancestor = ancestor->element_to_inherit_style_from()) {
        if (auto ancestor_data = ancestor->custom_property_data(); ancestor_data && ancestor->custom_property_data_is_animation_overlay())
            animated_above += ancestor_data->declared_count();
    }
    return changes <= ordinary_change_limit + animated_above;
}

void StyleComputer::stop_publishing_animated_custom_properties(DOM::AbstractElement abstract_element) const
{
    auto data = abstract_element.custom_property_data();
    if (!data || !abstract_element.custom_property_data_is_animation_overlay())
        return;
    if (!animated_custom_properties_may_move(abstract_element, data->declared_count()))
        return;
    abstract_element.replace_custom_property_data(Badge<StyleComputer> {}, data->parent(), IsAnimationOverlay::No);
    invalidate_animated_custom_property_readers(abstract_element, true);
}

void StyleComputer::publish_animated_custom_properties(ComputedStyleWorkingSet& computed_properties, DOM::AbstractElement abstract_element) const
{
    auto data = abstract_element.custom_property_data();
    bool const holds_animation_overlay = data && abstract_element.custom_property_data_is_animation_overlay();
    RefPtr<CustomPropertyData const> base = data;
    if (holds_animation_overlay)
        base = data->parent();

    auto const& animated_values = computed_properties.animated_custom_properties();
    if (animated_values.is_empty()) {
        stop_publishing_animated_custom_properties(abstract_element);
        return;
    }

    if (holds_animation_overlay && data->declared_count() == animated_values.size()) {
        bool values_unchanged = true;
        for (auto const& [name, value] : animated_values) {
            if (!data->declares_value(name, *value)) {
                values_unchanged = false;
                break;
            }
        }
        if (values_unchanged)
            return;
    }

    if (!animated_custom_properties_may_move(abstract_element, animated_values.size()))
        return;

    OrderedHashMap<Utf16FlyString, StyleProperty> overlay_values;
    for (auto const& [name, value] : animated_values) {
        overlay_values.set(name,
            StyleProperty {
                .important = Important::No,
                .property_id = PropertyID::Custom,
                .value = value,
            });
    }
    abstract_element.replace_custom_property_data(Badge<StyleComputer> {}, CustomPropertyData::create_animation_overlay(move(overlay_values), move(base)), IsAnimationOverlay::Yes);
    invalidate_animated_custom_property_readers(abstract_element, computed_properties.animated_custom_properties_trigger_restyle());
}

void StyleComputer::invalidate_animated_custom_property_readers(DOM::AbstractElement abstract_element, bool recompute_style) const
{
    auto& element = abstract_element.element();
    // NB: A read below an element that is not displayed samples the animations on its way down. No pass styles what is
    //     below that element, and what the read goes on to demand computes from the environment the sample published.
    if (m_inheritance_chain_read_depth > 0) {
        if (auto style = element.computed_style(); style && (style->in_display_none_subtree() || style->display().is_none()))
            return;
    }
    auto& style_engine = element.document().style_computer().style_engine();
    if (recompute_style) {
        style_engine.record_derived_element_style_input_change(element.style_node_id(), StyleEngine::PublishedStyle | StyleEngine::RecomputeStyle);
    } else if (!abstract_element.pseudo_element().has_value()) {
        // Style container queries can read animated values even when those values do not inherit.
        element.invalidate_descendant_styles_depending_on_style_container_query();
    }
    // NB: The style engine restyles the children of an element whose animations sampled other custom properties
    //     itself, as the engine learns of the element's new environment.
}

// The host's form of one animation definition a style computation decided.
static AnimationProperties animation_properties_from_ffi(ComputedValuesFFI::FfiComputedAnimation const& animation)
{
    Variant<double, Utf16String> duration { animation.duration };
    if (animation.duration_is_auto)
        duration = "auto"_utf16;
    auto timing_function_value = RustStyleValueHandle::retained(static_cast<StyleValueFFI::StyleValueData const*>(animation.timing_function));
    auto timing_function = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(timing_function_value.data()));
    static_assert(to_underlying(AnimationTimelineSource::Kind::Document) == to_underlying(ComputedValuesFFI::FfiAnimationTimelineKind::Document));
    static_assert(to_underlying(AnimationTimelineSource::Kind::None) == to_underlying(ComputedValuesFFI::FfiAnimationTimelineKind::None));
    static_assert(to_underlying(AnimationTimelineSource::Kind::Scroll) == to_underlying(ComputedValuesFFI::FfiAnimationTimelineKind::Scroll));
    static_assert(to_underlying(AnimationTimelineSource::Kind::View) == to_underlying(ComputedValuesFFI::FfiAnimationTimelineKind::View));
    static_assert(to_underlying(AnimationTimelineSource::Kind::Named) == to_underlying(ComputedValuesFFI::FfiAnimationTimelineKind::Named));
    return {
        .duration = move(duration),
        .timing_function = EasingFunction::from_style_value(timing_function),
        .iteration_count = animation.iteration_count,
        .direction = static_cast<AnimationDirection>(animation.direction),
        .play_state = static_cast<AnimationPlayState>(animation.play_state),
        .delay = animation.delay,
        .fill_mode = static_cast<AnimationFillMode>(animation.fill_mode),
        .composition = static_cast<AnimationComposition>(animation.composition),
        .name = css_string_from_rust(animation.name),
        .timeline = {
            .kind = static_cast<AnimationTimelineSource::Kind>(animation.timeline_kind),
            .scroller = static_cast<Scroller>(animation.scroll_scroller),
            .axis = static_cast<Axis>(animation.scroll_axis),
            .view_inset = Animations::view_timeline_inset_from_ffi(animation.view_inset),
            .name = animation.timeline_name ? css_string_from_rust(animation.timeline_name) : Utf16FlyString {},
        },
        .range = {
            .start = { static_cast<Animations::TimelineRangeName>(animation.range_start_name), animation.range_start_px, animation.range_start_percentage, Animations::retain_calculated_offset(animation.range_start_calculated) },
            .end = { static_cast<Animations::TimelineRangeName>(animation.range_end_name), animation.range_end_px, animation.range_end_percentage, Animations::retain_calculated_offset(animation.range_end_calculated) },
        },
        .timing_function_value = move(timing_function_value),
    };
}

void StyleComputer::apply_animation_definitions(DOM::AbstractElement& abstract_element, ReadonlySpan<ComputedValuesFFI::FfiComputedAnimation> animation_definitions, bool in_display_none_subtree) const
{
    auto& document = abstract_element.document();

    auto const* element_animations = abstract_element.css_defined_animations();

    // If we have a nullptr for element_animations it means that the pseudo element was invalid and thus we shouldn't apply animations
    if (!element_animations)
        return;

    // https://drafts.csswg.org/css-animations-1/#animations
    // Setting the 'display' property to 'none' will terminate any running animation applied to the element and its
    // descendants. If an element has a 'display' of 'none', updating 'display' to a value other than 'none' will
    // start all animations applied to the element by the 'animation-name' property, as well as all animations
    // applied to descendants with 'display' other than 'none'.
    // NB: We must not start animations on elements that are not rendered due to display:none. Once display becomes
    //     something other than none, the resulting style recomputation re-enters this function and starts them.
    //     Termination of running animations when display becomes none is handled by
    //     Element::play_or_cancel_animations_after_display_property_change(). The style computation answers
    //     whether the element is in such a subtree from the records the style engine holds.

    // NB: Which existing animation each definition claims is decided by the style computation, from the names of
    //     the animations this element holds, which it publishes. See match_existing_animations(). So are the
    //     keyframes each definition runs, from the `@keyframes` every style scope publishes.

    GC::RootVector<GC::Ref<CSSAnimation>> existing_animations { element_animations->span() };
    Vector<bool> existing_animation_was_claimed;
    existing_animation_was_claimed.resize(existing_animations.size());
    GC::RootVector<GC::Ref<CSSAnimation>> new_animations;

    for (size_t i = animation_definitions.size(); i-- > 0;) {
        auto const& definition = animation_definitions[i];
        auto animation_properties = animation_properties_from_ffi(definition);
        auto const* keyframe_set = static_cast<Animations::KeyframeEffect::KeyFrameSet const*>(definition.keyframe_set);

        auto matched_index = definition.matched_existing_index;
        VERIFY(matched_index < static_cast<i32>(existing_animations.size()));

        if (matched_index >= 0) {
            auto existing_animation = existing_animations[matched_index];
            VERIFY(existing_animation->animation_name() == animation_properties.name);
            VERIFY(!existing_animation_was_claimed[matched_index]);
            existing_animation_was_claimed[matched_index] = true;

            if (auto effect = existing_animation->effect()) {
                as<Animations::KeyframeEffect>(*effect).set_key_frame_set(keyframe_set);
                existing_animation->apply_css_properties(animation_properties, keyframe_set, abstract_element);
            }
            existing_animation->set_animation_name_index(i);
            new_animations.append(existing_animation);
            continue;
        }

        if (in_display_none_subtree)
            continue;

        // An animation applies to an element if its name appears as one of the identifiers in the computed value of the
        // animation-name property and the animation uses a valid @keyframes rule
        auto animation = CSSAnimation::create(document.relevant_settings_object());
        animation->set_animation_name(animation_properties.name);
        animation->set_owning_element(abstract_element);

        auto effect = Animations::KeyframeEffect::create();
        animation->set_effect(effect);

        animation->apply_css_properties(animation_properties, keyframe_set, abstract_element);
        animation->set_animation_name_index(i);

        effect->set_key_frame_set(keyframe_set);

        effect->set_target(abstract_element);
        new_animations.append(animation);
    }

    // Once an animation has started it continues until it ends or the animation-name is removed
    // NB: An animation no definition claimed is one whose animation-name entry has gone.
    for (size_t i = 0; i < existing_animations.size(); ++i) {
        if (!existing_animation_was_claimed[i]) {
            existing_animations[i]->cancel(Animations::Animation::ShouldInvalidate::No);
            existing_animations[i]->stop_following_named_timeline();
        }
    }

    // NB: We create animations in reverse definition order so flip it back.
    new_animations.reverse();

    abstract_element.set_css_defined_animations(move(new_animations));
}

void StyleComputer::apply_settled_animation_plan(Layout::BegunRead const& read, DOM::AbstractElement& abstract_element) const
{
    struct Context {
        GC::Ref<StyleComputer const> style_computer;
        DOM::AbstractElement& abstract_element;
    } context { *this, abstract_element };
    auto const* animations = abstract_element.css_defined_animations();
    bool has_animations = animations && !animations->is_empty();
    ComputedValuesFFI::rust_settled_animation_plan(m_style_engine.host(), &read, abstract_element.element().style_node_id().value(), pseudo_element_to_ffi(abstract_element.pseudo_element()), abstract_element.installed_style().record_pointer(), has_animations, &context, [](void* context_pointer, ComputedValuesFFI::FfiComputedAnimation const* definitions, size_t count, bool in_display_none_subtree) {
        auto& context = *static_cast<Context*>(context_pointer);
        context.style_computer->apply_animation_definitions(context.abstract_element, { definitions, count }, in_display_none_subtree);
    });
}

// The host applies the animation plan of a record the engine settled once the record is installed and samples the
// animations over it, as an animation update samples them over the record an element holds, and publishes what they
// compose.
// https://drafts.csswg.org/css-transitions-2/#defining-before-change-style
// Sampling the installed record can keep the epoch's before-change style, and the record held by then is the
// after-change one, so a record that owes the transition step keeps the record it moved away from first.
void StyleComputer::compose_installed_engine_record(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, InstalledStyle const& before_change_style) const
{
    apply_settled_animation_plan(read, abstract_element);
    if (before_change_style && document().is_in_style_stabilization_epoch())
        record_transition_stabilization_baseline(abstract_element, before_change_style);
    if (!has_animations_to_compose(abstract_element)) {
        stop_publishing_animated_custom_properties(abstract_element);
        return;
    }
    auto const& style = abstract_element.installed_style();
    if (!style)
        return;
    Animations::AnimationUpdateContext::ElementData element_data { style, reconstruct_computed_properties_for_animation(style) };
    element_data.base_is_current = true;
    Animations::AnimationUpdateContext context;
    context.elements.set(abstract_element, move(element_data));
    context.publish();
}

static Optional<double> collect_dimension_attribute(Vector<StyleProperty>& properties, DOM::Element const& element, Utf16FlyString const& attribute_name, CSS::PropertyID property_id)
{
    auto attribute = element.attribute(attribute_name);
    if (!attribute.has_value())
        return {};

    double unclamped_value;
    auto parsed_value = HTML::parse_dimension_value(*attribute, unclamped_value);
    if (!parsed_value)
        return {};

    properties.append({ .property_id = property_id, .value = *parsed_value });
    // NB: The ratio uses the parsed number, rather than the clamped layout dimension. Values beyond the numeric
    //     representation's range cannot form a finite ratio hint, but keep their ordinary dimension mapping.
    if (!parsed_value->is_length() || !__builtin_isfinite(unclamped_value))
        return {};
    return unclamped_value;
}

// https://html.spec.whatwg.org/multipage/rendering.html#map-to-the-aspect-ratio-property-(using-dimension-rules)
static void collect_aspect_ratio_from_dimension_values(Vector<StyleProperty>& properties, Optional<double> width, Optional<double> height)
{
    // if element has both attributes w and h, and parsing those attributes' values using the rules for parsing
    // dimension values doesn't generate an error or return a percentage for either,
    if (!width.has_value() || !height.has_value())
        return;

    // then the user agent is expected to use the parsed dimensions as a presentational hint for the 'aspect-ratio'
    // property of the form auto w / h.
    auto aspect_ratio = CSS::StyleValueList::create(
        CSS::StyleValueVector {
            CSS::KeywordStyleValue::create(CSS::Keyword::Auto),
            CSS::RatioStyleValue::create(
                CSS::NumberStyleValue::create(*width),
                CSS::NumberStyleValue::create(*height)),
        },
        CSS::StyleValueList::Separator::Space);
    properties.append({ .property_id = CSS::PropertyID::AspectRatio, .value = aspect_ratio });
}

// The whole transition step for an element's record, run once the record is installed.
//
// The step needs two styles: the one the element moved away from, which the caller names, and the one it moved to,
// which is the record just installed. Both are records, and the step reads the before-change half only through the
// transition baseline. A transition the step starts layers its current values into the after-change style to
// keep the frame from jumping; publishing that is the same animation overlay publication an animation sample performs,
// on the same element, over the same base, and the step samples the transitions over the installed record as such a
// sample does: the box-type transformation of the values they take adjusts the composition, and the base stays current.
RequiredInvalidationAfterStyleChange StyleComputer::run_transition_step_for_installed_record(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, InstalledStyle const& before_change_style) const
{
    auto const installed_style = abstract_element.installed_style();
    if (!installed_style || !before_change_style)
        return {};
    auto& element = abstract_element.element();
    auto pseudo_element = abstract_element.pseudo_element();

    record_transition_baseline_for_later_passes(abstract_element, before_change_style);

    // OPTIMIZATION: The transition entries and existing transitions `start_needed_transitions` decides over, plus this
    //               element's own provisional states. With none of them there is nothing to decide, and the
    //               after-change style need not be reconstructed at all.
    if (!element.has_matching_transition_property_entry(pseudo_element)
        && !element.has_existing_transitions(pseudo_element)
        && !has_provisional_transition_states(abstract_element))
        return {};

    // A transition starts from the before-change style the epoch keeps, if any, and never from a style under
    // display: none. The installed record may itself be display: none; checking it would skip the discrete transition
    // into that state.
    InstalledStyle const baseline { m_transition_baselines_recorded ? StyleEngineFFI::style_engine_transition_baseline(m_style_engine.host(), element.style_node_id().value(), pseudo_element_to_ffi(pseudo_element)) : nullptr };
    auto const& before_change = baseline ? baseline : before_change_style;
    if (has_flag(before_change.dependency_flags(), StyleRecordDependencyFlag::InDisplayNoneSubtree))
        return {};
    if (auto parent = abstract_element.element_to_inherit_style_from(); parent.has_value()) {
        if (auto parent_style = parent->computed_style(); parent_style && parent_style->in_display_none_subtree())
            return {};
    }

    begin_style_update();
    ScopeGuard end_style_update = [&] { this->end_style_update(); };
    auto new_style = start_needed_transitions(read, abstract_element, before_change);
    if (!new_style)
        return {};
    auto const* installed_overlay = static_cast<ComputedValuesFFI::AnimatedOverlay const*>(installed_style.view().animated_overlay);

    // A step that starts, ends or replaces nothing leaves the installed record as it stands.
    if (installed_overlay) {
        if (!abstract_element.installed_style().animation_overlay_changed(new_style->animated_overlay()))
            return {};
    } else if (auto animated_properties = new_style->animated_properties_snapshot(); !animated_properties || animated_properties->is_empty()) {
        return {};
    }
    SampledAnimationOverlay const overlay { abstract_element, *new_style };
    StyleEngineFFI::FfiAnimationOverlayPublication publication;
    publish_sampled_animation_overlays(read, { &overlay, 1 }, { &publication, 1 });
    element.refresh_computed_style(read, pseudo_element, InstalledStyle::adopt(publication.record));
    if (auto* svg_element = as_if<SVG::SVGElement>(element); svg_element && !pseudo_element.has_value())
        svg_element->note_svg_paint_resource_description_may_have_changed();
    return decode_style_invalidation(publication.invalidation.invalidation);
}

// The key of the provisional transition states of an element or pseudo-element, whose element has a style node.
static u64 transition_target_key(DOM::AbstractElement abstract_element)
{
    auto style_node_id = abstract_element.element().style_node_id();
    VERIFY(style_node_id != 0);
    return (static_cast<u64>(style_node_id.value()) << 8) | pseudo_element_to_ffi(abstract_element.pseudo_element());
}

// https://drafts.csswg.org/css-transitions/#starting
RefPtr<ComputedStyleWorkingSet> StyleComputer::start_needed_transitions(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, InstalledStyle const& before_change_style) const
{
    auto had_pending_animated_style_update = m_document->needs_animated_style_update();

    auto& element = abstract_element.element();
    auto pseudo_element = abstract_element.pseudo_element();
    auto target_key = transition_target_key(abstract_element);

    // NB: We know that a DocumentTimeline's current time is always in milliseconds
    auto current_time = m_document->timeline()->current_time();
    if (!current_time.has_value())
        return {};
    VERIFY(current_time->type == Animations::TimeValue::Type::Milliseconds);
    auto style_change_event_time = current_time->value;

    Vector<StyleValueFFI::FfiExistingTransition> existing_transitions;
    Vector<NonnullRefPtr<StyleValue const>> custom_current_values;
    // A running transition of a custom property holds it at what the overlay of the target's environment answers. One
    // not sampled yet holds it at its start value.
    auto custom_current_value = [&](PropertyNameAndID const& property, CSSTransition const& transition) -> StyleValueFFI::StyleValueData const* {
        auto value = transition.transition_start_value();
        if (auto data = abstract_element.custom_property_data(); data && abstract_element.custom_property_data_is_animation_overlay()) {
            if (auto animated = data->get(property.name()); animated.has_value())
                value = *animated->value;
        }
        custom_current_values.append(value);
        return value->rust_style_value_data();
    };
    if (auto const* transitions = element.existing_transitions(pseudo_element)) {
        for (auto const& [property, transition] : *transitions) {
            bool running = !transition->is_finished() && !transition->is_idle();
            existing_transitions.append({
                .property_id = to_underlying(property.id()),
                .custom_name = property.is_custom_property() ? property.name().raw_identity() : 0,
                .custom_current_value = property.is_custom_property() && running ? custom_current_value(property, transition) : nullptr,
                .running = running,
                .end_value = transition->transition_end_value()->rust_style_value_data(),
                .reversing_adjusted_start_value = transition->reversing_adjusted_start_value()->rust_style_value_data(),
                .timing_function_output = running ? transition->timing_function_output_at_time(style_change_event_time) : 0,
                .reversing_shortening_factor = transition->reversing_shortening_factor(),
            });
        }
    }

    // A transition action is provisional until the stabilization epoch commits; a later pass of the epoch decides
    // again, superseding the decision of this one.
    auto decide = [&](ProvisionalTransitionState& state, StyleValueFFI::FfiTransitionActionKind action) {
        ++document().style_invalidation_counters().provisional_transition_decisions;
        if (state.has_decision)
            ++document().style_invalidation_counters().superseded_provisional_transition_decisions;
        state.has_decision = true;
        if (state.proposed_transition)
            state.proposed_transition->discard_provisional_transition();
        state.proposed_transition = nullptr;
        state.action = action;
    };
    auto ensure_state = [&](PropertyNameAndID const& property) -> ProvisionalTransitionState& {
        auto create_state = [&] {
            VERIFY(document().is_in_style_stabilization_epoch());
            m_provisional_transition_state_indices_by_target.ensure(target_key).append(m_provisional_transition_states.size());
            m_provisional_transition_states.append({
                .element = element,
                .pseudo_element = pseudo_element,
                .property = property,
                .committed_transition = element.property_transition(pseudo_element, property),
                .proposed_transition = nullptr,
                .action = StyleValueFFI::FfiTransitionActionKind::None,
                .has_decision = false,
            });
            return m_provisional_transition_states.size() - 1;
        };
        if (property.is_custom_property()) {
            auto indices = m_provisional_transition_state_indices_by_target.get(target_key);
            for (auto index : indices.has_value() ? indices->span() : ReadonlySpan<size_t> {}) {
                if (m_provisional_transition_states[index].property == property)
                    return m_provisional_transition_states[index];
            }
            return m_provisional_transition_states[create_state()];
        }
        auto index = m_provisional_transition_state_indices.ensure((target_key << 16) | to_underlying(property.id()), create_state);
        return m_provisional_transition_states[index];
    };
    auto property_of_action = [](StyleValueFFI::FfiTransitionAction const& action) {
        if (action.custom_name != 0)
            return PropertyNameAndID::from_name(Utf16FlyString::from_raw(action.custom_name)).release_value();
        return PropertyNameAndID::from_id(static_cast<PropertyID>(action.property_id));
    };
    auto adopt = [](StyleValueFFI::StyleValueData const* value) {
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(value));
    };

    GC::RootVector<GC::Ref<Animations::KeyframeEffect>> newly_started_transition_effects;
    GC::RootHashTable<Animations::KeyframeEffect*> replaced_transition_effects;
    auto on_actions = [&](ReadonlySpan<StyleValueFFI::FfiTransitionAction> actions) {
        for (auto const& action : actions) {
            auto property = property_of_action(action);
            auto& state = ensure_state(property);
            decide(state, action.kind);
            if (action.kind != StyleValueFFI::FfiTransitionActionKind::None && action.kind != StyleValueFFI::FfiTransitionActionKind::Start) {
                VERIFY(state.committed_transition);
                if (auto effect = state.committed_transition->effect(); effect && effect->is_keyframe_effect())
                    replaced_transition_effects.set(static_cast<Animations::KeyframeEffect*>(effect.ptr()));
            }
            if (!action.start_value)
                continue;
            auto transition = CSSTransition::start_a_provisional_transition(abstract_element, property, document().transition_generation(),
                action.delay, style_change_event_time, style_change_event_time + action.active_duration,
                adopt(action.start_value), adopt(action.end_value), adopt(action.reversing_adjusted_start_value),
                action.reversing_shortening_factor, EasingFunction::from_style_value(adopt(action.timing_function)));
            state.proposed_transition = transition;
            newly_started_transition_effects.append(as<Animations::KeyframeEffect>(*transition->effect()));
        }
        // A property an earlier pass of the epoch decided over that this one does not leaves its transitions as they are.
        auto indices = m_provisional_transition_state_indices_by_target.get(target_key);
        for (auto index : indices.has_value() ? indices->span() : ReadonlySpan<size_t> {}) {
            auto& state = m_provisional_transition_states[index];
            if (!any_of(actions, [&](auto const& action) { return property_of_action(action) == state.property; }))
                decide(state, StyleValueFFI::FfiTransitionActionKind::None);
        }
    };
    StyleValueFFI::FfiTransitionInput input {
        .existing_transitions = existing_transitions.data(),
        .existing_transition_count = existing_transitions.size(),
        .target_node = element.style_node_id().value(),
        .target_pseudo_kind = pseudo_element_to_ffi(pseudo_element),
        .element_box_slot = Layout::Node::slot_id(element.unsafe_layout_node(read)).index,
    };
    StyleEngineFFI::style_engine_decide_transitions(m_style_engine.host(), &read, before_change_style.record_pointer(), abstract_element.installed_style().record_pointer(), &input, [](void* context, StyleValueFFI::FfiTransitionAction const* actions, size_t count) { (*static_cast<decltype(on_actions)*>(context))({ actions, count }); }, &on_actions);

    // A transition action is provisional until the stabilization epoch commits, but the style
    // published by this pass must already reflect that decision. Rebuild the effect stack without
    // transitions which are being removed, then layer any proposed replacements on top.
    if (replaced_transition_effects.is_empty() && newly_started_transition_effects.is_empty())
        return {};

    // OPTIMIZATION: Transition decisions read the installed records directly. Reconstruct a working style only when
    //               changes to the effect stack require rebuilding the overlay.
    auto new_style = reconstruct_computed_properties_for_animation(abstract_element.installed_style());
    new_style->clear_animated_properties(Badge<StyleComputer> {});
    auto animations = abstract_element.element().get_animations_internal(
        Animations::Animatable::GetAnimationsSorted::Yes,
        Animations::Animatable::GetAnimationsOptions { .subtree = false, .pseudo_element = {} });
    if (animations.is_exception()) {
        dbgln("Error getting animations for element {}", abstract_element.debug_description());
    } else {
        GC::RootVector<GC::Ref<Animations::KeyframeEffect>> remaining_effects;
        for (auto& animation : animations.value()) {
            auto effect = animation->effect();
            if (!effect || !effect->is_keyframe_effect())
                continue;
            auto& keyframe_effect = static_cast<Animations::KeyframeEffect&>(*effect);
            if (keyframe_effect.pseudo_element_type() != abstract_element.pseudo_element())
                continue;
            if (replaced_transition_effects.contains(&keyframe_effect))
                continue;
            remaining_effects.append(keyframe_effect);
        }
        remaining_effects.extend(newly_started_transition_effects);
        quick_sort(remaining_effects, [](auto const& a, auto const& b) {
            return Animations::KeyframeEffect::composite_order(a, b) < 0;
        });
        if (!remaining_effects.is_empty())
            collect_animations_into(read, abstract_element, remaining_effects.span(), *new_style);
    }

    if (!newly_started_transition_effects.is_empty()) {
        // NB: Construction does not invalidate animated style because the effects were just evaluated. Request the
        //     first animation frame directly so timeline updates can schedule subsequent animated style updates.
        set_rendering_pending(*m_document, RenderingPendingReason::NeedsRenderingUpdate, 1);
        m_document->page().client().request_frame();
        if (!had_pending_animated_style_update)
            m_document->clear_needs_animated_style_update();
    }
    return new_style;
}

bool StyleComputer::has_provisional_transition_states(DOM::AbstractElement abstract_element) const
{
    return m_provisional_transition_state_indices_by_target.contains(transition_target_key(abstract_element));
}

// What composes over the record an element or pseudo-element holds: the animations associated with the element, and the
// transitions this stabilization epoch started for it, which are associated with it only once the epoch commits.
bool StyleComputer::has_animations_to_compose(DOM::AbstractElement abstract_element) const
{
    auto const& element = abstract_element.element();
    return element.has_relevant_animations() || element.has_associated_animations() || has_provisional_transition_states(abstract_element);
}

void StyleComputer::register_style_engine_sheet_source(StyleSheetState const& sheet)
{
    auto identity = Parser::ValueParserFFI::rust_style_sheet_identity(sheet.native_sheet().handle());
    m_style_engine_sheet_sources.set(identity, sheet.make_weak_ptr<StyleSheetState const>());
}

Optional<StyleEngineRuleTarget> StyleComputer::style_engine_rule_target(Layout::BegunRead const& read, StyleEngineRuleID rule_id) const
{
    StyleEngineFFI::FfiNativeRuleTarget target {};
    if (!StyleEngineFFI::style_engine_native_rule_target(m_style_engine.host(), &read, rule_id.value(), &target))
        return {};
    RustDeclarationBlockSnapshot declaration { static_cast<Parser::ValueParserFFI::DeclarationBlockHeader const*>(target.declarations) };
    auto source = m_style_engine_sheet_sources.get(target.source_identity);
    if (!source.has_value())
        return {};
    RefPtr<StyleSheetState const> source_sheet = *source;
    if (!source_sheet)
        return {};
    auto layer_name = Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(target.layer_name), target.layer_name_length });
    return StyleEngineRuleTarget {
        .rule_identity = target.identity,
        .declaration_version = target.declaration_version,
        .declaration = move(declaration),
        .source_style_sheet = move(source_sheet),
        .has_container_conditions = target.has_container_conditions,
        .qualified_layer_name = move(layer_name),
        .cascade_origin = static_cast<CascadeOrigin>(target.origin),
    };
}

StyleEngineRuleID StyleComputer::style_engine_rule_id_for(Layout::BegunRead const& read, RustRule const& rule) const
{
    return StyleEngineRuleID { StyleEngineFFI::style_engine_native_rule_id(m_style_engine.render_document().host(), &read, rule.identity()) };
}

SheetID StyleComputer::style_engine_sheet_id_for(StyleSheetState const& sheet) const
{
    if (sheet.constructed())
        return m_constructed_sheet_ids.get(&sheet).value_or(0);
    return sheet.style_engine_sheet_id();
}

void StyleComputer::set_style_engine_sheet_id_for(StyleSheetState& sheet, SheetID sheet_id)
{
    if (sheet.constructed())
        m_constructed_sheet_ids.set(&sheet, sheet_id);
    else
        sheet.set_style_engine_sheet_id(sheet_id);
}

Vector<StyleProperty> StyleComputer::collect_presentational_hint_properties(DOM::AbstractElement abstract_element)
{
    Vector<StyleProperty> properties;
    if (abstract_element.pseudo_element().has_value())
        return properties;

    auto& element = abstract_element.element();
    element.apply_presentational_hints(properties);
    if (element.supports_dimension_attributes()) {
        auto const& dimension_source = is<HTML::HTMLImageElement>(element)
            ? static_cast<HTML::HTMLImageElement const&>(element).dimension_attribute_source()
            : element;
        auto width = collect_dimension_attribute(properties, dimension_source, HTML::AttributeNames::width, CSS::PropertyID::Width);
        auto height = collect_dimension_attribute(properties, dimension_source, HTML::AttributeNames::height, CSS::PropertyID::Height);

        // The width and height attributes map to the aspect-ratio property (using dimension rules) on img and video
        // elements, and input elements with a type attribute in the Image Button state.
        // NB: An input element only supports dimension attributes in the Image Button state.
        if (is<HTML::HTMLImageElement>(element) || is<HTML::HTMLVideoElement>(element) || is<HTML::HTMLInputElement>(element))
            collect_aspect_ratio_from_dimension_values(properties, width, height);
    }
    HashTable<CSS::PropertyID> seen_properties;
    for (size_t i = properties.size(); i > 0; --i) {
        if (seen_properties.set(properties[i - 1].property_id) != AK::HashSetResult::InsertedNewEntry)
            properties.remove(i - 1);
    }
    // Which properties a hint decides is a fact about the element, and this is the one place that
    // knows it: mapping the attributes needs the element fully built, and for a table cell it needs
    // the table's computed style, so it cannot be done when the element arrives.
    if (element.presentational_hint_properties_need_publication(properties)
        && record_element_presentational_hint_properties(element, properties))
        element.did_publish_presentational_hint_properties(properties);
    return properties;
}

static OrderedHashMap<Utf16FlyString, StyleProperty> own_values_of_custom_property_store(void const* store)
{
    OrderedHashMap<Utf16FlyString, StyleProperty> own_values;
    ComputedValuesFFI::rust_custom_property_store_for_each_own_entry(store, &own_values, [](void* context, size_t name_raw, bool important, void const* data) {
        auto& own_values = *static_cast<OrderedHashMap<Utf16FlyString, StyleProperty>*>(context);
        own_values.set(
            Utf16FlyString::from_raw(name_raw),
            StyleProperty {
                .important = important ? Important::Yes : Important::No,
                .property_id = PropertyID::Custom,
                .value = StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data))),
            });
    });
    return own_values;
}

RefPtr<CustomPropertyData const> StyleComputer::engine_custom_property_declarations_over(Layout::BegunRead const& read, u64 identity, RefPtr<CustomPropertyData const> const& inherited) const
{
    if (!m_style_engine.is_engine_custom_property_environment(read, identity))
        return {};
    u64 parent_identity = 0;
    auto const* store = StyleEngineFFI::style_engine_borrow_engine_custom_property_environment(m_style_engine.host(), &read, identity, &parent_identity);
    if (!store)
        return {};
    auto own_values = own_values_of_custom_property_store(store);
    ComputedValuesFFI::rust_custom_property_store_destroy(store);
    return CustomPropertyData::create(move(own_values), inherited);
}

RefPtr<CustomPropertyData const> StyleComputer::engine_custom_property_environment(Layout::BegunRead const& read, u64 identity, RefPtr<CustomPropertyData const> const& inherited) const
{
    if (!m_style_engine.is_engine_custom_property_environment(read, identity))
        return {};
    u64 parent_identity = 0;
    auto const* store = StyleEngineFFI::style_engine_borrow_engine_custom_property_environment(m_style_engine.host(), &read, identity, &parent_identity);
    if (!store)
        return {};
    if (parent_identity != (inherited ? inherited->identity() : 0)) {
        ComputedValuesFFI::rust_custom_property_store_destroy(store);
        return {};
    }
    // NB: The environment is the store, whose reference the engine handed over.
    return CustomPropertyData::create({}, inherited, store);
}

RefPtr<CustomPropertyData const> StyleComputer::inheritable_custom_property_environment(Layout::BegunRead const& read, CustomPropertyData const& environment) const
{
    // NB: What an environment the style engine resolved hands down is the engine's to name, under the registrations as
    //     they are now: the environments its records resolve over the element's are built over that one.
    if (m_style_engine.is_engine_custom_property_environment(read, environment.identity())) {
        auto inheritable = StyleEngineFFI::style_engine_inheritable_custom_property_environment(m_style_engine.host(), &read, environment.identity());
        if (!inheritable)
            return nullptr;
        return CustomPropertyData::of_store(reinterpret_cast<void const*>(static_cast<FlatPtr>(inheritable)));
    }

    // What one built here hands down is kept by its identity, under the registrations it was made under, so that every
    // child inherits the same environment.
    auto generation = document().custom_property_registration_generation();
    if (m_inheritable_custom_property_environments_generation != generation) {
        m_inheritable_custom_property_environments.clear();
        m_inheritable_custom_property_environments_generation = generation;
    }
    if (auto existing = m_inheritable_custom_property_environments.get(environment.identity()); existing.has_value())
        return existing->hands_down_itself ? RefPtr<CustomPropertyData const> { environment } : existing->inheritable;

    RefPtr<CustomPropertyData const> inheritable_parent;
    if (auto parent = environment.parent())
        inheritable_parent = inheritable_custom_property_environment(read, *parent);
    auto inheritable = environment.inheritable_impl(
        inheritable_parent,
        [&](Utf16FlyString const& name) { return document().get_registered_custom_property(name); });
    bool const hands_down_itself = inheritable.ptr() == &environment;
    m_inheritable_custom_property_environments.set(environment.identity(),
        InheritableCustomPropertyEnvironment {
            .environment = environment,
            .inheritable = hands_down_itself ? nullptr : inheritable,
            .hands_down_itself = hands_down_itself,
        });
    return inheritable;
}

// An environment nothing but the table holds is one no element is in, and the table is the only
// thing keeping it - and what it hands down - alive.
void StyleComputer::sweep_custom_property_environments() const
{
    m_inheritable_custom_property_environments.remove_all_matching([](auto&, InheritableCustomPropertyEnvironment const& entry) { return entry.environment->ref_count() == 1; });
}

void StyleComputer::update_root_element_font_metrics(ComputedValues const& values)
{
    m_root_element_font_metrics = Length::FontMetrics { values.font_size(), values.font_list().first_available_font().pixel_metrics(), values.line_height() };
    m_root_element_font_metrics_depend_on_viewport_metrics = values.font_metrics_depend_on_viewport_metrics();
    m_root_element_effective_zoom = values.effective_zoom();
}

CSSPixels StyleComputer::default_user_font_size()
{
    // FIXME: This value should be configurable by the user.
    return 16;
}

// https://w3c.github.io/csswg-drafts/css-fonts/#absolute-size-mapping
CSSPixels StyleComputer::absolute_size_mapping(AbsoluteSize absolute_size, CSSPixels default_font_size)
{
    // An <absolute-size> keyword refers to an entry in a table of font sizes computed and kept by the user agent. See
    // § 2.5.1 Absolute Size Keyword Mapping Table.
    // INTEROP: Other browsers share this keyword table for integral default sizes from 9px through 16px, so we use
    //          it instead of the ratios from the spec's guideline table.
    static constexpr Array<Array<u8, 8>, 8> font_size_table { {
        { 9, 9, 9, 9, 11, 14, 18, 27 },
        { 9, 9, 9, 10, 12, 15, 20, 30 },
        { 9, 9, 10, 11, 13, 17, 22, 33 },
        { 9, 9, 10, 12, 14, 18, 24, 36 },
        { 9, 10, 12, 13, 16, 20, 26, 39 },
        { 9, 10, 12, 14, 17, 21, 28, 42 },
        { 9, 10, 13, 15, 18, 23, 30, 45 },
        { 9, 10, 13, 16, 18, 24, 32, 48 },
    } };
    auto default_size = default_font_size.to_int();
    if (default_size >= 9 && default_size <= 16 && default_font_size == CSSPixels(default_size))
        return CSSPixels(font_size_table[default_size - 9][to_underlying(absolute_size)]);

    switch (absolute_size) {
    case AbsoluteSize::XxSmall:
        return default_font_size * CSSPixels(3) / 5;
    case AbsoluteSize::XSmall:
        return default_font_size * CSSPixels(3) / 4;
    case AbsoluteSize::Small:
        return default_font_size * CSSPixels(8) / 9;
    case AbsoluteSize::Medium:
        return default_font_size;
    case AbsoluteSize::Large:
        return default_font_size * CSSPixels(6) / 5;
    case AbsoluteSize::XLarge:
        return default_font_size * CSSPixels(3) / 2;
    case AbsoluteSize::XxLarge:
        return default_font_size * 2;
    case AbsoluteSize::XxxLarge:
        return default_font_size * 3;
    }

    VERIFY_NOT_REACHED();
}

ComputationContext StyleComputer::make_computation_context_for_property(PropertyID property_id, ComputedStyleWorkingSet const& style, Optional<DOM::AbstractElement> abstract_element) const
{
    auto subject_inline_axis_is_horizontal = [&]() {
        auto writing_mode = [&](DOM::AbstractElement const& candidate) -> Optional<WritingMode> {
            auto record = candidate.installed_style().view();
            if (!record.present)
                return {};
            return writing_mode_from_inherited_bits(style_record_inherited_values(record.groups).bits);
        };
        if (!abstract_element.has_value())
            return true;
        if (auto mode = writing_mode(*abstract_element); mode.has_value())
            return *mode == WritingMode::HorizontalTb;
        if (auto inheritance_parent = abstract_element->element_to_inherit_style_from(); inheritance_parent.has_value() && inheritance_parent->has_style())
            return writing_mode(*inheritance_parent).value_or(WritingMode::HorizontalTb) == WritingMode::HorizontalTb;
        return true;
    }();

    bool const is_document_element = abstract_element.has_value()
        && !abstract_element->pseudo_element().has_value()
        && abstract_element->element().is_document_element();

    switch (property_id) {
    // FIXME: While `color-scheme` doesn't actually require a computation context (since it only takes keyword values),
    //        callers request one uniformly. Since `color-scheme` must be computed before creating a generic computation
    //        context, use the font context instead.
    case PropertyID::ColorScheme:
    case PropertyID::FontFamily:
    case PropertyID::FontFeatureSettings:
    case PropertyID::FontKerning:
    case PropertyID::FontOpticalSizing:
    case PropertyID::FontSize:
    case PropertyID::FontSizeAdjust:
    case PropertyID::FontStyle:
    case PropertyID::FontVariantAlternates:
    case PropertyID::FontVariantCaps:
    case PropertyID::FontVariantEastAsian:
    case PropertyID::FontVariantEmoji:
    case PropertyID::FontVariantLigatures:
    case PropertyID::FontVariantNumeric:
    case PropertyID::FontVariantPosition:
    case PropertyID::FontVariationSettings:
    case PropertyID::FontWeight:
    case PropertyID::FontWidth:
    case PropertyID::MathDepth:
    case PropertyID::TextRendering: {
        auto inheritance_parent = abstract_element.map([](auto& element) { return element.element_to_inherit_style_from(); }).value_or(OptionalNone {});
        auto length_resolution_context = inheritance_parent.has_value() && inheritance_parent->has_style() && inheritance_parent->element().navigable()
            ? Length::ResolutionContext::for_element(inheritance_parent.value())
            : Length::ResolutionContext::for_document(m_document);
        length_resolution_context.subject_inline_axis_is_horizontal = subject_inline_axis_is_horizontal;
        length_resolution_context.subject_element = abstract_element.has_value() ? &abstract_element->element() : nullptr;
        length_resolution_context.subject_is_pseudo_element = abstract_element.has_value() && abstract_element->pseudo_element().has_value();

        return {
            .length_resolution_context = length_resolution_context,
            .abstract_element = abstract_element
        };
    }
    case PropertyID::LineHeight: {
        auto inheritance_parent = abstract_element.map([](auto& element) { return element.element_to_inherit_style_from(); }).value_or(OptionalNone {});

        auto line_height_font_metrics = Length::FontMetrics {
            style.font_size(),
            style.first_available_computed_font(document().font_computer())->pixel_metrics(),
            inheritance_parent.has_value() && inheritance_parent->has_style() ? inheritance_parent->computed_style()->line_height() : normal_line_height(document().font_computer().initial_font().pixel_metrics())
        };

        return {
            .length_resolution_context = {
                .viewport_rect = viewport_rect(),
                .font_metrics = line_height_font_metrics,
                .root_font_metrics = is_document_element
                    ? line_height_font_metrics
                    : m_root_element_font_metrics,
                .font_metrics_depend_on_viewport_metrics = style.font_metrics_depend_on_viewport_metrics(),
                .root_font_metrics_depend_on_viewport_metrics = is_document_element
                    ? style.font_metrics_depend_on_viewport_metrics()
                    : m_root_element_font_metrics_depend_on_viewport_metrics,
                .subject_inline_axis_is_horizontal = subject_inline_axis_is_horizontal,
                .subject_is_pseudo_element = abstract_element.has_value() && abstract_element->pseudo_element().has_value(),
                .subject_element = abstract_element.has_value() ? &abstract_element->element() : nullptr,
            },
            .abstract_element = abstract_element
        };
    }
    default: {
        auto font_metrics = Length::FontMetrics {
            style.font_size(),
            style.first_available_computed_font(document().font_computer())->pixel_metrics(),
            style.line_height(document().font_computer())
        };
        return {
            .length_resolution_context = {
                .viewport_rect = viewport_rect(),
                .font_metrics = font_metrics,
                .root_font_metrics = is_document_element ? font_metrics : m_root_element_font_metrics,
                .font_metrics_depend_on_viewport_metrics = style.font_metrics_depend_on_viewport_metrics(),
                .root_font_metrics_depend_on_viewport_metrics = is_document_element ? style.font_metrics_depend_on_viewport_metrics() : m_root_element_font_metrics_depend_on_viewport_metrics,
                .subject_inline_axis_is_horizontal = subject_inline_axis_is_horizontal,
                .subject_is_pseudo_element = abstract_element.has_value() && abstract_element->pseudo_element().has_value(),
                .subject_element = abstract_element.has_value() ? &abstract_element->element() : nullptr,
            },
            .abstract_element = abstract_element,
            .color_scheme = style.color_scheme(document().page().preferred_color_scheme(), document().supported_color_schemes())
        };
    }
    }

    VERIFY_NOT_REACHED();
}

static ComputedValuesFFI::FfiInputLineHeightMetrics input_line_height_metrics(ComputedStyleWorkingSet const& style, DOM::AbstractElement abstract_element, bool should_measure)
{
    ComputedValuesFFI::FfiInputLineHeightMetrics line_height_metrics {};
    if (should_measure) {
        line_height_metrics.current_line_height = style.line_height(abstract_element.element().document().font_computer()).to_double();
        line_height_metrics.minimum_line_height = normal_line_height(style.first_available_computed_font(abstract_element.element().document().font_computer())->pixel_metrics()).to_double();
    }
    return line_height_metrics;
}

void StyleComputer::finalize_animated_box_type(Layout::BegunRead const& read, ComputedStyleWorkingSet& style, DOM::AbstractElement abstract_element) const
{
    // A composition sampled over a record is transformed against the input the engine drove the record against.
    auto box_type = ComputedValuesFFI::rust_animated_box_type_transformation_input(m_style_engine.host(), &read, abstract_element.element().style_node_id().value(), pseudo_element_to_ffi(abstract_element.pseudo_element()));
    auto line_height_metrics = input_line_height_metrics(style, abstract_element, box_type.check_input_line_height);
    auto* animated_overlay = style.prepare_animated_overlay_for_rust_finalization(Badge<StyleComputer> {});
    ComputedValuesFFI::rust_finalize_animated_box_type(box_type, style.record(), bit_cast<u32>(style.display_before_box_type_transformation()), animated_overlay, &line_height_metrics);
    style.finish_animated_overlay_rust_mutation(Badge<StyleComputer> {});
}

StyleRecordID StyleComputer::intern_document_style(Layout::BegunRead const& read) const
{
    Vector<u8> document_supported_color_scheme_codes;
    auto document_supported_color_schemes = document().supported_color_schemes();
    if (document_supported_color_schemes.has_value()) {
        document_supported_color_scheme_codes.ensure_capacity(document_supported_color_schemes->size());
        for (auto const& scheme : *document_supported_color_schemes)
            document_supported_color_scheme_codes.unchecked_append(to_underlying(preferred_color_scheme_from_string(scheme)));
    }
    auto length_resolution_context = CSS::Length::ResolutionContext::for_document(document());
    auto viewport_rect = this->viewport_rect();
    ComputedValuesFFI::FfiDocumentLonghandInput const input {
        .color_scheme_input = {
            .preferred_color_scheme = static_cast<u8>(to_underlying(document().page().preferred_color_scheme())),
            .has_document_supported_schemes = document_supported_color_schemes.has_value(),
            .document_supported_scheme_codes = document_supported_color_scheme_codes.data(),
            .document_supported_scheme_count = document_supported_color_scheme_codes.size(),
        },
        .length_resolution_context = to_ffi_length_resolution_context(length_resolution_context),
        .device_pixels_per_css_pixel = m_document->page().client().device_pixels_per_css_pixel(),
        .initial_font_size_raw = InitialValues::font_size().raw_value(),
        .default_font_size_raw = default_user_font_size().raw_value(),
        .viewport_width = viewport_rect.width().to_double(),
        .viewport_height = viewport_rect.height().to_double(),
    };
    auto computed_properties = CSS::ComputedStyleWorkingSet::create_over_record(StyleRecordRef::adopt(ComputedValuesFFI::rust_create_document_style_record(m_style_engine.host(), &read, &input)));
    // The font group inputs borrow the font list the working set resolves, so it outlives the call.
    auto font_group_inputs = computed_properties->font_group_build_inputs(document(), document().style_scope().style_engine_tree_scope());
    return StyleRecordID { ComputedValuesFFI::rust_intern_document_style(m_style_engine.host(), &read, computed_properties->record(), &font_group_inputs, &input.length_resolution_context) };
}

void StyleComputer::publish_sampled_animation_overlays(Layout::BegunRead const& read, ReadonlySpan<SampledAnimationOverlay> overlays, Span<StyleEngineFFI::FfiAnimationOverlayPublication> publications) const
{
    // The engine composes each overlay over the record the element installed, rebuilding only the groups the overlay
    // writes, compares it with that record, and publishes it, answering the view of the record it published. The
    // animated platform font is the one thing it asks for.
    VERIFY(overlays.size() == publications.size());
    struct OverlayFont {
        ComputedStyleWorkingSet const& style;
        GC::Ref<DOM::Document const> document;
        TreeScopeID tree_scope;
    };
    GC::ConservativeVector<OverlayFont, 1> fonts;
    Vector<StyleEngineFFI::FfiAnimationOverlayPublicationInput, 1> inputs;
    fonts.ensure_capacity(overlays.size());
    inputs.ensure_capacity(overlays.size());
    auto const used_color_scheme_preference = document().page().preferred_color_scheme();
    for (auto const& [abstract_element, style] : overlays) {
        auto& element = abstract_element.element();
        fonts.unchecked_append({ style, document(), abstract_element.style_scope().style_engine_tree_scope() });
        auto animated_properties = style.animated_properties_snapshot();
        bool const publishes_overlay = animated_properties && !animated_properties->is_empty();
        auto custom_property_data = abstract_element.custom_property_data();
        inputs.unchecked_append({
            .style_node = element.style_node_id().value(),
            .pseudo_kind = pseudo_element_to_ffi(abstract_element.pseudo_element()),
            .style_record = abstract_element.style_record_identity().value(),
            .record = style.record(),
            .animated_overlay = style.animated_overlay(),
            .animation_overlay_identity = publishes_overlay ? animated_properties->identity() : 0,
            .used_color_scheme = static_cast<u8>(to_underlying(style.color_scheme(used_color_scheme_preference, document().supported_color_schemes()))),
            .display_before_box_type_transformation_raw = bit_cast<u32>(style.display_before_box_type_transformation()),
            .is_document_element = !abstract_element.pseudo_element().has_value() && element.is_document_element(),
            .custom_property_environment = custom_property_data ? custom_property_data->identity() : 0,
            .custom_property_store = custom_property_data ? custom_property_data->rust_store() : nullptr,
            .callback_context = &fonts.last(),
            .font_group_inputs = [](void* context, void* inputs) {
                auto const& font = *static_cast<OverlayFont const*>(context);
                *static_cast<ComputedValuesFFI::FfiFontGroupBuildInputs*>(inputs) = font.style.font_group_build_inputs(*font.document, font.tree_scope);
            },
        });
    }
    // NB: The engine counts the samples it composes, by whether they rebuilt every group.
    StyleEngineFFI::style_engine_publish_sampled_animation_overlays(m_style_engine.host(), &read, inputs.data(), inputs.size(), publications.data());
    for (auto const& published : publications)
        VERIFY(published.record);
}

StyleRecordID StyleComputer::intern_computed_style_inputs(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, ComputedValues const& values) const
{
    return install_computed_style_inputs(read, Optional<DOM::AbstractElement> { abstract_element }, values);
}

StyleRecordID StyleComputer::intern_anonymous_layout_style(Layout::BegunRead const& read, ComputedValues const& values) const
{
    return install_computed_style_inputs(read, {}, values);
}

StyleRecordID StyleComputer::install_computed_style_inputs(Layout::BegunRead const& read, Optional<DOM::AbstractElement> abstract_element, ComputedValues const& values) const
{
    auto const& base = values.base_values();
    // A record no element is assigned cannot own an animation overlay, so a layout-derived copy of an
    // animated style installs its final merged payloads directly. Splitting off the base there
    // would install (and paint) the un-animated values.
    bool const has_animations = values.has_animated_values() || values.animated_properties();
    auto const& payload_source = has_animations ? values : base;
    Array<void const*, to_underlying(StyleGroupIndex::Count)> payloads;
    for (size_t index = 0; index < payloads.size(); ++index)
        payloads[index] = payload_source.style_group_payload(static_cast<StyleGroupIndex>(index));
    auto custom_property_environment = abstract_element.has_value() ? abstract_element->custom_property_data() : nullptr;
    u64 counter_style_environment_identity = 0;
    if (abstract_element.has_value()
        && base.reads_counter_style_environment(abstract_element->pseudo_element().has_value()))
        counter_style_environment_identity = abstract_element->style_scope().counter_style_environment_identity(read);
    return const_cast<StyleComputer&>(*this).style_engine().install_host_style_record(read, payloads, payload_source.inherited_set_values(), payload_source.reset_bits(), custom_property_environment ? custom_property_environment->identity() : 0, counter_style_environment_identity, base.record().get(), custom_property_environment ? custom_property_environment->rust_store() : nullptr);
}

RefPtr<ComputedValues const> StyleComputer::engine_transient_pseudo_element_style(Layout::BegunRead const& read, DOM::Element const& element, StyleEngine::DemandedPseudoElement pseudo_element, StyleAtomID highlight_name)
{
    auto answer = StyleEngineFFI::style_engine_answer_pseudo_element_record_demand(m_style_engine.host(), &read, element.style_node_id().value(), StyleEngine::PseudoElementRecordDemand::ReadOnly, pseudo_element, highlight_name.value());
    InstalledStyle const style { answer.record.host_record };
    if (!style)
        return {};
    auto view = computed_style_record_view(style);
    // The engine holds the record only until the element's styles are next read or settled.
    return ComputedValues::Builder { *view }.build();
}

NonnullRefPtr<ComputedStyleWorkingSet> StyleComputer::reconstruct_computed_properties(ComputedValues const& computed_values) const
{
    auto style = ComputedStyleWorkingSet::create_with_base_values_from(computed_values);
    // The recorded pre-box-type-transformation display tracks the animated display while one is applied, on both
    // the animated style and its base. When the animation stops covering `display`, re-adjustment must start over
    // from the base style's display, or the sampled value the finished animation left behind is resurrected as
    // the element's display. Box-type transformations are idempotent, so the adjusted base display is a sound
    // transformation input.
    if (auto const* animated_properties = computed_values.animated_properties(); animated_properties && animated_properties->has_property(PropertyID::Display))
        style->set_display_before_box_type_transformation(computed_values.base_values().display());
    apply_animated_properties_to_reconstruction(*style, computed_values);
    return style;
}

void StyleComputer::apply_animated_properties_to_reconstruction(ComputedStyleWorkingSet& style, ComputedValues const& computed_values) const
{
    auto const* animated_properties = computed_values.animated_properties();
    if (!animated_properties)
        return;
    style.install_animated_overlay(Badge<StyleComputer> {}, animated_properties->overlay());
}

NonnullRefPtr<ComputedStyleWorkingSet> StyleComputer::reconstruct_computed_properties_for_animation(InstalledStyle const& installed_style) const
{
    auto record = installed_style.view();
    VERIFY(record.present);
    auto style = ComputedStyleWorkingSet::create_for_animation_update(
        record.longhand_table_record,
        static_cast<ComputedValuesFFI::AnimatedOverlay const*>(record.animated_overlay));
    if (record.animated_overlay && ComputedValuesFFI::rust_animated_overlay_contains(static_cast<ComputedValuesFFI::AnimatedOverlay const*>(record.animated_overlay), to_underlying(PropertyID::Display))) {
        auto const* box = &style_record_group<ComputedValues::BoxValues>(StyleEngineFFI::style_engine_view_base_groups(&record));
        style->set_display_before_box_type_transformation(display_from_ffi_display(box->display));
    }
    return style;
}

u64 StyleComputer::style_environment_version_for_sharing() const
{
    return document().style_environment_version() ^ (m_viewport_environment_version << 32);
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_font_size(NonnullRefPtr<StyleValue const> const& absolutized_value, int computed_math_depth, Optional<DOM::AbstractElement> const& inheritance_parent, CSSPixels initial_font_size)
{
    auto inherited_font_size = inheritance_parent.has_value() && inheritance_parent->has_style()
        ? inheritance_parent->computed_style()->font_size()
        : initial_font_size;

    auto inherited_math_depth = inheritance_parent.has_value() && inheritance_parent->has_style()
        ? inheritance_parent->computed_style()->math_depth()
        : InitialValues::math_depth();

    // The size keyword tables and the math scaling rules live in the Rust style computation core.
    auto result = ComputedValuesFFI::rust_compute_font_size(absolutized_value->rust_style_value_data(), computed_math_depth, inherited_font_size.raw_value(), inherited_math_depth, default_user_font_size().raw_value());
    if (result.handled) {
        if (result.unchanged)
            return absolutized_value;
        return LengthStyleValue::create(Length::make_px(result.value));
    }

    VERIFY(absolutized_value->is_calculated());
    return LengthStyleValue::create(absolutized_value->as_calculated().resolve_length({ .percentage_basis = Length::make_px(inherited_font_size) }).value());
}

// The FontStyleKeyword discriminants cross the boundary as the mapped keyword code; pin them.
static_assert(to_underlying(FontStyleKeyword::Normal) == 0);
static_assert(to_underlying(FontStyleKeyword::Italic) == 1);
static_assert(to_underlying(FontStyleKeyword::Left) == 2);
static_assert(to_underlying(FontStyleKeyword::Right) == 3);
static_assert(to_underlying(FontStyleKeyword::Oblique) == 4);

NonnullRefPtr<StyleValue const> StyleComputer::compute_font_style(NonnullRefPtr<StyleValue const> const& absolutized_value)
{
    // https://drafts.csswg.org/css-fonts-4/#font-style-prop
    // the keyword specified, plus angle in degrees if specified

    // The keyword-to-font-style-keyword mapping lives in the Rust style computation core.
    // NB: We always parse as a FontStyleStyleValue, but StylePropertyMap is able to set a KeywordStyleValue directly.
    auto computation = ComputedValuesFFI::rust_compute_font_style(absolutized_value->rust_style_value_data());
    if (computation.is_keyword)
        return FontStyleStyleValue::create(static_cast<FontStyleKeyword>(computation.font_style_keyword));

    return absolutized_value;
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_font_weight(NonnullRefPtr<StyleValue const> const& absolutized_value, Optional<DOM::AbstractElement> const& inheritance_parent)
{
    auto inherited_font_weight = inheritance_parent.has_value() && inheritance_parent->has_style()
        ? inheritance_parent->computed_style()->font_weight()
        : InitialValues::font_weight();

    // The weight chart lives in the Rust style computation core.
    auto result = ComputedValuesFFI::rust_compute_font_weight(absolutized_value->rust_style_value_data(), inherited_font_weight);
    if (result.handled) {
        if (result.unchanged)
            return absolutized_value;
        return NumberStyleValue::create(result.value);
    }

    // AD-HOC: Anywhere we support a numbers we should also support calcs
    VERIFY(absolutized_value->is_calculated());
    return NumberStyleValue::create(absolutized_value->as_calculated().resolve_number({}).value());
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_font_width(NonnullRefPtr<StyleValue const> const& absolutized_value)
{
    // The width keyword percentage table lives in the Rust style computation core.
    auto result = ComputedValuesFFI::rust_compute_font_width(absolutized_value->rust_style_value_data());
    if (result.handled) {
        if (result.unchanged)
            return absolutized_value;
        return PercentageStyleValue::create(Percentage(result.value));
    }

    // AD-HOC: We support calculated percentages as well
    VERIFY(absolutized_value->is_calculated());
    return PercentageStyleValue::create(absolutized_value->as_calculated().resolve_percentage({}).value());
}

}
