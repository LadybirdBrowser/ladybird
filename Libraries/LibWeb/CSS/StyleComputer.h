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
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CascadeOrigin.h>
#include <LibWeb/CSS/ComputedStyleWorkingSet.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/SelectorMatching.h>
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
    static constexpr bool OVERRIDES_FINALIZE = true;

    static void for_each_property_expanding_shorthands(PropertyID, StyleValue const&, Function<void(PropertyID, StyleValue const&)> const& set_longhand_property);
    static NonnullRefPtr<StyleValue const> get_non_animated_inherit_value(PropertyID, DOM::AbstractElement);
    struct AnimatedInheritValue {
        NonnullRefPtr<StyleValue const> value;
        AnimatedPropertyResultOfTransition is_result_of_transition;
    };
    static Optional<AnimatedInheritValue> get_animated_inherit_value(PropertyID, DOM::AbstractElement);

    static Optional<Utf16String> user_agent_style_sheet_source(Utf16View name);

    explicit StyleComputer(DOM::Document&);
    virtual ~StyleComputer() override = default;

    DOM::Document& document() { return m_document; }
    DOM::Document const& document() const { return m_document; }

    [[nodiscard]] NonnullRefPtr<ComputedValues const> create_document_style() const;

    // Materialize the complete computed view for one exact StyleEngine target and publish its
    // StyleRecord assignment.
    [[nodiscard]] NonnullRefPtr<ComputedValues const> materialize_style_record(DOM::AbstractElement, Optional<bool&> did_change_custom_properties = {}, StyleEngineMatchResult* = nullptr, Optional<StyleEngine::StyleRecordDelta&> = {}, u8 inherited_style_groups = 0) const;
    [[nodiscard]] bool can_reuse_style_after_inherited_custom_property_change(DOM::Element&) const;
    void record_style_custom_property_reference(DOM::Element&, Utf16FlyString const&) const;
    // Compute the cascade supplied by rules, presentational hints, and inheritance while excluding the element's
    // inline declaration. Editing uses this to identify transport-only style without mutating the live element.
    [[nodiscard]] NonnullRefPtr<ComputedStyleWorkingSet> compute_properties_without_inline_style(DOM::AbstractElement) const;
    [[nodiscard]] RefPtr<ComputedValues const> compute_pseudo_element_style_if_needed(DOM::AbstractElement, Optional<bool&> did_change_custom_properties, StyleEngineMatchResult* = nullptr, Optional<StyleEngine::StyleRecordDelta&> = {}) const;
    [[nodiscard]] JsonArray collect_devtools_applied_style_rules(DOM::AbstractElement, bool include_inherited, bool include_user_agent_styles);

    // The way back from a StyleEngine rule identity to what that rule contributes to the cascade.
    // Matching in the engine answers with identities; the cascade needs a declaration and the place
    // it sits in. The map is per document because the identities are, and it is not per scope: a
    // `:host` rule decides for an element whose own scope is not the one the rule lives in.
    // Keyed by the rule rather than by its identity, because the two are written at different
    // times: the identity when the sheet is fed to the engine, the rest when the rule cache is
    // built. Keying by the rule lets either come first.
    void register_style_engine_rule_target(CSSRule const&, StyleEngineRuleTarget);
    void register_style_engine_rule_identity(StyleEngineRuleID rule_id, CSSRule const&);
    [[nodiscard]] Optional<StyleEngineRuleTarget> style_engine_rule_target(StyleEngineRuleID rule_id) const;
    void invalidate_parsed_substitutions_for_rule(StyleEngineRuleID rule_id) const;

    static CSSPixels default_user_font_size();
    static void ensure_style_metadata_tables_installed();
    static CSSPixels absolute_size_mapping(AbsoluteSize, CSSPixels default_font_size);
    [[nodiscard]] RefPtr<StyleValue const> recascade_font_size_if_needed(DOM::AbstractElement, CascadedProperties&, bool& depends_on_viewport_metrics) const;

    void set_viewport_rect(Badge<DOM::Document>, CSSPixelRect const& viewport_rect) { m_viewport_rect = viewport_rect; }
    [[nodiscard]] CSSPixelRect const& viewport_rect_for_style_environment() const { return m_viewport_rect; }
    [[nodiscard]] Length::FontMetrics const& root_element_font_metrics() const { return m_root_element_font_metrics; }
    [[nodiscard]] bool root_element_font_metrics_depend_on_viewport_metrics() const { return m_root_element_font_metrics_depend_on_viewport_metrics; }

    // Drop caches whose keys contain inputs that are stable only within one engine transaction.
    // Style sharing has a self-validating key and survives ordinary transaction boundaries.
    void prepare_for_style_engine_transaction() const;

    // Forget every style one element computed on another's behalf. See m_style_sharing_cache.
    void drop_style_sharing_cache() const;

    struct ComputedStyleInvalidation {
        RequiredInvalidationAfterStyleChange invalidation;
        bool any_computed_value_changed { false };
    };
    [[nodiscard]] Optional<ComputedStyleInvalidation> cached_computed_style_invalidation(StyleEngine::StyleRecordDelta const&, bool element_folds_transform_into_layout) const;
    void cache_computed_style_invalidation(StyleEngine::StyleRecordDelta const&, bool element_folds_transform_into_layout, ComputedStyleInvalidation) const;

    // Publish a computed style built outside the ordinary cascade path, such as an inherited-group
    // swap, so StyleEngine's final node-to-style relation remains authoritative.
    [[nodiscard]] StyleEngine::StyleRecordDelta publish_computed_style_inputs(DOM::AbstractElement, ComputedValues const&) const;
    // Give a layout-only variant of an element or pseudo-element style an authoritative record
    // without replacing the StyleEngine assignment of its DOM target.
    [[nodiscard]] StyleRecordID intern_computed_style_inputs(DOM::AbstractElement, ComputedValues const&) const;
    // Anonymous layout boxes have no style target, but their immutable group tuple is still an
    // authoritative shared record rather than layout-owned complete computed values.
    [[nodiscard]] StyleRecordID intern_anonymous_layout_style(ComputedValues const&) const;

    // Replace a fully inheriting element's inherited groups without re-running its cascade or
    // driving every longhand. The result is shared by every element making the same transition.
    [[nodiscard]] RefPtr<ComputedValues const> inherited_style_group_swap(DOM::Element&, ComputedValues const& old_values, ComputedValues const& new_parent_values) const;

    [[nodiscard]] ComputedStyleRecordView computed_style_record_view(StyleRecordID) const;

    // Presence and display:none-subtree placement of a style record, read without materializing a full style record view.
    struct StyleRecordStatus {
        bool present { false };
        bool in_display_none_subtree { false };
    };
    [[nodiscard]] StyleRecordStatus style_record_status(StyleRecordID) const;

    [[nodiscard]] void const* style_record_payloads(StyleRecordID) const;
    void pin_style_record(StyleRecordID) const;
    void unpin_style_record(StyleRecordID) const;
    [[nodiscard]] u64 computed_style_record_view_pin_count() const { return m_computed_style_record_view_pin_count; }

    // Two elements whose cascade declares the same custom properties against the same inherited
    // environment hold the same environment, so they are given one object rather than an object
    // each. What that buys is not the bytes: an environment resolves once, and a child naming its
    // parent's environment by identity is told the truth when two parents agree.
    [[nodiscard]] NonnullRefPtr<CustomPropertyData const> intern_custom_property_data(NonnullRefPtr<CustomPropertyData const>) const;
    void sweep_custom_property_environments() const;

    // Whether the collection refreshes a previously published style outside the drive; a refresh
    // re-runs the animated element style adjustments and leaves the non-inherited-property
    // inheritance invalidation mark itself.
    enum class AnimationRefresh {
        No,
        Yes,
    };
    void collect_animations_into(DOM::AbstractElement, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>>, ComputedStyleWorkingSet&, AnimationRefresh) const;

    // `explicitly_inherited_non_inherited_style_groups` reports the style groups whose values the
    // computation read from the half of the style it inherits from that a child normally cannot
    // see, which decides whether its answer can be offered to another element.
    [[nodiscard]] NonnullRefPtr<ComputedStyleWorkingSet> compute_properties(DOM::AbstractElement, CascadedProperties&, u64 matching_pseudo_element_styles, u32* explicitly_inherited_non_inherited_style_groups = nullptr, ComputedValues const* previous_values = nullptr, u32 computed_group_mask = ComputedValues::all_style_groups, u64 const* computed_properties_to_evaluate = nullptr, ComputedValues const* inheritance_parent_values = nullptr, bool stop_after_longhand_drive = false) const;

    void compute_property_values(ComputedStyleWorkingSet&, Optional<DOM::AbstractElement>) const;
    void apply_post_compute_adjustments(ComputedStyleWorkingSet&, DOM::AbstractElement) const;
    void process_animation_definitions(ComputedStyleWorkingSet const& computed_properties, CascadedProperties const&, DOM::AbstractElement& abstract_element) const;

    NonnullRefPtr<StyleValue const> compute_value_of_custom_property(ComputedStyleWorkingSet const*, AbstractOrHypotheticalElement const&, Utf16FlyString const& name) const;
    NonnullRefPtr<StyleValue const> resolve_unresolved_style_value(AbstractOrHypotheticalElement, PropertyNameAndID const&, UnresolvedStyleValue const&) const;
    ComputationContext fallback_computation_context_for_custom_property(AbstractOrHypotheticalElement const&) const;

    static NonnullRefPtr<StyleValue const> compute_value_of_property(PropertyID, NonnullRefPtr<StyleValue const> const& specified_value, Function<NonnullRefPtr<StyleValue const>(PropertyID)> const& get_property_specified_value, ComputationContext const&, double device_pixels_per_css_pixel);
    static NonnullRefPtr<StyleValue const> compute_animation_name(NonnullRefPtr<StyleValue const> const& absolutized_value);
    static NonnullRefPtr<StyleValue const> compute_border_or_outline_width(NonnullRefPtr<StyleValue const> const& absolutized_value, double device_pixels_per_css_pixel);
    static NonnullRefPtr<StyleValue const> compute_corner_shape(NonnullRefPtr<StyleValue const> const& absolutized_value);
    static NonnullRefPtr<StyleValue const> compute_font_feature_tag_value_list(NonnullRefPtr<StyleValue const> const& absolutized_value);
    static NonnullRefPtr<StyleValue const> compute_math_depth(NonnullRefPtr<StyleValue const> const& absolutized_value, Optional<DOM::AbstractElement> const& inheritance_parent);
    static NonnullRefPtr<StyleValue const> compute_font_size(NonnullRefPtr<StyleValue const> const& absolutized_value, int computed_math_depth, Optional<DOM::AbstractElement> const& inheritance_parent, CSSPixels initial_font_size = InitialValues::font_size());
    static NonnullRefPtr<StyleValue const> compute_font_style(NonnullRefPtr<StyleValue const> const& absolutized_value);
    static NonnullRefPtr<StyleValue const> compute_font_weight(NonnullRefPtr<StyleValue const> const& absolutized_value, Optional<DOM::AbstractElement> const& inheritance_parent);
    static NonnullRefPtr<StyleValue const> compute_font_width(NonnullRefPtr<StyleValue const> const& absolutized_value);
    static NonnullRefPtr<StyleValue const> compute_line_height(NonnullRefPtr<StyleValue const> const& absolutized_value, CSSPixels computed_font_size);
    static NonnullRefPtr<StyleValue const> compute_transform_origin(NonnullRefPtr<StyleValue const> const& absolutized_value);

    [[nodiscard]] NonnullRefPtr<ComputedValues const> build_computed_values(ComputedStyleWorkingSet&, DOM::AbstractElement, StyleScope const&, ComputedValues const* previous_base = nullptr, u32 groups_to_apply = ComputedValues::all_style_groups) const;
    // The animation-frame variant: keep the previous style's base and rebuild only the groups the
    // animated properties write, falling back to the full build when a touched group is unknown.
    [[nodiscard]] NonnullRefPtr<ComputedValues const> build_animated_computed_values(ComputedStyleWorkingSet&, DOM::AbstractElement, StyleScope const&, ComputedValues const& previous_values) const;
    [[nodiscard]] NonnullRefPtr<ComputedStyleWorkingSet> reconstruct_computed_properties(ComputedValues const&) const;
    void apply_animated_properties_to_reconstruction(ComputedStyleWorkingSet&, ComputedValues const&) const;

    void begin_transition_stabilization_epoch();
    void record_transition_stabilization_baseline(DOM::AbstractElement) const;
    void commit_transition_stabilization_epoch();
    void for_each_provisional_transition_effect(DOM::AbstractElement const&, Function<void(Animations::KeyframeEffect&)> const&) const;

