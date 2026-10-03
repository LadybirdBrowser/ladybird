/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonArray.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/WeakPtr.h>
#include <LibGC/Weak.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/CSS/CSSAnimationProperties.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CascadeOrigin.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/InstalledStyle.h>
#include <LibWeb/CSS/MediaQuery.h>
#include <LibWeb/CSS/RustDeclarationBlock.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/SharedCompiledStyleSheet.h>
#include <LibWeb/CSS/StyleGroupPayloadPins.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

#include <LibWeb/CSS/StyleEngineBridge.h>

namespace Web::CSS {

// Matching an originating element answers both its own cascade and every
// pseudo-element cascade. A caller that computes those cascades as one batch
// can keep this result between them.
struct StyleEngineMatchResult {
    StyleNodeID node;
    Optional<Vector<StyleEngine::RuleMatch>> matches;
    Optional<u32> signature;
};

class WEB_API StyleComputer final : public GC::Cell {
    GC_CELL(StyleComputer, GC::Cell);
    GC_DECLARE_ALLOCATOR(StyleComputer);

public:
    static void for_each_property_expanding_shorthands(PropertyID, StyleValue const&, Function<void(PropertyID, StyleValue const&)> const& set_longhand_property);

    static Optional<Utf16String> user_agent_style_sheet_source(Utf16View name);

    static Vector<StyleProperty> collect_presentational_hint_properties(DOM::AbstractElement);

    explicit StyleComputer(DOM::Document&);
    virtual ~StyleComputer() override = default;

    DOM::Document& document() { return m_document; }
    DOM::Document const& document() const { return m_document; }

    [[nodiscard]] NonnullRefPtr<ComputedValues const> create_document_style() const;

    // The document element's style installed: the metrics `rem` resolves against are its font's.
    void update_root_element_font_metrics(ComputedValues const&);
    // The style of a pseudo-element the element holds no style for, as the style engine derives it for one read alone;
    // null where the engine leaves the read to C++.
    [[nodiscard]] RefPtr<ComputedValues const> engine_transient_pseudo_element_style(Layout::BegunRead const& read, DOM::Element const&, StyleEngine::DemandedPseudoElement);
    [[nodiscard]] JsonArray collect_devtools_applied_style_rules(Layout::BegunRead const& read, DOM::AbstractElement, bool include_inherited, bool include_user_agent_styles);

    // The way back from a StyleEngine rule identity to what that rule contributes to the cascade.
    // Matching in the engine answers with identities; the cascade needs a declaration and the place
    // it sits in. Rust owns the rule data and per-document identities; this side resolves only
    // the source's document/loading state.
    // DevTools resolves native identities to CSSOM wrappers only when requested.
    void register_style_engine_sheet_source(StyleSheetState const&);
    [[nodiscard]] Optional<StyleEngineRuleTarget> style_engine_rule_target(Layout::BegunRead const& read, StyleEngineRuleID rule_id) const;

    static CSSPixels default_user_font_size();
    static void ensure_style_metadata_tables_installed();
    static CSSPixels absolute_size_mapping(AbsoluteSize, CSSPixels default_font_size);

    void set_viewport_rect(Badge<DOM::Document>, CSSPixelRect const& viewport_rect) { m_viewport_rect = viewport_rect; }
    [[nodiscard]] CSSPixelRect const& viewport_rect_for_style_environment() const { return m_viewport_rect; }
    [[nodiscard]] Length::FontMetrics const& root_element_font_metrics() const { return m_root_element_font_metrics; }
    [[nodiscard]] bool root_element_font_metrics_depend_on_viewport_metrics() const { return m_root_element_font_metrics_depend_on_viewport_metrics; }
    // Moves with the viewport rect the environment resolves viewport units against.
    void bump_viewport_environment_version() { ++m_viewport_environment_version; }
    // The environment as a compositor animation request names it: the document's version and the viewport's.
    [[nodiscard]] u64 style_environment_version_for_sharing() const;

    // Drop caches whose keys contain inputs that are stable only within one engine transaction.
    // Style sharing has a self-validating key and survives ordinary transaction boundaries.
    void prepare_for_style_engine_transaction() const;

