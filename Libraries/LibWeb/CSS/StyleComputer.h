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
#include <LibWeb/CSS/RustDeclarationBlock.h>
#include <LibWeb/CSS/RustMediaList.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/SharedCompiledStyleSheet.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

#include <LibWeb/CSS/StyleEngineBridge.h>

namespace Web::CSS {

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

    // Interns the document's own style, the viewport's, which no element holds.
    [[nodiscard]] StyleRecordID intern_document_style(Layout::BegunRead const&) const;

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
    static CSSPixels absolute_size_mapping(AbsoluteSize, CSSPixels default_font_size);

    void set_viewport_rect(Badge<DOM::Document>, CSSPixelRect const& viewport_rect) { m_viewport_rect = viewport_rect; }
    [[nodiscard]] CSSPixelRect const& viewport_rect_for_style_environment() const { return m_viewport_rect; }
    [[nodiscard]] Length::FontMetrics const& root_element_font_metrics() const { return m_root_element_font_metrics; }
    [[nodiscard]] bool root_element_font_metrics_depend_on_viewport_metrics() const { return m_root_element_font_metrics_depend_on_viewport_metrics; }
    // https://drafts.csswg.org/css-viewport/#effective-zoom
    // NB: The root element font metrics are zoomed by this effective zoom of the root element.
    [[nodiscard]] double root_element_effective_zoom() const { return m_root_element_effective_zoom; }
    // Moves with the viewport rect the environment resolves viewport units against.
    void bump_viewport_environment_version() { ++m_viewport_environment_version; }
    // The environment as a compositor animation request names it: the document's version and the viewport's.
    [[nodiscard]] u64 style_environment_version_for_sharing() const;

    // Drop caches whose keys contain inputs that are stable only within one engine transaction.
    // Style sharing has a self-validating key and survives ordinary transaction boundaries.
    void prepare_for_style_engine_transaction() const;

    void begin_style_update() const;
    void end_style_update() const;
    void begin_inheritance_chain_read() const { ++m_inheritance_chain_read_depth; }
    void end_inheritance_chain_read() const { --m_inheritance_chain_read_depth; }

    struct ComputedStyleInvalidation {
        RequiredInvalidationAfterStyleChange invalidation;
        bool any_computed_value_changed { false };
    };

    // Has the engine compose each element's sampled overlay over the record the element installed, which it was sampled
    // on, compare it with that record and publish it, in one call of the engine, answering each at the same index of
    // `publications`.
    struct SampledAnimationOverlay {
        DOM::AbstractElement element;
        ComputedStyleWorkingSet const& style;
    };
    void publish_sampled_animation_overlays(Layout::BegunRead const& read, ReadonlySpan<SampledAnimationOverlay>, Span<StyleEngineFFI::FfiAnimationOverlayPublication> publications) const;
    // Give a layout-only variant of an element or pseudo-element style an authoritative record
    // without replacing the StyleEngine assignment of its DOM target.
    [[nodiscard]] StyleRecordID intern_computed_style_inputs(Layout::BegunRead const& read, DOM::AbstractElement, ComputedValues const&) const;
    // Anonymous layout boxes have no style target, but their immutable group tuple is still an
    // authoritative shared record rather than layout-owned complete computed values.
    [[nodiscard]] StyleRecordID intern_anonymous_layout_style(Layout::BegunRead const& read, ComputedValues const&) const;

    [[nodiscard]] ComputedStyleRecordView computed_style_record_view(InstalledStyle const&) const;
    // The style an element or a pseudo-element installs as `style_record`, which a style update answered.
    [[nodiscard]] InstalledStyle install_style(Layout::BegunRead const& read, StyleRecordID style_record) const;
    // A style installation swaps in the styles the render side noted the elements hold as it published them, so it
    // tells the render side only of the elements whose styles the installation cleared before swapping theirs in.
    void begin_style_installation();
    void end_style_installation();
    // The generation an element and its ancestors take as a style below them moves: one per installation, so that a
    // walk up from an element stops at the first ancestor another element's walk reached, and a fresh one otherwise.
    [[nodiscard]] u64 subtree_style_generation_for_move();
    // Whether the render side is to be told of the custom-property environment `identity`, which it was not yet in this
    // installation. Outside of one, it always is.
    [[nodiscard]] bool custom_property_environment_needs_note(CustomPropertyData const&) const;
    void begin_style_record_view_epoch() const;
    void end_style_record_view_epoch() const;