private:
    virtual void finalize() override;

    virtual void visit_edges(Visitor&) override;

    [[nodiscard]] StyleEngine::StyleRecordDelta record_computed_style_inputs(Optional<DOM::AbstractElement>, ComputedValues const&, StyleNodeID style_node_id) const;

    enum class ComputeStyleMode {
        Normal,
        CreatePseudoElementStyleIfNeeded,
    };

    enum class IncludeInlineStyle : u8 {
        No,
        Yes,
    };

public:
    // One declaration block the cascade applies, and where it sits. This is what a match becomes
    // once the rule it names has been turned back into what that rule contributes: the cascade never
    // asks a match anything else, so this is the whole of the boundary between matching and
    // cascading, and either matcher can produce it.
    struct CascadeContribution {
        GC::Ptr<CSSStyleProperties const> declaration;
        StyleEngineRuleID style_engine_rule_id;
        u32 semantic_declaration_id { 0 };
        // The shadow root the rule decides from, which is not always the one the element is in.
        GC::Ptr<DOM::ShadowRoot const> source_shadow_root;
        Optional<Utf16FlyString> layer_name;
        CascadeOrigin cascade_origin { CascadeOrigin::Author };
        u32 author_context_index { 0 };
        u32 layer_index { 0 };
    };

    // Everything the cascade needs that is not the element's own declarations, in the order it
    // applies them.
    struct CascadeInput : public RefCounted<CascadeInput> {
        Vector<CascadeContribution> contributions;
        u32 author_context_count { 0 };
        // The author context the element's own inline style belongs to.
        Optional<u32> inline_style_context_index;
        // Which synthetic pseudo-elements some rule decides for, as a bit per pseudo-element. The
        // element's own style carries it so a consumer knows which pseudo-elements to materialize.
        u64 matching_pseudo_element_styles { 0 };
        // A traversal-local exact identity for the complete rule-match input. Present only when
        // StyleEngine proved the result reusable from the element's ancestry context.
        Optional<u32> match_signature;
    };

    // Two elements given the same style input compute the same style, so the second one can take the
    // first one's answer instead of deriving it again. The key is everything the computation is
    // allowed to read: every declaration that decides for the element in the order it applies, the
    // inherited context it computes against, and the shape of the element itself. Anything else the
    // computation turns out to have read - its container, its attributes, its place among its
    // siblings - takes the result out of the cache rather than being keyed on, so the key stays a
    // fixed size and the escape hatches stay honest.
    struct StyleSharingCandidate {
        // Empty until the cascade has run; unusable when the element is not a sharing candidate.
        Vector<u64> key;
        // Every value the key names by its address rather than by its owner. A presentational hint
        // is mapped afresh for each element and dies with the cascade that read it, so without this
        // the next element's hint is free to land on the same address and read as the same value.
        Vector<NonnullRefPtr<StyleValue const>> pinned_key_values;
        StyleGroupPayloadPins pinned_parent_groups;
        // The style the key names the inherited groups of, for the same reason.
        StyleRecordID parent_style_record_identity;
        bool is_candidate { false };
        // Whether the cascade holds anything that reads the inherited custom property environment:
        // a declaration of a custom property, or a value with a substitution still to make. Only
        // then does the key name that environment, which is an object the element's own ancestors
        // mint afresh whenever any of them recomputes.
        bool cascade_reads_custom_properties { false };
        RefPtr<CustomPropertyData const> pinned_parent_custom_property_data;
        // The style groups whose values the computation read from the inherited style's
        // non-inherited half, which the key does not name.
        u32 explicitly_inherited_non_inherited_style_groups { 0 };
        // Set when an entry with this key was found, in which case nothing was computed.
        RefPtr<ComputedValues const> shared_values;
        Optional<StyleRecordID> shared_style_record_identity;
        Optional<u64> new_style_sharing_entry_hash;
        // Set when nothing this element's last computation read has moved, in which case the style
        // it produced is still the answer and nothing was computed either.
        RefPtr<ComputedValues const> reused_values;
        // The exact cascade winner delta's conservative computed dependency closure. Present only
        // when a preceding base style is available to supply every group outside the closure.
        Optional<u32> computed_groups_to_rebuild;
        Optional<Array<u64, (number_of_longhand_properties + 63) / 64>> computed_properties_to_evaluate;
        bool computed_property_closure_is_exact { false };
        u8 inherited_style_groups { 0 };
    };