    void begin_style_update() const;
    void end_style_update() const;

    struct ComputedStyleInvalidation {
        RequiredInvalidationAfterStyleChange invalidation;
        bool any_computed_value_changed { false };
    };

    // Has the engine compose a sampled overlay over the record it was sampled on, compares it with that record, and
    // publishes it. `before_publication` sees the comparison first.
    struct SampledAnimationOverlayPublication {
        StyleEngineFFI::FfiAnimationInvalidation invalidation;
        StyleEngine::StyleRecordDelta publication;
    };
    [[nodiscard]] SampledAnimationOverlayPublication publish_sampled_animation_overlay(Layout::BegunRead const& read, DOM::AbstractElement, ComputedStyleWorkingSet&, StyleRecordID style_record, Function<void(StyleEngineFFI::FfiAnimationInvalidation const&)> const& before_publication = {}) const;
    // Give a layout-only variant of an element or pseudo-element style an authoritative record
    // without replacing the StyleEngine assignment of its DOM target.
    [[nodiscard]] StyleRecordID intern_computed_style_inputs(Layout::BegunRead const& read, DOM::AbstractElement, ComputedValues const&) const;
    // Anonymous layout boxes have no style target, but their immutable group tuple is still an
    // authoritative shared record rather than layout-owned complete computed values.
    [[nodiscard]] StyleRecordID intern_anonymous_layout_style(Layout::BegunRead const& read, ComputedValues const&) const;

    [[nodiscard]] ComputedStyleRecordView computed_style_record_view(Layout::BegunRead const& read, StyleRecordID) const;
    [[nodiscard]] ComputedStyleRecordView computed_style_record_view(InstalledStyle const&) const;
    // The style an element or a pseudo-element installs as `style_record`, which a style update answered.
    [[nodiscard]] InstalledStyle install_style(Layout::BegunRead const& read, StyleRecordID style_record) const;
    void pin_style_record(StyleRecordID) const;
    void unpin_style_record(StyleRecordID) const;
    void begin_style_record_view_epoch() const;
    void end_style_record_view_epoch() const;
    [[nodiscard]] u64 computed_style_record_view_pin_count() const { return m_computed_style_record_view_pin_count; }

    void sweep_custom_property_environments() const;
    // The environment the style engine resolved under `identity`, materialized over the data the
    // element inherits, which must be the environment the engine resolved it over. Nothing when
    // the identity is no engine environment or was resolved over another.
    [[nodiscard]] RefPtr<CustomPropertyData const> engine_custom_property_environment(Layout::BegunRead const& read, u64 identity, RefPtr<CustomPropertyData const> const& inherited) const;

    // Whether the collection refreshes a previously published style outside the drive; a refresh
    // re-runs the animated element style adjustments and leaves the non-inherited-property
    // inheritance invalidation mark itself.
    enum class AnimationRefresh {
        No,
        Yes,
    };
    void collect_animations_into(Layout::BegunRead const& read, DOM::AbstractElement, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>>, ComputedStyleWorkingSet&, AnimationRefresh) const;

    void apply_animation_definitions(DOM::AbstractElement&, ReadonlySpan<ComputedValuesFFI::FfiComputedAnimation> animation_definitions, bool in_display_none_subtree) const;
    // Applies the animation plan the record an element or pseudo-element holds decides, for a record the style engine
    // settled, where applying it would change anything.
    void apply_settled_animation_plan(Layout::BegunRead const& read, DOM::AbstractElement&) const;
    // Starts the CSS animations of an element or pseudo-element and composes its animations over the record the style
    // engine settled for it, once the host has installed it.
    void compose_installed_engine_record(Layout::BegunRead const& read, DOM::AbstractElement, StyleRecordID before_change_style_record) const;