    void sweep_custom_property_environments() const;
    // The environment the style engine resolved under `identity`, materialized over the data the
    // element inherits, which must be the environment the engine resolved it over. Nothing when
    // the identity is no engine environment or was resolved over another.
    [[nodiscard]] RefPtr<CustomPropertyData const> engine_custom_property_environment(Layout::BegunRead const& read, u64 identity, RefPtr<CustomPropertyData const> const& inherited) const;
    // The environment `environment` was built over: the one the engine resolved it over, or the node its chain lies over.
    // What a child of an element holding `environment` inherits of it, under the registrations as they are now.
    [[nodiscard]] RefPtr<CustomPropertyData const> inheritable_custom_property_environment(Layout::BegunRead const& read, CustomPropertyData const& environment) const;
    // The custom properties the engine's environment `identity` declares of its own, over `inherited` rather than the
    // environment the engine resolved them over. Nothing where `identity` is not an environment the engine resolved.
    [[nodiscard]] RefPtr<CustomPropertyData const> engine_custom_property_declarations_over(Layout::BegunRead const& read, u64 identity, RefPtr<CustomPropertyData const> const& inherited) const;

    // Refreshes a previously published style outside the drive: re-runs the animated element style
    // adjustments and leaves the non-inherited-property inheritance invalidation mark itself.
    void collect_animations_into(Layout::BegunRead const& read, DOM::AbstractElement, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>>, ComputedStyleWorkingSet&) const;
    // One element's animations, refreshed over the record it holds as `collect_animations_into()` refreshes them.
    struct AnimationRefreshRequest {
        DOM::AbstractElement abstract_element;
        ReadonlySpan<GC::Ref<Animations::KeyframeEffect>> effects;
        ComputedStyleWorkingSet& computed_properties;
    };
    // Refreshes the animations of elements of this document, sampling them all in one call of the style engine.
    void refresh_animations_into_each(Layout::BegunRead const& read, ReadonlySpan<AnimationRefreshRequest>) const;

    void apply_animation_definitions(DOM::AbstractElement&, ReadonlySpan<ComputedValuesFFI::FfiComputedAnimation> animation_definitions, bool in_display_none_subtree) const;
    // Applies the animation plan the record an element or pseudo-element holds decides, for a record the style engine
    // settled, where applying it would change anything.
    void apply_settled_animation_plan(Layout::BegunRead const& read, DOM::AbstractElement&) const;
    // Starts the CSS animations of an element or pseudo-element and composes its animations over the record the style
    // engine settled for it, once the host has installed it.
    void compose_installed_engine_record(Layout::BegunRead const& read, DOM::AbstractElement, InstalledStyle const& before_change_style) const;
    [[nodiscard]] bool has_animations_to_compose(DOM::AbstractElement) const;
    void stop_publishing_animated_custom_properties(DOM::AbstractElement) const;

    static NonnullRefPtr<StyleValue const> compute_font_size(NonnullRefPtr<StyleValue const> const& absolutized_value, int computed_math_depth, Optional<DOM::AbstractElement> const& inheritance_parent, CSSPixels initial_font_size = InitialValues::font_size());
    static NonnullRefPtr<StyleValue const> compute_font_style(NonnullRefPtr<StyleValue const> const& absolutized_value);
    static NonnullRefPtr<StyleValue const> compute_font_weight(NonnullRefPtr<StyleValue const> const& absolutized_value, Optional<DOM::AbstractElement> const& inheritance_parent);
    static NonnullRefPtr<StyleValue const> compute_font_width(NonnullRefPtr<StyleValue const> const& absolutized_value);

    [[nodiscard]] NonnullRefPtr<ComputedStyleWorkingSet> reconstruct_computed_properties(ComputedValues const&) const;
    void apply_animated_properties_to_reconstruction(ComputedStyleWorkingSet&, ComputedValues const&) const;
    [[nodiscard]] NonnullRefPtr<ComputedStyleWorkingSet> reconstruct_computed_properties_for_animation(InstalledStyle const&) const;