private:
    void clear_style_sharing_cache() const;
    [[nodiscard]] NonnullRefPtr<ComputedValues const> build_and_share_computed_values(NonnullRefPtr<ComputedStyleWorkingSet>, DOM::AbstractElement, StyleScope const&, StyleSharingCandidate&) const;
    [[nodiscard]] static Vector<GC::Ptr<DOM::ShadowRoot const>, 4> author_context_shadow_roots(DOM::AbstractElement);

    // The same input, from StyleEngine's own matching. Empty when the engine could not answer for
    // this element, which is not the same as an element nothing decides for.
    [[nodiscard]] RefPtr<CascadeInput const> style_engine_cascade_input(DOM::AbstractElement, StyleEngineMatchResult* = nullptr) const;

    [[nodiscard]] RefPtr<ComputedStyleWorkingSet> compute_style_impl(DOM::AbstractElement, ComputeStyleMode, Optional<bool&> did_change_custom_properties, StyleScope const&, IncludeInlineStyle, StyleEngineMatchResult* = nullptr, StyleSharingCandidate* = nullptr) const;
    [[nodiscard]] NonnullRefPtr<CascadedProperties> compute_cascaded_values(DOM::AbstractElement, CascadeInput const&, IncludeInlineStyle, StyleSharingCandidate* sharing = nullptr, Vector<StyleProperty> const* precomputed_presentational_hints = nullptr) const;
    void collect_animation_effects_into(DOM::AbstractElement, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>>, ComputedStyleWorkingSet&) const;
    void compute_custom_properties(ComputedStyleWorkingSet&, DOM::AbstractElement) const;
    Vector<GC::Ref<Animations::KeyframeEffect>> start_needed_transitions(ComputedValues const& old_style, ComputedStyleWorkingSet& new_style, DOM::AbstractElement) const;
    void resolve_effective_overflow_values(ComputedStyleWorkingSet&) const;
    void adjust_element_style_if_needed(ComputedStyleWorkingSet&, DOM::AbstractElement) const;
    void adjust_animated_element_style_if_needed(ComputedStyleWorkingSet&, DOM::AbstractElement) const;

    [[nodiscard]] CSSPixelRect viewport_rect() const { return m_viewport_rect; }