    static NonnullRefPtr<StyleValue const> compute_font_size(NonnullRefPtr<StyleValue const> const& absolutized_value, int computed_math_depth, Optional<DOM::AbstractElement> const& inheritance_parent, CSSPixels initial_font_size = InitialValues::font_size());
    static NonnullRefPtr<StyleValue const> compute_font_style(NonnullRefPtr<StyleValue const> const& absolutized_value);
    static NonnullRefPtr<StyleValue const> compute_font_weight(NonnullRefPtr<StyleValue const> const& absolutized_value, Optional<DOM::AbstractElement> const& inheritance_parent);
    static NonnullRefPtr<StyleValue const> compute_font_width(NonnullRefPtr<StyleValue const> const& absolutized_value);

    [[nodiscard]] NonnullRefPtr<ComputedStyleWorkingSet> reconstruct_computed_properties(ComputedValues const&) const;
    void apply_animated_properties_to_reconstruction(ComputedStyleWorkingSet&, ComputedValues const&) const;
    [[nodiscard]] NonnullRefPtr<ComputedStyleWorkingSet> reconstruct_computed_properties_for_animation(Layout::BegunRead const& read, StyleRecordID) const;

    void begin_transition_stabilization_epoch();
    // https://drafts.csswg.org/css-transitions-2/#defining-before-change-style
    // Keeps `before_change_style_record` as the style a later pass of the stabilization epoch decides the element's
    // transitions against, unless a pass already kept one.
    void record_transition_stabilization_baseline(DOM::AbstractElement, StyleRecordID before_change_style_record) const;
    // Keeps `before_change_style_record` that way where the element's style scope can run a later pass at all.
    void record_transition_baseline_for_later_passes(DOM::AbstractElement, StyleRecordID before_change_style_record) const;
    // Runs the whole transition step for an installed record, against the record the element moved away from, which
    // the caller keeps alive. Returns what publishing a started transition's values invalidates, which the caller
    // reacts to like to the rest of the style change.
    [[nodiscard]] RequiredInvalidationAfterStyleChange run_transition_step_for_installed_record(Layout::BegunRead const& read, DOM::AbstractElement, StyleRecordID before_change_style_record) const;
    void commit_transition_stabilization_epoch();
    void for_each_provisional_transition_effect(DOM::AbstractElement const&, Function<void(Animations::KeyframeEffect&)> const&) const;

    // The media features of the current style update, which the style engine copies with each transaction.
    [[nodiscard]] Parser::ValueParserFFI::FfiMediaEnvironment const* ensure_media_environment_for_style_update() const;

private:
    virtual void visit_edges(Visitor&) override;

    [[nodiscard]] StyleEngine::StyleRecordDelta record_computed_style_inputs(Layout::BegunRead const& read, Optional<DOM::AbstractElement>, ComputedValues const&, StyleNodeID style_node_id) const;

private:
    // `sampled_style_record` names the record the working set was reconstructed from, where it was.
    void collect_animation_effects_into(Layout::BegunRead const& read, DOM::AbstractElement, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>>, ComputedStyleWorkingSet&, StyleRecordID sampled_style_record) const;
    void publish_animated_custom_properties(ComputedStyleWorkingSet&, DOM::AbstractElement) const;
    void invalidate_animated_custom_property_readers(DOM::AbstractElement, OrderedHashMap<Utf16FlyString, NonnullRefPtr<StyleValue const>> const& animated_values) const;
    void start_needed_transitions(Layout::BegunRead const& read, ComputedStyleWorkingSet&, DOM::AbstractElement, StyleRecordID before_change_style_record) const;
    [[nodiscard]] bool has_provisional_transition_states(DOM::AbstractElement) const;
    void finalize_style(Layout::BegunRead const& read, ComputedStyleWorkingSet&, DOM::AbstractElement, ComputedValuesFFI::FfiStyleFinalizationMode) const;

    [[nodiscard]] CSSPixelRect viewport_rect() const { return m_viewport_rect; }

public:
    // The document's StyleEngine.
    [[nodiscard]] StyleEngine& style_engine() { return m_style_engine; }
    [[nodiscard]] StyleEngine const& style_engine() const { return m_style_engine; }