    void begin_transition_stabilization_epoch();
    // https://drafts.csswg.org/css-transitions-2/#defining-before-change-style
    // Keeps `before_change_style` as the style a later pass of the stabilization epoch decides the element's
    // transitions against, unless a pass already kept one.
    void record_transition_stabilization_baseline(DOM::AbstractElement, InstalledStyle const& before_change_style) const;
    // Keeps `before_change_style` that way where the element's style scope can run a later pass at all.
    void record_transition_baseline_for_later_passes(DOM::AbstractElement, InstalledStyle const& before_change_style) const;
    // Runs the whole transition step for an installed record, against the record the element moved away from.
    // Returns what publishing a started transition's values invalidates, which the caller reacts to like to the rest
    // of the style change.
    [[nodiscard]] RequiredInvalidationAfterStyleChange run_transition_step_for_installed_record(Layout::BegunRead const& read, DOM::AbstractElement, InstalledStyle const& before_change_style) const;
    void commit_transition_stabilization_epoch();
    void for_each_provisional_transition_effect(DOM::AbstractElement const&, Function<void(Animations::KeyframeEffect&)> const&) const;

    // The media features of the current style update, which the style engine copies with each transaction.
    [[nodiscard]] Parser::ValueParserFFI::FfiMediaEnvironment const* ensure_media_environment_for_style_update() const;

private:
    virtual void visit_edges(Visitor&) override;

    [[nodiscard]] StyleRecordID install_computed_style_inputs(Layout::BegunRead const& read, Optional<DOM::AbstractElement>, ComputedValues const&) const;

private:
    // Samples over the record the element holds, which the working set was reconstructed from.
    void collect_animation_effects_into(Layout::BegunRead const& read, DOM::AbstractElement, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>>, ComputedStyleWorkingSet&) const;
    struct AnimationSample;
    [[nodiscard]] NonnullOwnPtr<AnimationSample> begin_animation_sample(Layout::BegunRead const& read, DOM::AbstractElement, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>>, ComputedStyleWorkingSet&) const;
    void finish_animation_sample(AnimationSample&, ComputedValuesFFI::FfiHostAnimationSampleResult const&) const;
    void finish_animation_refresh(Layout::BegunRead const& read, DOM::AbstractElement, ComputedStyleWorkingSet&) const;
    void publish_animated_custom_properties(ComputedStyleWorkingSet&, DOM::AbstractElement) const;
    [[nodiscard]] bool animated_custom_properties_may_move(DOM::AbstractElement, size_t animated_value_count) const;
    void invalidate_animated_custom_property_readers(DOM::AbstractElement, bool recompute_style) const;
    RefPtr<ComputedStyleWorkingSet> start_needed_transitions(Layout::BegunRead const& read, DOM::AbstractElement, InstalledStyle const& before_change_style) const;
    [[nodiscard]] bool has_provisional_transition_states(DOM::AbstractElement) const;
    void finalize_animated_box_type(Layout::BegunRead const& read, ComputedStyleWorkingSet&, DOM::AbstractElement) const;

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
    // What decides which user-agent and user sheets a document holds, and the namespace its HTML elements are in.
    struct NonAuthorStyleSheetsKey {
        bool in_quirks_mode { false };
        bool needs_mathml_and_svg_user_agent_style_sheets { false };
        bool is_html_document { false };
        StyleSheetState const* user_style_sheet { nullptr };
        bool operator==(NonAuthorStyleSheetsKey const&) const = default;
    };
    // The key the recorded sheets were collected under, if any were.
    [[nodiscard]] Optional<NonAuthorStyleSheetsKey>& non_author_style_sheets_key() { return m_non_author_style_sheets_key; }

    [[nodiscard]] StyleEngineRuleID style_engine_rule_id_for(Layout::BegunRead const& read, RustRule const&) const;
    [[nodiscard]] SheetID style_engine_sheet_id_for(StyleSheetState const&) const;
    void set_style_engine_sheet_id_for(StyleSheetState&, SheetID);

    [[nodiscard]] HashMap<SharedCompiledStyleSheetKey, RefPtr<SharedCompiledStyleSheet>>& shared_compiled_style_sheets() { return m_shared_compiled_style_sheets; }
    [[nodiscard]] Vector<SharedCompiledStyleSheetKey>& detached_shared_compiled_style_sheets() { return m_detached_shared_compiled_style_sheets; }
    // How many shared compiled sheets a document keeps after no tree scope holds them.
    static constexpr size_t max_detached_shared_compiled_style_sheets = 8;