public:
    // The document's StyleEngine.
    [[nodiscard]] StyleEngine& style_engine() { return m_style_engine; }
    [[nodiscard]] StyleEngine const& style_engine() const { return m_style_engine; }

    // The user-agent and user sheets attached to this document's StyleEngine. User-agent sheets are
    // shared between documents, so a per-document identity cannot live on the sheet itself.
    struct NonAuthorStyleSheet {
        GC::Ptr<CSSStyleSheet> sheet;
        SheetID sheet_id;
    };
    [[nodiscard]] Vector<NonAuthorStyleSheet>& non_author_style_sheets() { return m_non_author_style_sheets; }

    // The user-agent sheets are process-wide singletons, so their rules cannot carry a StyleEngine
    // identity the way an author rule does: every document compiles the same rule objects into its
    // own engine, and the last one to do so would own the field. Their identities live here, per
    // document, next to the sheets they came from and cleared with them.
    [[nodiscard]] HashMap<GC::Ptr<CSSRule const>, StyleEngineRuleID>& non_author_rule_ids() { return m_non_author_rule_ids; }

    // A constructed sheet's rules hold per-document identities for the same reason, but in their own
    // map: the non-author rebuild clears its map wholesale, and must not wipe adopted sheets' rules.
    [[nodiscard]] HashMap<GC::Ptr<CSSRule const>, StyleEngineRuleID>& constructed_rule_ids() { return m_constructed_rule_ids; }

    [[nodiscard]] StyleEngineRuleID style_engine_rule_id_for(CSSRule const&) const;
    [[nodiscard]] SheetID style_engine_sheet_id_for(CSSStyleSheet const&) const;
    void set_style_engine_sheet_id_for(CSSStyleSheet&, SheetID);

    // The reverse of an element's style node identity. StyleEngine plans in identities; turning a
    // plan back into elements needs this, and it is maintained at exactly the two points the
    // identity itself is.
    void register_style_node(StyleNodeID style_node_id, DOM::Element&);
    void ensure_style_node_slot(StyleNodeID);
    void unregister_style_node(StyleNodeID style_node_id);
    [[nodiscard]] GC::Ptr<DOM::Element> element_for_style_node(StyleNodeID style_node_id) const;
    void prepare_elements_for_style_computation();
    void for_each_style_node(Function<void(DOM::Element&)>) const;

    // Style scopes are numbered per document, with zero naming the document's own scope. A scope is
    // never reused, so a sheet detached with an identity that has been retired detaches nothing
    // rather than something else.
    [[nodiscard]] TreeScopeID allocate_tree_scope() { return ++m_next_tree_scope; }