    // The user-agent and user sheets attached to this document's StyleEngine. User-agent sheets are
    // shared between documents, so a per-document identity cannot live on the sheet itself.
    struct NonAuthorStyleSheet {
        RefPtr<StyleSheetState> sheet;
        SheetID sheet_id;
    };
    [[nodiscard]] Vector<NonAuthorStyleSheet>& non_author_style_sheets() { return m_non_author_style_sheets; }

    [[nodiscard]] StyleEngineRuleID style_engine_rule_id_for(Layout::BegunRead const& read, RustRule const&) const;
    [[nodiscard]] SheetID style_engine_sheet_id_for(StyleSheetState const&) const;
    void set_style_engine_sheet_id_for(StyleSheetState&, SheetID);

    [[nodiscard]] HashMap<SharedCompiledStyleSheetKey, RefPtr<SharedCompiledStyleSheet>>& shared_compiled_style_sheets() { return m_shared_compiled_style_sheets; }

    // The reverse of a node's style node identity. StyleEngine plans in identities; turning a
    // plan back into nodes needs this, and it is maintained at exactly the two points the
    // identity itself is.
    void register_style_node(StyleNodeID style_node_id, DOM::Node&);
    void ensure_style_node_slot(StyleNodeID);
    void unregister_style_node(StyleNodeID style_node_id);
    [[nodiscard]] GC::Ptr<DOM::Element> element_for_style_node(StyleNodeID style_node_id) const;
    [[nodiscard]] GC::Ptr<DOM::Node> node_for_style_node(StyleNodeID style_node_id) const;
    void prepare_elements_for_style_computation();
    void for_each_style_node(Function<void(DOM::Element&)>) const;

    // Style scopes are numbered per document, with zero naming the document's own scope. A scope is
    // never reused, so a sheet detached with an identity that has been retired detaches nothing
    // rather than something else.
    [[nodiscard]] TreeScopeID allocate_tree_scope(DOM::ShadowRoot&);
    // The shadow root a scope numbers, while it lives and still belongs to this document.
    [[nodiscard]] DOM::ShadowRoot* shadow_root_for_tree_scope(TreeScopeID) const;
    // Each shadow root a scope numbers, while it lives and still belongs to this document.
    template<typename Callback>
    void for_each_shadow_root(Callback&& callback) const
    {
        for (u32 scope = 1; scope <= m_shadow_roots_by_tree_scope.size(); ++scope) {
            if (auto* shadow_root = shadow_root_for_tree_scope(TreeScopeID { scope }))
                callback(*shadow_root);
        }
    }

private:
    GC::Ref<DOM::Document> m_document;

    Length::FontMetrics m_default_font_metrics;
    mutable Length::FontMetrics m_root_element_font_metrics;
    mutable bool m_root_element_font_metrics_depend_on_viewport_metrics { false };

    mutable Optional<ComputationContext> m_cached_font_computation_context;
    mutable Optional<ComputationContext> m_cached_line_height_computation_context;
    mutable Optional<ComputationContext> m_cached_generic_computation_context;
    mutable u64 m_style_update_depth { 0 };
    mutable Optional<MediaEnvironmentSnapshot> m_style_update_media_environment;
    mutable Optional<Parser::ValueParserFFI::FfiMediaEnvironment> m_style_update_ffi_media_environment;
    u64 m_viewport_environment_version { 0 };
    // The environments the style engine resolved, by the identity it minted, materialized once.
    mutable HashMap<u64, NonnullRefPtr<CustomPropertyData const>> m_engine_custom_property_environments;

    // What one final value parses to against one registration's syntax: a pure function of the
    // value, the syntax, and the registration generation, unlike the computed-value step after it,
    // which resolves font-relative units against the reading element and runs per read.
    struct RegisteredCustomPropertyParse {
        NonnullRefPtr<StyleValue const> value;
        void const* syntax_identity { nullptr };
        u64 registration_generation { 0 };
        NonnullRefPtr<StyleValue const> parsed;
    };
    mutable HashMap<void const*, Vector<RegisteredCustomPropertyParse>> m_registered_custom_property_parses;