    // The reverse of a node's style node identity. StyleEngine plans in identities; turning a
    // plan back into nodes needs this, and it is maintained at exactly the two points the
    // identity itself is.
    void register_style_node(StyleNodeID style_node_id, DOM::Node&);
    void ensure_style_node_slot(StyleNodeID);
    void unregister_style_node(StyleNodeID style_node_id);
    [[nodiscard]] GC::Ptr<DOM::Element> element_for_style_node(StyleNodeID style_node_id) const;
    [[nodiscard]] GC::Ptr<DOM::Node> node_for_style_node(StyleNodeID style_node_id) const;
    // The node a style node id the render clock's lanes report names, or none where the id left its node since the
    // rendering update the lanes follow took them in last: the id may name another node by now, which the lanes, on a
    // fork of the render state from before, never saw.
    [[nodiscard]] GC::Ptr<DOM::Node> node_for_lane_style_node(StyleNodeID style_node_id) const;
    [[nodiscard]] GC::Ptr<DOM::Element> element_for_lane_style_node(StyleNodeID style_node_id) const;
    // Forgets the style node ids that left their nodes, once a rendering update took in what the lanes reported.
    void forget_style_nodes_retired_beside_lanes() { m_style_nodes_retired_beside_lanes.clear(); }
    void prepare_elements_for_style_computation();

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
    mutable double m_root_element_effective_zoom { 1 };

    mutable u64 m_style_update_depth { 0 };
    mutable u64 m_inheritance_chain_read_depth { 0 };
    mutable Optional<MediaEnvironmentSnapshot> m_style_update_media_environment;
    mutable Optional<Parser::ValueParserFFI::FfiMediaEnvironment> m_style_update_ffi_media_environment;
    u64 m_viewport_environment_version { 0 };
    // What each environment built here hands down, by its identity, under the registrations of the generation below.
    struct InheritableCustomPropertyEnvironment {
        NonnullRefPtr<CustomPropertyData const> environment;
        RefPtr<CustomPropertyData const> inheritable;
        bool hands_down_itself { false };
    };
    mutable HashMap<u64, InheritableCustomPropertyEnvironment> m_inheritable_custom_property_environments;
    mutable u64 m_inheritable_custom_property_environments_generation { 0 };

    struct ProvisionalTransitionState {
        GC::Ptr<DOM::Element> element;
        Optional<PseudoElement> pseudo_element;
        PropertyID property_id;
        GC::Ptr<CSSTransition> committed_transition;
        GC::Ptr<CSSTransition> proposed_transition;
        StyleValueFFI::FfiTransitionActionKind action {};
        bool has_decision { false };
    };
    // Whether the animation collection of the computation in progress resolved a keyframe-borne
    // `inherit` for a non-inherited property; folded into the explicit-inheritance bookkeeping.
    mutable Vector<ProvisionalTransitionState> m_provisional_transition_states;
    mutable HashMap<u64, size_t> m_provisional_transition_state_indices;
    mutable HashMap<u64, Vector<size_t>> m_provisional_transition_state_indices_by_target;
    // Whether the engine keeps a before-change style the current stabilization epoch recorded.
    mutable bool m_transition_baselines_recorded { false };

    ComputationContext make_computation_context_for_property(PropertyID, ComputedStyleWorkingSet const&, Optional<DOM::AbstractElement>) const;
    CSSPixelRect m_viewport_rect;

    mutable StyleEngine m_style_engine;
    mutable u32 m_style_record_view_epoch_depth { 0 };
    u32 m_style_installation_depth { 0 };
    u64 m_subtree_style_generation { 0 };
    u64 m_installation_subtree_style_generation { 0 };
    mutable HashMap<u64, NonnullRefPtr<CustomPropertyData const>> m_custom_property_environments_noted_during_installation;
    // Indexed by each kind's dense index; see style_node_is_text(). The element-kind table also
    // holds shadow roots: a root gets no style, but it has a StyleNodeID of its own.
    Vector<GC::Ptr<DOM::Node>> m_element_style_nodes;
    Vector<GC::Ptr<DOM::Text>> m_text_style_nodes;
    HashTable<StyleNodeID> m_style_nodes_retired_beside_lanes;
    // The root each scope numbers, by scope minus one.
    Vector<GC::Weak<DOM::ShadowRoot>> m_shadow_roots_by_tree_scope;
    Vector<NonAuthorStyleSheet> m_non_author_style_sheets;
    Optional<NonAuthorStyleSheetsKey> m_non_author_style_sheets_key;
    HashMap<RefPtr<StyleSheetState const>, SheetID> m_constructed_sheet_ids;
    HashMap<SharedCompiledStyleSheetKey, RefPtr<SharedCompiledStyleSheet>> m_shared_compiled_style_sheets;
    // The shared compiled sheets that no tree scope holds any longer, least recently detached first.
    Vector<SharedCompiledStyleSheetKey> m_detached_shared_compiled_style_sheets;
    HashMap<u64, WeakPtr<StyleSheetState const>> m_style_engine_sheet_sources;
};

}