private:
    [[nodiscard]] Length::FontMetrics calculate_root_element_font_metrics(ComputedStyleWorkingSet const&) const;

    GC::Ref<DOM::Document> m_document;

    Length::FontMetrics m_default_font_metrics;
    mutable Length::FontMetrics m_root_element_font_metrics;
    mutable bool m_root_element_font_metrics_depend_on_viewport_metrics { false };

    mutable Optional<ComputationContext> m_cached_font_computation_context;
    mutable Optional<ComputationContext> m_cached_line_height_computation_context;
    mutable Optional<ComputationContext> m_cached_generic_computation_context;
    // The style most recently built, kept as a payload donor: a run of elements computing the same
    // style shares group payloads through it, which no parent or previous-style adoption can do.
    mutable RefPtr<ComputedValues const> m_last_built_computed_values;

    // What one element's computation produced, for the next element with the same key. Only the
    // effects an element carries away from its own computation are here; everything else the
    // computation did was to values that are in the shared style.
    struct StyleSharingEntry {
        Vector<u64> key;
        StyleGroupPayloadPins pinned_parent_groups;
        Vector<NonnullRefPtr<StyleValue const>> pinned_key_values;
        StyleRecordID parent_style_record_identity;
        // The style groups whose values the computation read from the inherited style's
        // non-inherited half, which the key does not name. An entry with any such group answers
        // only for an element inheriting from that same style.
        u32 explicitly_inherited_non_inherited_style_groups { 0 };
        NonnullRefPtr<ComputedValues const> values;
        RefPtr<CustomPropertyData const> custom_property_data;
        Optional<StyleRecordID> style_record_identity;
        Vector<u64> style_input_declaration_words;
        Vector<NonnullRefPtr<StyleValue const>> pinned_style_input_values;
        bool cascade_declares_custom_properties { false };
        Vector<Utf16FlyString> custom_property_references;
        bool style_uses_var_css_function { false };
        bool style_uses_inherit_css_function { false };
    };
    // Keyed by the hash of the key, since two keys of different length can share a bucket. Mutable
    // declaration blocks and the document environment carry revisions in the key, so entries remain
    // valid across style transactions. Bound the retained set so obsolete revisions cannot grow it
    // without limit on a page that continuously edits declarations.
    static constexpr size_t maximum_persistent_style_sharing_entries = 8192;
    mutable HashMap<u64, Vector<StyleSharingEntry>> m_style_sharing_cache;
    mutable size_t m_style_sharing_cache_entry_count { 0 };
    mutable u64 m_style_sharing_transaction_generation { 0 };
    // The computed-property consequence of one semantic style transition. Element-dependent
    // observer consequences are added separately and are never cached by style-record identity.
    struct ComputedStyleInvalidationCacheEntry {
        StyleRecordID old_style_record;
        StyleRecordID new_style_record;
        bool element_folds_transform_into_layout { false };
        ComputedStyleInvalidation result;
    };
    mutable HashMap<u32, Vector<ComputedStyleInvalidationCacheEntry>> m_computed_style_invalidation_cache;
    struct InheritedStyleGroupSwapEntry {
        StyleRecordID old_style_record;
        Array<void const*, ComputedValues::inherited_style_group_count> new_parent_groups;
        NonnullRefPtr<ComputedValues const> result;
    };
    mutable Vector<InheritedStyleGroupSwapEntry> m_inherited_style_group_swaps;
    mutable bool m_materializing_for_targeted_style_update { false };
    // The word buffer one element's record gives up, reused by the next element's.
    mutable OwnPtr<StyleInputRecord> m_style_input_record_scratch;
    // Interned custom property environments, keyed by a hash of what decides one: the environment it
    // inherits from, and every name and value it declares.
    mutable HashMap<u64, Vector<NonnullRefPtr<CustomPropertyData const>>> m_custom_property_environments;

    // What one element's cascaded custom property declarations resolved to. A rule that declares
    // custom properties on every element - which is how utility frameworks carry their theme - hands
    // the same list to every element it matches, and the environment that list produces depends on
    // nothing but the list and the environment it inherits.
    struct CascadedCustomPropertyEnvironment {
        Vector<u64> key;
        RefPtr<CustomPropertyData const> parent;
        RefPtr<CustomPropertyData const> result;
    };
    mutable HashMap<u64, Vector<CascadedCustomPropertyEnvironment>> m_cascaded_custom_property_environments;

    mutable Vector<u64> m_cascaded_custom_property_key_scratch;

    // The custom property names one declared value references through literal var() names, read off its tokens once
    // per distinct value. A value whose references are not all in its tokens - a var() whose name slot is itself
    // substituted - is marked instead, and its user treats it as able to reference anything. The keepalive reference
    // is what makes the pointer key safe against reuse.
    struct CustomPropertyReferenceScan {
        NonnullRefPtr<StyleValue const> value;
        Vector<Utf16FlyString> references;
        bool all_references_visible { true };
    };
    CustomPropertyReferenceScan const& custom_property_reference_scan(NonnullRefPtr<StyleValue const> const&) const;
    mutable HashMap<void const*, CustomPropertyReferenceScan> m_custom_property_reference_scans;

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

    // What one unresolved declaration parses to after var() substitution. The substituted source
    // is a pure function of the declaration, the custom property environment, and the registration
    // generation. Entries survive transaction boundaries and are swept with dead declarations and
    // custom property environments so their parsed value identities remain stable while reusable.
    struct ParsedSubstitution {
        u64 custom_property_environment_identity { 0 };
        u64 registration_generation { 0 };
        PropertyID property_id;
        NonnullRefPtr<StyleValue const> parsed;
    };
    mutable HashMap<StyleEngineRuleID, Vector<ParsedSubstitution>> m_parsed_substitutions;
    mutable u64 m_parsed_substitution_registration_generation { 0 };
    [[nodiscard]] u64 parsed_substitution_cache_bytes() const;
    void settle_parsed_substitution_cache() const;

    // While one element's own custom properties resolve in dependency order, the values already final for it. A
    // nested var() naming one reads the finished answer instead of resolving the value again. Members of a reference
    // cycle are never in here while their cycle resolves, so the substitution guards remain the only thing that
    // decides what a cycle is.
    struct ActiveCustomPropertyResolution {
        DOM::AbstractElement element;
        HashMap<Utf16FlyString, NonnullRefPtr<StyleValue const>> finalized;

        void visit_edges(GC::Cell::Visitor& visitor) const
        {
            element.visit(visitor);
        }
    };
    mutable Optional<ActiveCustomPropertyResolution> m_active_custom_property_resolution;

    // The cascade input a match signature names, expanded once and answered from for every other
    // element the traversal proves has the same one. Keyed by the signature and what is being
    // styled, and valid for exactly as long as the sharing cache above is.
    mutable HashMap<u64, NonnullRefPtr<CascadeInput const>> m_style_engine_cascade_input_cache;

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
        // The transition consumer reads only this property's animated before-change value and
        // whether its specified value was currentColor. Pin those derived values for the
        // stabilization epoch instead of retaining another complete computed-style projection.
        RefPtr<StyleValue const> before_change_value;
        bool before_change_value_originates_from_current_color { false };
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
    // Style, layout, or animation feedback can introduce a transition for any property in a later
    // pass. Pin the authoritative before-change record until the stabilization epoch commits.
    mutable HashMap<u64, StyleRecordID> m_transition_stabilization_baselines;

    ComputationContext make_computation_context_for_property(PropertyID, ComputedStyleWorkingSet const&, Optional<DOM::AbstractElement>) const;
    ComputationContext const& get_computation_context_for_property(PropertyID, ComputedStyleWorkingSet const&, Optional<DOM::AbstractElement>) const;
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
    Vector<GC::Ptr<DOM::Element>> m_style_nodes;
    TreeScopeID m_next_tree_scope;
    Vector<NonAuthorStyleSheet> m_non_author_style_sheets;
    HashMap<GC::Ptr<CSSRule const>, StyleEngineRuleID> m_non_author_rule_ids;
    HashMap<GC::Ptr<CSSStyleSheet const>, SheetID> m_constructed_sheet_ids;
    HashMap<GC::Ptr<CSSRule const>, StyleEngineRuleID> m_constructed_rule_ids;
    HashMap<GC::Ptr<CSSRule const>, StyleEngineRuleTarget> m_style_engine_rule_targets;
    HashMap<StyleEngineRuleID, GC::Ptr<CSSRule const>> m_style_engine_rules_by_id;
};

}