    enum class ProvisionalTransitionAction : u8 {
        None,
        Remove,
        Cancel,
        Start,
        RemoveAndStart,
        CancelRemoveAndStart,
    };
    struct ProvisionalTransitionState {
        GC::Ptr<DOM::Element> element;
        Optional<PseudoElement> pseudo_element;
        PropertyID property_id;
        GC::Ptr<CSSTransition> committed_transition;
        GC::Ptr<CSSTransition> proposed_transition;
        ProvisionalTransitionAction action { ProvisionalTransitionAction::None };
        bool has_decision { false };
    };
    // Whether the animation collection of the computation in progress resolved a keyframe-borne
    // `inherit` for a non-inherited property; folded into the explicit-inheritance bookkeeping.
    mutable u32 m_keyframes_inherited_non_inherited_style_groups { 0 };
    mutable Vector<ProvisionalTransitionState> m_provisional_transition_states;
    mutable HashMap<u64, size_t> m_provisional_transition_state_indices;
    mutable HashMap<u64, Vector<size_t>> m_provisional_transition_state_indices_by_target;
    // Whether the engine keeps a before-change style the current stabilization epoch recorded.
    mutable bool m_transition_baselines_recorded { false };

    ComputationContext make_computation_context_for_property(Layout::BegunRead const& read, PropertyID, ComputedStyleWorkingSet const&, Optional<DOM::AbstractElement>) const;
    ComputationContext const& get_computation_context_for_property(Layout::BegunRead const& read, PropertyID, ComputedStyleWorkingSet const&, Optional<DOM::AbstractElement>) const;
    void clear_computation_context_caches() const
    {
        const_cast<StyleComputer*>(this)->m_cached_font_computation_context = {};
        const_cast<StyleComputer*>(this)->m_cached_line_height_computation_context = {};
        const_cast<StyleComputer*>(this)->m_cached_generic_computation_context = {};
    }

    bool computation_context_cache_is_empty() const
    {
        return !m_cached_font_computation_context.has_value() && !m_cached_line_height_computation_context.has_value() && !m_cached_generic_computation_context.has_value();
    }

    CSSPixelRect m_viewport_rect;

    mutable StyleEngine m_style_engine;
    mutable u64 m_computed_style_record_view_pin_count { 0 };
    mutable u32 m_style_record_view_epoch_depth { 0 };
    // Indexed by each kind's dense index; see style_node_is_text(). The element-kind table also
    // holds shadow roots: a root gets no style, but it has a StyleNodeID of its own.
    Vector<GC::Ptr<DOM::Node>> m_element_style_nodes;
    Vector<GC::Ptr<DOM::Text>> m_text_style_nodes;
    // The root each scope numbers, by scope minus one.
    Vector<GC::Weak<DOM::ShadowRoot>> m_shadow_roots_by_tree_scope;
    Vector<NonAuthorStyleSheet> m_non_author_style_sheets;
    HashMap<RefPtr<StyleSheetState const>, SheetID> m_constructed_sheet_ids;
    HashMap<SharedCompiledStyleSheetKey, RefPtr<SharedCompiledStyleSheet>> m_shared_compiled_style_sheets;
    HashMap<u64, WeakPtr<StyleSheetState const>> m_style_engine_sheet_sources;
};

// Keeps a style record alive for as long as it is held, as a transition step that reads the record an element moved
// away from needs. A null record pins nothing.
class StyleRecordPin {
    AK_MAKE_NONCOPYABLE(StyleRecordPin);
    AK_MAKE_NONMOVABLE(StyleRecordPin);

public:
    StyleRecordPin(StyleComputer const& style_computer, StyleRecordID style_record)
        : m_style_computer(style_computer)
        , m_style_record(style_record)
    {
        if (!!m_style_record)
            m_style_computer->pin_style_record(m_style_record);
    }

    ~StyleRecordPin()
    {
        if (!!m_style_record)
            m_style_computer->unpin_style_record(m_style_record);
    }

    StyleRecordID style_record() const { return m_style_record; }

private:
    GC::Ref<StyleComputer const> m_style_computer;
    StyleRecordID m_style_record;
};

}
