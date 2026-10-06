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
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/CSS/AnimationEvent.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/CSS/CSSLayerStatementRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSScopeRule.h>
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
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
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
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PendingSubstitutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
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
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/StyleValueRustFFI.h>
#include <LibWeb/ValueParserRustFFI.h>
#include <math.h>

namespace Web::CSS {

static Utf16View utf16_view(ComputedValuesFFI::FfiUtf16View view)
{
    if (view.length == 0)
        return {};
    VERIFY((view.ascii == nullptr) != (view.utf16 == nullptr));
    if (view.ascii)
        return StringView { reinterpret_cast<char const*>(view.ascii), view.length };
    return { reinterpret_cast<char16_t const*>(view.utf16), view.length };
}

// The custom properties the style queries of a substitution read are the element's style query
// references, as a style query in a container condition records them.
static void record_style_query_dependencies(DOM::AbstractElement element, void* dependencies)
{
    ComputedValuesFFI::rust_style_query_dependencies_take(dependencies, &element, [](void* context, ComputedValuesFFI::FfiUtf16View name) {
        auto& element = *static_cast<DOM::AbstractElement*>(context);
        element.element().record_style_query_custom_property_reference(element.pseudo_element(), Utf16FlyString::from_utf16(utf16_view(name)));
    });
}

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

// What a rule contributes, for the two rule types that carry a declaration block.
static RustDeclarationBlock const& declaration_of_rule(CSSRule const& rule)
{
    if (rule.type() == CSSRule::Type::Style)
        return static_cast<CSSStyleRule const&>(rule).declaration();
    if (rule.type() == CSSRule::Type::NestedDeclarations)
        return static_cast<CSSNestedDeclarations const&>(rule).declaration();
    VERIFY_NOT_REACHED();
}

StyleComputer::StyleComputer(DOM::Document& document)
    : m_document(document)
    , m_default_font_metrics(16, Platform::FontPlugin::the().default_font(16)->pixel_metrics(), InitialValues::line_height())
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

ComputedStyleRecordView StyleComputer::computed_style_record_view(Layout::BegunRead const& read, StyleRecordID style_record_identity) const
{
    return computed_style_record_view(install_style(read, style_record_identity));
}

ComputedStyleRecordView StyleComputer::computed_style_record_view(InstalledStyle const& style) const
{
    if (!style)
        return {};
    // A view held across a later install keeps the record it borrows from live.
    bool owns_style_record_pin = m_style_record_view_epoch_depth == 0 || style.view().animation_overlay_identity != 0;
    if (owns_style_record_pin) {
        pin_style_record(style.record());
        ++m_computed_style_record_view_pin_count;
    }
    return ComputedStyleRecordView { style.view(), *this, style.record(), owns_style_record_pin };
}

InstalledStyle StyleComputer::install_style(Layout::BegunRead const& read, StyleRecordID style_record) const
{
    if (!style_record)
        return {};
    return { style_record, m_style_engine.style_record_view(read, style_record) };
}

void StyleComputer::pin_style_record(StyleRecordID style_record_identity) const
{
    VERIFY(style_record_identity);
    StyleEngineFFI::style_engine_pin_style_record(m_style_engine.host(), style_record_identity.value());
}

void StyleComputer::unpin_style_record(StyleRecordID style_record_identity) const
{
    VERIFY(style_record_identity);
    StyleEngineFFI::style_engine_unpin_style_record(m_style_engine.host(), style_record_identity.value());
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
    // Registration is where an element's publications keyed by its style node begin: an attribute written before it
    // had one published nothing.
    if (auto* svg_element = as_if<SVG::SVGElement>(node))
        Layout::publish_svg_attribute_facts(*svg_element);
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
        StyleEngineFFI::style_engine_consume_element_style_input(m_style_engine.host(), style_node_id);
        m_style_engine.note_style_node_arrived_or_retired(style_node_id);
    }
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

void StyleComputer::for_each_style_node(Function<void(DOM::Element&)> callback) const
{
    for (auto node : m_element_style_nodes) {
        if (auto* element = as_if<DOM::Element>(node.ptr()))
            callback(*element);
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

void StyleComputer::record_transition_stabilization_baseline(DOM::AbstractElement abstract_element, StyleRecordID before_change_style_record) const
{
    auto style_node_id = abstract_element.element().style_node_id();
    if (style_node_id == 0)
        return;
    // Few epochs record a baseline, so the engine keeps them only for one that does.
    auto& style_engine = const_cast<StyleEngine&>(m_style_engine);
    if (!exchange(m_transition_baselines_recorded, true))
        StyleEngineFFI::style_engine_begin_transition_baselines(style_engine.host());
    StyleEngineFFI::style_engine_record_transition_baseline(style_engine.host(), style_node_id, pseudo_element_to_ffi(abstract_element.pseudo_element()), before_change_style_record.value());
}

// https://drafts.csswg.org/css-transitions-2/#defining-before-change-style
// A later pass of this stabilization epoch can give the element a transition it does not declare yet, which is decided
// against the style the element moved away from in this one. Size container queries and the feedback epoch run such
// passes.
void StyleComputer::record_transition_baseline_for_later_passes(DOM::AbstractElement abstract_element, StyleRecordID before_change_style_record) const
{
    if (abstract_element.style_scope().rule_cache().has_size_container_queries || document().is_in_style_stabilization_feedback_epoch())
        record_transition_stabilization_baseline(abstract_element, before_change_style_record);
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
            if (element.property_transition(state.pseudo_element, state.property_id) == state.committed_transition)
                element.remove_transition(state.pseudo_element, state.property_id);
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

void StyleComputer::for_each_property_expanding_shorthands(PropertyID property_id, StyleValue const& value, Function<void(PropertyID, StyleValue const&)> const& set_longhand_property)
{
    // The expansion recursion and pending-substitution values live in the Rust style value graph.
    // This wrapper creates C++ facades only for the returned longhand roots.
    auto expansion = ComputedValuesFFI::rust_expand_property_shorthands(
        to_underlying(property_id), value.rust_style_value_data());
    ScopeGuard destroy_expansion = [&] {
        ComputedValuesFFI::rust_shorthand_expansion_destroy(expansion.storage);
    };
    HashMap<void const*, NonnullRefPtr<StyleValue const>> wrapper_cache;
    for (size_t i = 0; i < expansion.count; ++i) {
        auto const& property = expansion.properties[i];
        auto& expanded_value = wrapper_cache.ensure(property.data, [&] {
            return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(
                static_cast<StyleValueFFI::StyleValueData const*>(property.data)));
        });
        set_longhand_property(static_cast<PropertyID>(property.property_id), *expanded_value);
    }
}

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

    AnimationSample(GC::Ref<StyleComputer const> style_computer, DOM::AbstractElement abstract_element, ComputedStyleWorkingSet& computed_properties, Layout::BegunRead const& read)
        : context { style_computer, abstract_element, computed_properties, read }
    {
    }

    struct Context {
        GC::Ref<StyleComputer const> style_computer;
        DOM::AbstractElement abstract_element;
        ComputedStyleWorkingSet& computed_properties;
        Layout::BegunRead const& read;
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

void StyleComputer::collect_animations_into(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>> effects, ComputedStyleWorkingSet& computed_properties, AnimationRefresh refresh) const
{
    if (refresh == AnimationRefresh::No) {
        collect_animation_effects_into(read, abstract_element, effects, computed_properties, {});
        publish_animated_custom_properties(computed_properties, abstract_element);
        return;
    }
    m_keyframes_inherited_non_inherited_style_groups = 0;
    // A refresh samples over the record the element holds, which the working set was reconstructed from.
    collect_animation_effects_into(read, abstract_element, effects, computed_properties, abstract_element.style_record_identity());
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
        samples.unchecked_append(begin_animation_sample(read, request.abstract_element, request.effects, request.computed_properties, request.abstract_element.style_record_identity()));
        inputs.unchecked_append(samples.last()->input);
    }
    Vector<ComputedValuesFFI::FfiHostAnimationSampleResult> results;
    results.resize(inputs.size());
    ComputedValuesFFI::rust_sample_animation_effects_each(inputs.data(), inputs.size(), &read, results.data());
    for (size_t index = 0; index < requests.size(); ++index) {
        auto const& request = requests[index];
        m_keyframes_inherited_non_inherited_style_groups = 0;
        finish_animation_sample(*samples[index], results[index]);
        finish_animation_refresh(read, request.abstract_element, request.computed_properties);
    }
}

void StyleComputer::finish_animation_refresh(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, ComputedStyleWorkingSet& computed_properties) const
{
    publish_animated_custom_properties(computed_properties, abstract_element);
    // An animation-only overlay update resolves keyframe values just like a full style computation does, so a
    // keyframe-borne `inherit` on a non-inherited property discovered here must leave the same invalidation
    // mark behind, or a later change to the parent's value never reaches this element's animated style.
    if (m_keyframes_inherited_non_inherited_style_groups != 0) {
        if (auto* parent = abstract_element.element().parent())
            parent->add_children_explicitly_inherited_non_inherited_style_groups(m_keyframes_inherited_non_inherited_style_groups);
        m_keyframes_inherited_non_inherited_style_groups = 0;
    }
    if (computed_properties.requires_animated_post_compute_adjustments()) {
        computed_properties.prepare_for_animated_post_compute_adjustments(Badge<StyleComputer> {});
        finalize_animated_box_type(read, computed_properties, abstract_element);
    }
}

// The timing the style engine computes the key an effect samples its keyframes at from: what its animation contributes,
// the effect's own timing, and its timeline's current time. The engine decides it only where every time is in one unit:
// a duration, or a percentage of a scroll timeline's progress.
static ComputedValuesFFI::FfiEffectTiming style_engine_effect_timing(Animations::KeyframeEffect const& effect, Animations::Animation const& animation)
{
    ComputedValuesFFI::FfiEffectTiming timing {};
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
        // A scroll timeline's time is the scroll progress of the scroller it follows, which is what lets a clock tick
        // sample it where the compositor has scrolled to.
        if (auto const* scroll_timeline = as_if<Animations::ScrollTimeline>(*timeline); scroll_timeline && scroll_timeline->followed_scroller().has_value()) {
            auto const& scroller = *scroll_timeline->followed_scroller();
            timing.has_timeline_scroller = true;
            timing.timeline_scroller_is_vertical = scroller.vertical;
            timing.timeline_scroller = scroller.scroll_node.node_id.value();
        }
    }
    optional_duration(animation.start_time(), timing.has_start_time, timing.start_time);
    optional_duration(animation.hold_time(), timing.has_hold_time, timing.hold_time);
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

void StyleComputer::collect_animation_effects_into(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>> effects, ComputedStyleWorkingSet& computed_properties, StyleRecordID sampled_style_record) const
{
    auto sample = begin_animation_sample(read, abstract_element, effects, computed_properties, sampled_style_record);
    auto result = ComputedValuesFFI::rust_sample_animation_effects(&sample->input, &read);
    finish_animation_sample(*sample, result);
}

NonnullOwnPtr<StyleComputer::AnimationSample> StyleComputer::begin_animation_sample(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>> effects, ComputedStyleWorkingSet& computed_properties, StyleRecordID sampled_style_record) const
{
    auto sample = make<AnimationSample>(*this, abstract_element, computed_properties, read);
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
        double current_key = 0;
        if (!timing.decidable) {
            auto output_progress = effect->transformed_progress();
            if (!output_progress.has_value())
                continue;
            current_key = clamp(*output_progress * 100.0 * Animations::KeyframeEffect::AnimationKeyFrameKeyScaleFactor, static_cast<double>(NumericLimits<i64>::min()), static_cast<double>(NumericLimits<i64>::max()));
        }
        easing_points.unchecked_append({});
        sampled_effects.unchecked_append({
            .identity = effect->animation_preparation_identity(),
            .generation = effect->animation_preparation_generation(),
            .timing = timing,
            .easing = CSS::to_ffi_easing_descriptor<Compositing::RustFFI::FfiEasingDescriptor>(effect->timing_function(), easing_points.last()),
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
    if (base_custom_property_data && base_custom_property_data->is_animation_overlay_for(abstract_element))
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
    };

    using SampleContext = AnimationSample::Context;
    sample->input = {
        .host = m_style_engine.host(),
        .style_node = abstract_element.element().style_node_id().value(),
        .pseudo_kind = abstract_element.pseudo_element().map([](auto pseudo_element) { return static_cast<u8>(to_underlying(pseudo_element)); }).value_or(NumericLimits<u8>::max()),
        .effects = sampled_effects.data(),
        .effect_count = sampled_effects.size(),
        .longhand_table = computed_properties.computed_longhand_table(),
        .animated_overlay = computed_properties.animated_overlay(Badge<StyleComputer> {}),
        .style_record = sampled_style_record.value(),
        .custom_property_store = custom_property_data ? custom_property_data->rust_store() : nullptr,
        .base_custom_property_store = base_custom_property_data ? base_custom_property_data->rust_store() : nullptr,
        .inheritance_custom_property_store = inheritance_custom_property_data ? inheritance_custom_property_data->rust_store() : nullptr,
        .element_declares_own_custom_properties = element_declares_own_custom_properties,
        .custom_property_environments = {
            custom_property_data ? custom_property_data->identity() : 0,
            inheritance_custom_property_data ? inheritance_custom_property_data->identity() : 0,
        },
        .inheritance_parent_style_record = inheritance_parent.has_value() ? inheritance_parent->style_record_identity().value() : 0,
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
                    sample.style_computer->make_computation_context_for_property(sample.read, property_id, sample.computed_properties, sample.abstract_element).length_resolution_context,
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
    if (result.substitution_marks & ComputedValuesFFI::SUBSTITUTION_MARK_VAR)
        element.set_style_uses_var_css_function();
    if (result.substitution_marks & ComputedValuesFFI::SUBSTITUTION_MARK_ATTR)
        element.set_style_uses_attr_css_function();
    if (result.substitution_marks & ComputedValuesFFI::SUBSTITUTION_MARK_IF)
        element.set_style_uses_if_css_function();
    if (result.substitution_marks & ComputedValuesFFI::SUBSTITUTION_MARK_INHERIT)
        element.set_style_uses_inherit_css_function();
    if (result.substitution_marks & ComputedValuesFFI::SUBSTITUTION_MARK_CUSTOM_FUNCTION)
        element.set_style_uses_custom_function();
    if (result.style_query_dependencies)
        record_style_query_dependencies(abstract_element, result.style_query_dependencies);

    switch (result.outcome) {
    case ComputedValuesFFI::FfiHostAnimationSampleOutcome::Unchanged:
        return;
    case ComputedValuesFFI::FfiHostAnimationSampleOutcome::Cleared:
        computed_properties.clear_animated_properties(Badge<StyleComputer> {});
        return;
    case ComputedValuesFFI::FfiHostAnimationSampleOutcome::Evaluated:
        break;
    }
    m_keyframes_inherited_non_inherited_style_groups |= result.keyframes_inherited_non_inherited_style_groups;
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

void StyleComputer::publish_animated_custom_properties(ComputedStyleWorkingSet& computed_properties, DOM::AbstractElement abstract_element) const
{
    auto data = abstract_element.custom_property_data();
    RefPtr<CustomPropertyData const> base = data;
    if (data && data->is_animation_overlay_for(abstract_element))
        base = data->parent();

    auto const& animated_values = computed_properties.animated_custom_properties();
    if (animated_values.is_empty()) {
        if (base.ptr() != data.ptr()) {
            abstract_element.replace_custom_property_data(Badge<StyleComputer> {}, base);
            invalidate_animated_custom_property_readers(abstract_element, animated_values);
        }
        return;
    }

    if (data && data->is_animation_overlay_for(abstract_element) && data->own_values().size() == animated_values.size()) {
        bool values_unchanged = true;
        for (auto const& [name, value] : animated_values) {
            auto existing = data->own_values().find(name);
            if (existing == data->own_values().end() || !existing->value.value->equals(*value)) {
                values_unchanged = false;
                break;
            }
        }
        if (values_unchanged)
            return;
    }

    OrderedHashMap<Utf16FlyString, StyleProperty> overlay_values;
    for (auto const& [name, value] : animated_values) {
        overlay_values.set(name,
            StyleProperty {
                .important = Important::No,
                .property_id = PropertyID::Custom,
                .value = value,
            });
    }
    abstract_element.replace_custom_property_data(Badge<StyleComputer> {}, CustomPropertyData::create_animation_overlay(move(overlay_values), move(base), abstract_element));
    invalidate_animated_custom_property_readers(abstract_element, animated_values);
}

void StyleComputer::invalidate_animated_custom_property_readers(DOM::AbstractElement abstract_element, OrderedHashMap<Utf16FlyString, NonnullRefPtr<StyleValue const>> const& animated_values) const
{
    auto& element = abstract_element.element();
    // Which custom properties the element's own declarations read is not recorded: an element
    // whose custom properties animate recomputes.
    auto& style_engine = element.document().style_computer().style_engine();
    style_engine.record_derived_element_style_input_change(element.style_node_id(), StyleEngine::PublishedStyle | StyleEngine::RecomputeStyle);

    auto any_animated_custom_property_inherits = [&] {
        if (animated_values.is_empty())
            return true;
        for (auto const& [name, value] : animated_values) {
            auto registration = m_document->get_registered_custom_property(name);
            if (!registration.has_value() || registration->inherit)
                return true;
        }
        return false;
    };
    if (!abstract_element.pseudo_element().has_value() && any_animated_custom_property_inherits()) {
        style_engine.record_flat_tree_descendant_style_input_changes(
            element.style_node_id(),
            StyleEngine::InheritedStyle,
            RequiredInvalidationAfterStyleChange::all_inherited_style_groups);
    }
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

    auto existing_animations = *element_animations;
    Vector<bool> existing_animation_was_claimed;
    existing_animation_was_claimed.resize(existing_animations.size());
    Vector<GC::Ref<CSSAnimation>> new_animations;

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
        if (!existing_animation_was_claimed[i])
            existing_animations[i]->cancel(Animations::Animation::ShouldInvalidate::No);
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
    ComputedValuesFFI::rust_settled_animation_plan(m_style_engine.host(), &read, abstract_element.element().style_node_id().value(), pseudo_element_to_ffi(abstract_element.pseudo_element()), abstract_element.style_record_identity().value(), has_animations, &context, [](void* context_pointer, ComputedValuesFFI::FfiComputedAnimation const* definitions, size_t count, bool in_display_none_subtree) {
        auto& context = *static_cast<Context*>(context_pointer);
        context.style_computer->apply_animation_definitions(context.abstract_element, { definitions, count }, in_display_none_subtree);
    });
}

// A C++ computation applies the animation plan it decides beside the record it computes, and collects the animations of
// the element or pseudo-element, those the plan starts among them, into that record. For a record the engine settled,
// the host applies the plan once the record is installed and samples the animations over it, as an animation update
// samples them over the record an element holds, and publishes what they compose.
// https://drafts.csswg.org/css-transitions-2/#defining-before-change-style
// Sampling the installed record can keep the epoch's before-change style, and the record held by then is the
// after-change one, so a record that owes the transition step keeps the record it moved away from first.
void StyleComputer::compose_installed_engine_record(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, StyleRecordID before_change_style_record) const
{
    apply_settled_animation_plan(read, abstract_element);
    if (!!before_change_style_record && document().is_in_style_stabilization_epoch())
        record_transition_stabilization_baseline(abstract_element, before_change_style_record);
    auto& element = abstract_element.element();
    if (!element.has_relevant_animations() && !element.has_associated_animations())
        return;
    auto style_record = abstract_element.style_record_identity();
    if (!style_record)
        return;
    Animations::AnimationUpdateContext::ElementData element_data { style_record, reconstruct_computed_properties_for_animation(read, style_record) };
    element_data.base_is_current = true;
    Animations::AnimationUpdateContext context;
    context.elements.set(abstract_element, move(element_data));
    context.publish();
}

static void collect_dimension_attribute(Vector<StyleProperty>& properties, DOM::Element const& element, Utf16FlyString const& attribute_name, CSS::PropertyID property_id)
{
    auto attribute = element.attribute(attribute_name);
    if (!attribute.has_value())
        return;

    auto parsed_value = HTML::parse_dimension_value(*attribute);
    if (!parsed_value)
        return;

    properties.append({ .property_id = property_id, .value = parsed_value.release_nonnull() });
}

// The whole transition step for an element's record, run once the record is installed.
//
// The step needs two styles: the one the element moved away from, which the caller names, and the one it moved to,
// which is the record just installed. Both are records, and the step reads the before-change half only through
// `decide_transitions`' baseline. A transition the step starts layers its current values into the after-change style to
// keep the frame from jumping; publishing that is the same animation overlay publication an animation sample performs,
// on the same element, over the same base, and the step samples the transitions over the installed record as such a
// sample does: the box-type transformation of the values they take adjusts the composition, and the base stays current.
RequiredInvalidationAfterStyleChange StyleComputer::run_transition_step_for_installed_record(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, StyleRecordID before_change_style_record) const
{
    auto installed_style_record = abstract_element.style_record_identity();
    if (!installed_style_record || !before_change_style_record)
        return {};
    auto& element = abstract_element.element();
    auto pseudo_element = abstract_element.pseudo_element();

    record_transition_baseline_for_later_passes(abstract_element, before_change_style_record);

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
    if (m_transition_baselines_recorded) {
        if (auto baseline = StyleEngineFFI::style_engine_transition_baseline(m_style_engine.host(), element.style_node_id().value(), pseudo_element_to_ffi(pseudo_element)); baseline != 0)
            before_change_style_record = StyleRecordID { baseline };
    }
    if (has_flag(m_style_engine.style_record_dependency_flags(read, before_change_style_record), StyleRecordDependencyFlag::InDisplayNoneSubtree))
        return {};
    if (auto parent = abstract_element.element_to_inherit_style_from(); parent.has_value()) {
        if (auto parent_style = parent->computed_style(); parent_style && parent_style->in_display_none_subtree())
            return {};
    }

    begin_style_update();
    ScopeGuard end_style_update = [&] { this->end_style_update(); };
    // The after-change style is the installed record with every value its overlay holds: the current values of the
    // element's running transitions and animations, as the computation that installed it collected them.
    auto new_style = reconstruct_computed_properties_for_animation(read, installed_style_record);
    auto const* installed_overlay = static_cast<ComputedValuesFFI::AnimatedOverlay const*>(m_style_engine.style_record_view(read, installed_style_record).animated_overlay);
    if (installed_overlay)
        new_style->install_animated_overlay(Badge<StyleComputer> {}, installed_overlay);
    start_needed_transitions(read, *new_style, abstract_element, before_change_style_record);

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
    element.refresh_computed_style(pseudo_element, StyleRecordID { publication.publication.new_style_record });
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
void StyleComputer::start_needed_transitions(Layout::BegunRead const& read, ComputedStyleWorkingSet& new_style, DOM::AbstractElement abstract_element, StyleRecordID before_change_style_record) const
{
    auto had_pending_animated_style_update = m_document->needs_animated_style_update();

    auto& element = abstract_element.element();
    auto pseudo_element = abstract_element.pseudo_element();
    auto target_key = transition_target_key(abstract_element);

    // NB: We know that a DocumentTimeline's current time is always in milliseconds
    auto current_time = m_document->timeline()->current_time();
    if (!current_time.has_value())
        return;
    VERIFY(current_time->type == Animations::TimeValue::Type::Milliseconds);
    auto style_change_event_time = current_time->value;

    Vector<StyleValueFFI::FfiExistingTransition> existing_transitions;
    for (auto property_id : element.property_ids_with_existing_transitions(pseudo_element)) {
        auto transition = element.property_transition(pseudo_element, property_id);
        bool running = !transition->is_finished() && !transition->is_idle();
        existing_transitions.append({
            .property_id = to_underlying(property_id),
            .running = running,
            .end_value = transition->transition_end_value()->rust_style_value_data(),
            .reversing_adjusted_start_value = transition->reversing_adjusted_start_value()->rust_style_value_data(),
            .timing_function_output = running ? transition->timing_function_output_at_time(style_change_event_time) : 0,
            .reversing_shortening_factor = transition->reversing_shortening_factor(),
        });
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
    auto ensure_state = [&](PropertyID property_id) -> ProvisionalTransitionState& {
        auto index = m_provisional_transition_state_indices.ensure((target_key << 16) | to_underlying(property_id), [&] {
            VERIFY(document().is_in_style_stabilization_epoch());
            m_provisional_transition_state_indices_by_target.ensure(target_key).append(m_provisional_transition_states.size());
            m_provisional_transition_states.append({
                .element = element,
                .pseudo_element = pseudo_element,
                .property_id = property_id,
                .committed_transition = element.property_transition(pseudo_element, property_id),
                .proposed_transition = nullptr,
                .action = StyleValueFFI::FfiTransitionActionKind::None,
                .has_decision = false,
            });
            return m_provisional_transition_states.size() - 1;
        });
        return m_provisional_transition_states[index];
    };
    auto adopt = [](StyleValueFFI::StyleValueData const* value) {
        return StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(value));
    };

    Vector<GC::Ref<Animations::KeyframeEffect>> newly_started_transition_effects;
    HashTable<Animations::KeyframeEffect*> replaced_transition_effects;
    auto on_actions = [&](ReadonlySpan<StyleValueFFI::FfiTransitionAction> actions) {
        for (auto const& action : actions) {
            auto property_id = static_cast<PropertyID>(action.property_id);
            auto& state = ensure_state(property_id);
            decide(state, action.kind);
            if (action.kind != StyleValueFFI::FfiTransitionActionKind::None && action.kind != StyleValueFFI::FfiTransitionActionKind::Start) {
                VERIFY(state.committed_transition);
                if (auto effect = state.committed_transition->effect(); effect && effect->is_keyframe_effect())
                    replaced_transition_effects.set(static_cast<Animations::KeyframeEffect*>(effect.ptr()));
            }
            if (!action.start_value)
                continue;
            auto transition = CSSTransition::start_a_provisional_transition(abstract_element, property_id, document().transition_generation(),
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
            if (!any_of(actions, [&](auto const& action) { return action.property_id == to_underlying(state.property_id); }))
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
    StyleEngineFFI::style_engine_decide_transitions(m_style_engine.host(), &read, before_change_style_record.value(), abstract_element.style_record_identity().value(), &input, [](void* context, StyleValueFFI::FfiTransitionAction const* actions, size_t count) { (*static_cast<decltype(on_actions)*>(context))({ actions, count }); }, &on_actions);

    // A transition action is provisional until the stabilization epoch commits, but the style
    // published by this pass must already reflect that decision. Rebuild the effect stack without
    // transitions which are being removed, then layer any proposed replacements on top.
    if (!replaced_transition_effects.is_empty()) {
        new_style.clear_animated_properties(Badge<StyleComputer> {});
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
            if (!remaining_effects.is_empty())
                collect_animations_into(read, abstract_element, remaining_effects.span(), new_style, AnimationRefresh::Yes);
        }
    }

    // Immediately set the properties to the transitions' current values, to prevent single-frame jumps.
    if (!newly_started_transition_effects.is_empty()) {
        collect_animations_into(read, abstract_element, newly_started_transition_effects.span(), new_style, AnimationRefresh::Yes);
        // NB: Construction does not invalidate animated style because the effects were just evaluated. Request the
        //     first animation frame directly so timeline updates can schedule subsequent animated style updates.
        m_document->page().client().request_frame();
        if (!had_pending_animated_style_update)
            m_document->clear_needs_animated_style_update();
    }
}

bool StyleComputer::has_provisional_transition_states(DOM::AbstractElement abstract_element) const
{
    return m_provisional_transition_state_indices_by_target.contains(transition_target_key(abstract_element));
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
    RustDeclarationBlockSnapshot declaration { static_cast<Parser::ValueParserFFI::DeclarationBlockData const*>(target.declarations) };
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

static bool custom_property_inherits(DOM::Document const& document, Utf16FlyString const& name)
{
    // A custom property inherits unless it has been registered with an explicit `inherits: false`.
    auto registration = document.get_registered_custom_property(name);
    return !registration.has_value() || registration->inherit;
}

enum class IsCustomProperty : u8 {
    No,
    Yes,
};

enum class Inherits : u8 {
    No,
    Yes,
};

enum class NameIsValid : u8 {
    No,
    Yes,
};

enum class IsValid : u8 {
    No,
    Yes,
};

static JsonObject serialize_devtools_style_declaration(
    String name,
    String value,
    Important important,
    IsCustomProperty is_custom_property,
    Inherits inherits,
    NameIsValid is_name_valid,
    IsValid is_valid)
{
    JsonObject serialized_property;
    serialized_property.set("name"sv, move(name));
    serialized_property.set("value"sv, move(value));
    serialized_property.set("priority"sv, important == Important::Yes ? "important"sv : ""sv);
    serialized_property.set("isCustomProperty"sv, is_custom_property == IsCustomProperty::Yes);
    serialized_property.set("inherits"sv, inherits == Inherits::Yes);
    serialized_property.set("isNameValid"sv, is_name_valid == NameIsValid::Yes);
    serialized_property.set("isValid"sv, is_valid == IsValid::Yes);
    return serialized_property;
}

static JsonArray serialize_devtools_style_declarations(DOM::Document const& document, RustDeclarationBlock const& declaration)
{
    JsonArray declarations;

    auto serialize_property = [&](Utf16FlyString const& name, StyleProperty const& property, IsCustomProperty is_custom_property, Inherits inherits) {
        declarations.must_append(serialize_devtools_style_declaration(
            name.to_utf16_string().to_utf8_but_should_be_ported_to_utf16(),
            property.value->to_string(SerializationMode::Normal),
            property.important,
            is_custom_property,
            inherits,
            NameIsValid::Yes,
            IsValid::Yes));
    };

    for (auto const& property : declaration.properties()) {
        serialize_property(
            string_from_property_id(property.property_id),
            property,
            IsCustomProperty::No,
            is_inherited_property(property.property_id) ? Inherits::Yes : Inherits::No);
    }

    for (auto const& custom_property : declaration.custom_properties())
        serialize_property(
            custom_property.key,
            custom_property.value,
            IsCustomProperty::Yes,
            custom_property_inherits(document, custom_property.key) ? Inherits::Yes : Inherits::No);

    return declarations;
}

static JsonArray serialize_devtools_style_declarations(DOM::Document const& document, Vector<Parser::DevToolsStyleDeclaration> const& declarations)
{
    JsonArray serialized_declarations;

    for (auto const& declaration : declarations) {
        bool inherits = declaration.is_custom_property
            ? custom_property_inherits(document, declaration.name)
            : PropertyNameAndID::from_name(declaration.name)
                  .map([](auto const& property) { return !property.is_custom_property() && is_inherited_property(property.id()); })
                  .value_or(false);

        serialized_declarations.must_append(serialize_devtools_style_declaration(
            MUST(declaration.name.view().to_utf8()),
            declaration.value.to_utf8(),
            declaration.important,
            declaration.is_custom_property ? IsCustomProperty::Yes : IsCustomProperty::No,
            inherits ? Inherits::Yes : Inherits::No,
            declaration.is_name_valid ? NameIsValid::Yes : NameIsValid::No,
            declaration.is_valid ? IsValid::Yes : IsValid::No));
    }

    return serialized_declarations;
}

static Vector<Parser::DevToolsStyleDeclaration> parse_devtools_style_declarations(DOM::Document const& document, StringView declaration_block)
{
    return Parser::parse_css_declaration_block_for_devtools(Parser::ParsingParams(document), declaration_block);
}

static Vector<Parser::DevToolsStyleDeclaration> parse_devtools_style_declarations(DOM::Document const& document, Utf16View declaration_block)
{
    return Parser::parse_css_declaration_block_for_devtools(Parser::ParsingParams(document), declaration_block);
}

static Optional<size_t> source_offset_for_line_and_column(StringView source, SourcePosition const& position)
{
    size_t line = 0;
    size_t column = 0;

    Utf8View source_code_points { source };
    for (auto it = source_code_points.begin(); it != source_code_points.end();) {
        auto offset = source_code_points.byte_offset_of(it);
        if (line == position.line && column == position.column)
            return offset;

        auto code_point = *it;
        ++it;

        if (code_point == '\r') {
            if (offset + 1 < source.length() && source[offset + 1] == '\n')
                ++it;
            ++line;
            column = 0;
        } else if (code_point == '\n' || code_point == '\f') {
            ++line;
            column = 0;
        } else {
            ++column;
        }
    }

    if (line == position.line && column == position.column)
        return source.length();

    return {};
}

static Optional<String> extract_css_declaration_block_from_source(CSSRule const& rule)
{
    if (rule.type() != CSSRule::Type::Style)
        return {};

    auto const* style_sheet = rule.parent_style_sheet();
    if (!style_sheet)
        return {};

    auto const source_text = style_sheet->source_text();
    if (!source_text.has_value())
        return {};

    auto source = source_text->to_utf8();
    auto source_view = source.bytes_as_string_view();
    auto const& source_location = rule.source_location();
    if (!source_location.has_value())
        return {};

    auto maybe_offset = source_offset_for_line_and_column(source_view, *source_location);
    if (!maybe_offset.has_value())
        return {};

    Optional<u8> string_quote;
    bool in_comment = false;
    bool escaped = false;
    Optional<size_t> block_start;
    size_t block_depth = 0;

    for (size_t offset = *maybe_offset; offset < source_view.length(); ++offset) {
        auto ch = source_view[offset];
        auto next_ch = offset + 1 < source_view.length() ? source_view[offset + 1] : '\0';

        if (in_comment) {
            if (ch == '*' && next_ch == '/') {
                in_comment = false;
                ++offset;
            }
            continue;
        }

        if (string_quote.has_value()) {
            if (escaped) {
                escaped = false;
                continue;
            }
            if (ch == '\\') {
                escaped = true;
                continue;
            }
            if (ch == *string_quote)
                string_quote = {};
            continue;
        }

        if (ch == '/' && next_ch == '*') {
            in_comment = true;
            ++offset;
            continue;
        }

        if (ch == '"' || ch == '\'') {
            string_quote = ch;
            continue;
        }

        if (ch == '{') {
            if (!block_start.has_value())
                block_start = offset + 1;
            ++block_depth;
            continue;
        }

        if (ch == '}' && block_start.has_value()) {
            VERIFY(block_depth > 0);
            --block_depth;
            if (block_depth == 0)
                return MUST(String::from_utf8(source_view.substring_view(*block_start, offset - *block_start)));
        }
    }

    return {};
}

static bool has_inherited_declaration(DOM::Document const& document, ReadonlySpan<StyleProperty> properties, OrderedHashMap<Utf16FlyString, StyleProperty> const& custom_properties)
{
    if (any_of(properties, [](auto const& property) {
            return CSS::is_inherited_property(property.property_id);
        })) {
        return true;
    }

    return any_of(custom_properties, [&](auto const& custom_property) {
        return custom_property_inherits(document, custom_property.key);
    });
}

static JsonObject serialize_devtools_style_sheet_identifier(StyleSheetIdentifier const& identifier)
{
    JsonObject serialized_identifier;
    serialized_identifier.set("type"sv, style_sheet_identifier_type_to_string(identifier.type));
    if (identifier.dom_element_unique_id.has_value())
        serialized_identifier.set("domElementUniqueId"sv, identifier.dom_element_unique_id->value());
    if (identifier.url.has_value())
        serialized_identifier.set("url"sv, identifier.url->to_utf8());
    serialized_identifier.set("ruleCount"sv, identifier.rule_count);
    return serialized_identifier;
}

// What DevTools shows for one rule the engine says decides for this element. Which of the rule's
// selectors matched is asked with a selector query, because the engine reports the rule rather than
// the entry, and a panel can afford to match each selector again.
static JsonObject serialize_devtools_applied_rule(DOM::Document& document, CSSRule const& rule, DOM::AbstractElement const& element)
{
    auto const& declaration = declaration_of_rule(rule);
    auto authored_text = extract_css_declaration_block_from_source(rule);
    SelectorList const* selector_list = nullptr;
    if (auto const* style_rule = as_if<CSSStyleRule>(rule))
        selector_list = &style_rule->absolutized_selectors();
    else if (auto const* nested = as_if<CSSNestedDeclarations>(rule))
        selector_list = &nested->absolutized_selectors();
    SelectorList const empty_selectors;
    auto const& selectors = selector_list ? *selector_list : empty_selectors;

    JsonArray serialized_selectors;
    JsonArray specificities;
    JsonArray matched_selector_indexes;
    for (size_t index = 0; index < selectors.size(); ++index) {
        auto const& selector = selectors[index];
        serialized_selectors.must_append(selector->serialize().to_utf8());
        specificities.must_append(selector->specificity());
        SelectorList selector_query_list;
        selector_query_list.append(selector);
        auto selector_query = DOM::SelectorQuery::create(move(selector_query_list));
        if (selector_query->matches(element.element(), document))
            matched_selector_indexes.must_append(index);
    }

    JsonObject serialized_rule;
    serialized_rule.set("type"sv, to_underlying(rule.type()));
    serialized_rule.set("className"sv, rule.type() == CSSRule::Type::Style ? "CSSStyleRule"sv : "CSSNestedDeclarations"sv);
    serialized_rule.set("selectors"sv, move(serialized_selectors));
    serialized_rule.set("selectorsSpecificity"sv, move(specificities));
    serialized_rule.set("matchedSelectorIndexes"sv, move(matched_selector_indexes));
    serialized_rule.set("cssText"sv, rule.serialized().to_utf8());
    if (authored_text.has_value()) {
        serialized_rule.set("authoredText"sv, *authored_text);
        serialized_rule.set("declarations"sv, serialize_devtools_style_declarations(document, parse_devtools_style_declarations(document, authored_text->bytes_as_string_view())));
    } else {
        auto style = rule.type() == CSSRule::Type::Style
            ? static_cast<CSSStyleRule const&>(rule).style()
            : static_cast<CSSNestedDeclarations const&>(rule).style();
        serialized_rule.set("authoredText"sv, style->serialized().to_utf8());
        serialized_rule.set("declarations"sv, serialize_devtools_style_declarations(document, declaration));
    }
    if (auto* sheet = rule.parent_style_sheet()) {
        if (auto identifier = style_sheet_identifier_for(*sheet); identifier.has_value())
            serialized_rule.set("ruleId"sv, serialize_devtools_style_sheet_identifier(*identifier));
    }
    return serialized_rule;
}

static JsonObject serialize_devtools_inline_style(DOM::Document const& document, DOM::AbstractElement abstract_element, CSSStyleProperties const& declaration)
{
    auto authored_text = abstract_element.element().get_attribute(HTML::AttributeNames::style);

    JsonObject serialized_rule;
    serialized_rule.set("type"sv, 100);
    serialized_rule.set("className"sv, 100);
    serialized_rule.set("cssText"sv, declaration.serialized().to_utf8());
    if (authored_text.has_value()) {
        auto authored_text_utf8 = authored_text->to_utf8();
        serialized_rule.set("authoredText"sv, authored_text_utf8);
        serialized_rule.set("declarations"sv, serialize_devtools_style_declarations(document, parse_devtools_style_declarations(document, authored_text->utf16_view())));
    } else {
        serialized_rule.set("authoredText"sv, declaration.serialized().to_utf8());
        serialized_rule.set("declarations"sv, serialize_devtools_style_declarations(document, declaration.declaration_block()));
    }
    serialized_rule.set("isSystem"sv, false);
    serialized_rule.set("nodeId"sv, abstract_element.element().unique_id().value());
    return serialized_rule;
}

static void append_devtools_applied_style_entry(JsonArray& entries, JsonObject rule, Optional<UniqueNodeID> inherited_node_id = {})
{
    JsonObject entry;

    JsonValue matched_selector_indexes { JsonArray {} };
    if (auto value = rule.get("matchedSelectorIndexes"sv); value.has_value())
        matched_selector_indexes = *value;
    rule.remove("matchedSelectorIndexes"sv);
    auto is_system = rule.get_bool("isSystem"sv).value_or(false);

    entry.set("rule"sv, move(rule));
    entry.set("isSystem"sv, is_system);
    entry.set("matchedSelectorIndexes"sv, move(matched_selector_indexes));
    if (inherited_node_id.has_value())
        entry.set("inheritedNodeId"sv, inherited_node_id->value());
    else
        entry.set("inherited"sv, JsonValue {});

    entries.must_append(move(entry));
}

JsonArray StyleComputer::collect_devtools_applied_style_rules(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, bool include_inherited, bool include_user_agent_styles)
{
    JsonArray entries;

    auto append_rules_for_abstract_element = [&](DOM::AbstractElement current_element, Optional<UniqueNodeID> inherited_node_id) {
        if (auto inline_style = current_element.inline_style()) {
            if (!inherited_node_id.has_value() || has_inherited_declaration(m_document, inline_style->properties(), inline_style->custom_properties()))
                append_devtools_applied_style_entry(entries, serialize_devtools_inline_style(m_document, current_element, *inline_style), inherited_node_id);
        }

        auto node = current_element.element().style_node_id();
        if (node == 0)
            return;
        Vector<StyleEngine::RuleMatch> matches;
        if (!style_engine().match_element(read, node, matches, StyleEngine::MatchPurpose::Exact))
            return;

        // The engine reports the rules in the order the cascade applies them, and the panel lists
        // the winning one first.
        for (auto const& match : matches.in_reverse()) {
            if (match.pseudo_element != NumericLimits<u32>::max())
                continue;
            auto target = style_engine_rule_target(read, StyleEngineRuleID { match.rule });
            if (!target.has_value() || !target->source_style_sheet)
                continue;
            if (target->cascade_origin == CascadeOrigin::UserAgent && !include_user_agent_styles)
                continue;
            if (inherited_node_id.has_value()) {
                struct Context {
                    GC::Ref<DOM::Document const> document;
                    bool has_inherited_declaration { false };
                } context { m_document };
                Parser::ValueParserFFI::rust_declaration_data_visit(target->declaration.data(), &context, [](void* raw_context, Parser::ValueParserFFI::FfiDeclaredProperty const* property) {
                    auto& context = *static_cast<Context*>(raw_context);
                    if (context.has_inherited_declaration)
                        return;
                    if (property->name.length == 0)
                        context.has_inherited_declaration = is_inherited_property(static_cast<PropertyID>(property->property_id));
                    else
                        context.has_inherited_declaration = custom_property_inherits(context.document, Utf16FlyString::from_utf16({ reinterpret_cast<char16_t const*>(property->name.utf16), property->name.length }));
                });
                if (!context.has_inherited_declaration)
                    continue;
            }
            auto* rule = target->source_style_sheet->rules().rule_for_identity(target->rule_identity);
            if (!rule)
                continue;
            append_devtools_applied_style_entry(entries, serialize_devtools_applied_rule(m_document, *rule, current_element), inherited_node_id);
        }
    };

    append_rules_for_abstract_element(abstract_element, {});

    if (!include_inherited)
        return entries;

    for (auto current_element = abstract_element.element_to_inherit_style_from(); current_element.has_value(); current_element = current_element->element_to_inherit_style_from())
        append_rules_for_abstract_element(*current_element, current_element->element().unique_id());

    return entries;
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
        collect_dimension_attribute(properties, dimension_source, HTML::AttributeNames::width, CSS::PropertyID::Width);
        collect_dimension_attribute(properties, dimension_source, HTML::AttributeNames::height, CSS::PropertyID::Height);
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

RefPtr<CustomPropertyData const> StyleComputer::engine_custom_property_environment(Layout::BegunRead const& read, u64 identity, RefPtr<CustomPropertyData const> const& inherited) const
{
    if (!StyleEngine::is_engine_custom_property_environment(identity))
        return {};
    if (auto existing = m_engine_custom_property_environments.get(identity); existing.has_value())
        return *existing;
    u64 parent_identity = 0;
    auto const* store = m_style_engine.borrow_engine_custom_property_environment(read, identity, parent_identity);
    if (!store)
        return {};
    if (parent_identity != (inherited ? inherited->identity() : 0)) {
        ComputedValuesFFI::rust_custom_property_store_destroy(store);
        return {};
    }
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
    auto data = CustomPropertyData::create(move(own_values), inherited, store, identity);
    m_engine_custom_property_environments.set(identity, data);
    // What its children inherit is the engine's to name too: the children's environments are resolved over it.
    if (auto inheritable = m_style_engine.inheritable_custom_property_environment(read, identity); inheritable != identity)
        data->set_inheritable(document(), inheritable == (inherited ? inherited->identity() : 0) ? inherited : engine_custom_property_environment(read, inheritable, inherited));
    return data;
}

// An environment nothing but the table holds is one no element is in, and the table is the only
// thing keeping it - and its parent chain - alive.
void StyleComputer::sweep_custom_property_environments() const
{
    m_engine_custom_property_environments.remove_all_matching([](auto&, NonnullRefPtr<CustomPropertyData const> const& data) { return data->ref_count() == 1; });
}

void StyleComputer::update_root_element_font_metrics(ComputedValues const& values)
{
    m_root_element_font_metrics = Length::FontMetrics { values.font_size(), values.font_list().first_available_font().pixel_metrics(), values.line_height() };
    m_root_element_font_metrics_depend_on_viewport_metrics = values.font_metrics_depend_on_viewport_metrics();
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

ComputationContext StyleComputer::make_computation_context_for_property(Layout::BegunRead const& read, PropertyID property_id, ComputedStyleWorkingSet const& style, Optional<DOM::AbstractElement> abstract_element) const
{
    auto subject_inline_axis_is_horizontal = [&]() {
        auto writing_mode = [&](DOM::AbstractElement const& candidate) -> Optional<WritingMode> {
            auto record = m_style_engine.style_record_view(read, candidate.style_record_identity());
            if (!record.present)
                return {};
            auto const* inherited_box = static_cast<ComputedValuesFFI::InheritedBoxValues const*>(record.payloads[to_underlying(StyleGroupIndex::InheritedBoxValues)]);
            return static_cast<WritingMode>(inherited_box->writing_mode);
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
            inheritance_parent.has_value() && inheritance_parent->has_style() ? inheritance_parent->computed_style()->line_height() : InitialValues::line_height()
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
    ComputedValuesFFI::rust_finalize_animated_box_type(box_type, style.mutable_computed_longhand_table(), animated_overlay, &line_height_metrics);
    style.finish_animated_overlay_rust_mutation(Badge<StyleComputer> {});
}

NonnullRefPtr<ComputedValues const> StyleComputer::create_document_style() const
{
    ensure_style_metadata_tables_installed();

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
    auto computed_properties = CSS::ComputedStyleWorkingSet::create_with_longhand_table(ComputedValuesFFI::rust_create_document_longhand_table(&input));
    CSS::ColorResolutionContext color_resolution_context {
        .color_scheme = document().page().preferred_color_scheme(),
        .current_color = CSS::InitialValues::color(),
        .current_color_style_value = &computed_properties->property(PropertyID::Color),
        .calculation_resolution_context = { .length_resolution_context = CSS::Length::ResolutionContext::for_document(document()) },
    };
    auto computed_values = CSS::ComputedValues::create(*computed_properties, document(), document().style_scope(), move(color_resolution_context));
    return computed_values;
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
    Vector<OverlayFont, 1> fonts;
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
            .longhand_table = style.computed_longhand_table(),
            .animated_overlay = style.animated_overlay(),
            .animation_overlay_identity = publishes_overlay ? animated_properties->identity() : 0,
            .used_color_scheme = static_cast<u8>(to_underlying(style.color_scheme(used_color_scheme_preference, document().supported_color_schemes()))),
            .display_before_box_type_transformation_raw = bit_cast<u32>(style.display_before_box_type_transformation()),
            .is_document_element = !abstract_element.pseudo_element().has_value() && element.is_document_element(),
            .inherited_group_count = ComputedValues::inherited_style_group_count,
            .custom_property_environment = custom_property_data ? custom_property_data->identity() : 0,
            .custom_property_store = custom_property_data ? custom_property_data->rust_store() : nullptr,
            .callback_context = &fonts.last(),
            .font_group_inputs = [](void* context, void* inputs) {
                auto const& font = *static_cast<OverlayFont const*>(context);
                *static_cast<ComputedValuesFFI::FfiFontGroupBuildInputs*>(inputs) = font.style.font_group_build_inputs(*font.document, font.tree_scope);
            },
        });
    }
    StyleEngineFFI::style_engine_publish_sampled_animation_overlays(m_style_engine.host(), &read, inputs.data(), inputs.size(), publications.data());
    auto& counters = document().style_invalidation_counters();
    for (auto const& published : publications) {
        VERIFY(published.view.present);
        if (published.rebuilt_every_group)
            counters.animated_style_full_builds++;
        else
            counters.animated_style_overlay_builds++;
    }
}

StyleRecordID StyleComputer::intern_computed_style_inputs(Layout::BegunRead const& read, DOM::AbstractElement abstract_element, ComputedValues const& values) const
{
    return record_computed_style_inputs(read, Optional<DOM::AbstractElement> { abstract_element }, values, 0).new_style_record;
}

StyleRecordID StyleComputer::intern_anonymous_layout_style(Layout::BegunRead const& read, ComputedValues const& values) const
{
    return record_computed_style_inputs(read, {}, values, 0).new_style_record;
}

StyleEngine::StyleRecordDelta StyleComputer::record_computed_style_inputs(Layout::BegunRead const& read, Optional<DOM::AbstractElement> abstract_element, ComputedValues const& values, StyleNodeID style_node_id) const
{
    auto const& base = values.base_values();
    // An unassigned record cannot own an animation overlay, so a layout-derived copy of an
    // animated style interns its final merged payloads directly. Splitting off the base there
    // would intern (and paint) the un-animated values.
    bool const unassigned_with_animations = style_node_id == 0 && (values.has_animated_values() || values.animated_properties());
    auto const& payload_source = unassigned_with_animations ? values : base;
    Array<void const*, to_underlying(StyleGroupIndex::Count)> payloads;
    for (size_t index = 0; index < payloads.size(); ++index)
        payloads[index] = payload_source.style_group_payload(static_cast<StyleGroupIndex>(index));
    auto custom_property_environment = abstract_element.has_value() ? abstract_element->custom_property_data() : nullptr;
    u64 counter_style_environment_identity = 0;
    if (abstract_element.has_value()
        && base.reads_counter_style_environment(abstract_element->pseudo_element().has_value()))
        counter_style_environment_identity = abstract_element->style_scope().counter_style_environment_identity(read);
    auto animated_properties = style_node_id != 0 ? values.animated_properties() : nullptr;
    u64 animation_overlay_identity = animated_properties ? animated_properties->identity() : 0;
    Array<void const*, to_underlying(StyleGroupIndex::Count)> animation_overlay_payloads;
    if (animated_properties) {
        for (size_t index = 0; index < animation_overlay_payloads.size(); ++index)
            animation_overlay_payloads[index] = values.style_group_payload(static_cast<StyleGroupIndex>(index));
    }
    auto pseudo_kind = pseudo_element_to_ffi(abstract_element.has_value() ? abstract_element->pseudo_element() : Optional<CSS::PseudoElement> {});
    auto publication = const_cast<StyleComputer&>(*this).style_engine().publish_computed_groups(read, style_node_id, pseudo_kind, payloads, ComputedValues::inherited_style_group_count, custom_property_environment ? custom_property_environment->identity() : 0, false, counter_style_environment_identity, animation_overlay_identity, animated_properties ? animated_properties->overlay() : nullptr, animated_properties ? animation_overlay_payloads.span() : ReadonlySpan<void const*> {}, base.computed_longhand_table(), custom_property_environment ? custom_property_environment->rust_store() : nullptr);
    return publication;
}

RefPtr<ComputedValues const> StyleComputer::engine_transient_pseudo_element_style(Layout::BegunRead const& read, DOM::Element const& element, StyleEngine::DemandedPseudoElement pseudo_element)
{
    auto answer = m_style_engine.answer_pseudo_element_record_demand(read, element.style_node_id(), StyleEngine::PseudoElementRecordDemand::ReadOnly, pseudo_element);
    auto view = computed_style_record_view(read, StyleRecordID { answer.record.style_record });
    if (!view)
        return {};
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
    style->freeze_computed_longhand_table();
    apply_animated_properties_to_reconstruction(*style, computed_values);
    return style;
}

void StyleComputer::apply_animated_properties_to_reconstruction(ComputedStyleWorkingSet& style, ComputedValues const& computed_values) const
{
    auto const* animated_properties = computed_values.animated_properties();
    if (!animated_properties)
        return;
    for (auto const& entry : animated_properties->entries()) {
        auto property_id = static_cast<PropertyID>(entry.property);
        style.set_animated_property(
            Badge<StyleComputer> {}, property_id, animated_properties->property(property_id),
            // NB: An adjustment wins over an important declaration as a transition's value does, and the working
            //     set's flag says only that.
            entry.result_of_transition || entry.post_compute_adjustment ? AnimatedPropertyResultOfTransition::Yes : AnimatedPropertyResultOfTransition::No,
            entry.inherited ? ComputedStyleWorkingSet::Inherited::Yes : ComputedStyleWorkingSet::Inherited::No);
    }
}

NonnullRefPtr<ComputedStyleWorkingSet> StyleComputer::reconstruct_computed_properties_for_animation(Layout::BegunRead const& read, StyleRecordID style_record) const
{
    auto record = m_style_engine.style_record_view(read, style_record);
    VERIFY(record.present);
    auto style = ComputedStyleWorkingSet::create_for_animation_update(
        static_cast<ComputedValuesFFI::ComputedLonghandTable const*>(record.longhand_table),
        static_cast<ComputedValuesFFI::AnimatedOverlay const*>(record.animated_overlay));
    if (record.animated_overlay && ComputedValuesFFI::rust_animated_overlay_contains(static_cast<ComputedValuesFFI::AnimatedOverlay const*>(record.animated_overlay), to_underlying(PropertyID::Display))) {
        auto const* box = static_cast<ComputedValuesFFI::BoxValues const*>(record.base_payloads[to_underlying(StyleGroupIndex::BoxValues)]);
        style->set_display_before_box_type_transformation(display_from_ffi_display(box->display));
    }
    return style;
}

u64 StyleComputer::style_environment_version_for_sharing() const
{
    return document().style_environment_version() ^ (m_viewport_environment_version << 32);
}

void StyleComputer::ensure_style_metadata_tables_installed()
{
    static bool const installed = [] {
        // Transfer one shared Rust reference for every longhand initial value, so
        // initial-value selection never crosses the FFI.
        Vector<void const*> initial_value_entries;
        initial_value_entries.ensure_capacity(number_of_longhand_properties);
        for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
            auto initial_value = property_initial_value(static_cast<PropertyID>(i));
            initial_value_entries.unchecked_append(StyleValueFFI::rust_style_value_retain(initial_value->rust_style_value_data()));
        }
        ComputedValuesFFI::rust_style_metadata_set_initial_value_table(initial_value_entries.data(), initial_value_entries.size());

        return true;
    }();
    (void)installed;
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
