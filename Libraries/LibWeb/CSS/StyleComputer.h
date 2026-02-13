/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CascadeOrigin.h>
#include <LibWeb/CSS/CascadedProperties.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/SelectorEngine.h>
#include <LibWeb/CSS/StyleInvalidationData.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// A counting bloom filter with 2 hash functions.
// NOTE: If a counter overflows, it's kept maxed-out until the whole filter is cleared.
template<typename CounterType, size_t key_bits>
class CountingBloomFilter {
public:
    CountingBloomFilter() { }

    void clear() { __builtin_memset(m_buckets, 0, sizeof(m_buckets)); }

    void increment(u32 key)
    {
        auto& first = bucket1(key);
        if (first < NumericLimits<CounterType>::max())
            ++first;
        auto& second = bucket2(key);
        if (second < NumericLimits<CounterType>::max())
            ++second;
    }

    void decrement(u32 key)
    {
        auto& first = bucket1(key);
        if (first < NumericLimits<CounterType>::max())
            --first;
        auto& second = bucket2(key);
        if (second < NumericLimits<CounterType>::max())
            --second;
    }

    [[nodiscard]] bool may_contain(u32 hash) const
    {
        return bucket1(hash) && bucket2(hash);
    }

private:
    static constexpr u32 bucket_count = 1 << key_bits;
    static constexpr u32 key_mask = bucket_count - 1;

    [[nodiscard]] u32 hash1(u32 key) const { return key & key_mask; }
    [[nodiscard]] u32 hash2(u32 key) const { return (key >> 16) & key_mask; }

    [[nodiscard]] CounterType& bucket1(u32 key) { return m_buckets[hash1(key)]; }
    [[nodiscard]] CounterType& bucket2(u32 key) { return m_buckets[hash2(key)]; }
    [[nodiscard]] CounterType bucket1(u32 key) const { return m_buckets[hash1(key)]; }
    [[nodiscard]] CounterType bucket2(u32 key) const { return m_buckets[hash2(key)]; }

    CounterType m_buckets[bucket_count];
};

class WEB_API StyleComputer final : public GC::Cell {
    GC_CELL(StyleComputer, GC::Cell);
    GC_DECLARE_ALLOCATOR(StyleComputer);

public:
    static void for_each_property_expanding_shorthands(PropertyID, StyleValue const&, Function<void(PropertyID, StyleValue const&)> const& set_longhand_property);
    static NonnullRefPtr<StyleValue const> get_non_animated_inherit_value(PropertyID, DOM::AbstractElement);
    struct AnimatedInheritValue {
        NonnullRefPtr<StyleValue const> value;
        AnimatedPropertyResultOfTransition is_result_of_transition;
    };
    static Optional<AnimatedInheritValue> get_animated_inherit_value(PropertyID, DOM::AbstractElement);

    static Optional<String> user_agent_style_sheet_source(StringView name);

    explicit StyleComputer(DOM::Document&);
    ~StyleComputer();

    DOM::Document& document() { return m_document; }
    DOM::Document const& document() const { return m_document; }

    void reset_ancestor_filter();
    void reset_has_result_cache();
    void push_ancestor(DOM::Element const&);
    void pop_ancestor(DOM::Element const&);

    [[nodiscard]] GC::Ref<ComputedProperties> create_document_style() const;

    [[nodiscard]] GC::Ref<ComputedProperties> compute_style(DOM::AbstractElement, Optional<bool&> did_change_custom_properties = {}) const;
    [[nodiscard]] GC::Ptr<ComputedProperties> compute_pseudo_element_style_if_needed(DOM::AbstractElement, Optional<bool&> did_change_custom_properties) const;

    [[nodiscard]] Vector<MatchingRule const*> collect_matching_rules(DOM::AbstractElement, CascadeOrigin, PseudoClassBitmap& attempted_pseudo_class_matches, Optional<FlyString const> qualified_layer_name = {}) const;

    InvalidationSet invalidation_set_for_properties(Vector<InvalidationSet::Property> const&, StyleScope const&) const;
    bool invalidation_property_used_in_has_selector(InvalidationSet::Property const&, StyleScope const&) const;

    static CSSPixels default_user_font_size();
    static CSSPixels absolute_size_mapping(AbsoluteSize, CSSPixels default_font_size);
    static CSSPixels relative_size_mapping(RelativeSize, CSSPixels inherited_font_size);
    [[nodiscard]] RefPtr<StyleValue const> recascade_font_size_if_needed(DOM::AbstractElement, CascadedProperties&) const;

    void set_viewport_rect(Badge<DOM::Document>, CSSPixelRect const& viewport_rect) { m_viewport_rect = viewport_rect; }

    void collect_animation_into(DOM::AbstractElement, GC::Ref<Animations::KeyframeEffect> animation, ComputedProperties&) const;

    [[nodiscard]] GC::Ref<ComputedProperties> compute_properties(DOM::AbstractElement, CascadedProperties&) const;

    void compute_property_values(ComputedProperties&, Optional<DOM::AbstractElement>) const;
    void process_animation_definitions(ComputedProperties const& computed_properties, DOM::AbstractElement& abstract_element) const;

    [[nodiscard]] inline bool should_reject_with_ancestor_filter(Selector const&) const;

    static NonnullRefPtr<StyleValue const> compute_value_of_custom_property(DOM::AbstractElement, FlyString const& custom_property, Optional<Parser::GuardedSubstitutionContexts&> = {});

    static NonnullRefPtr<StyleValue const> compute_value_of_property(PropertyID, NonnullRefPtr<StyleValue const> const& specified_value, Function<NonnullRefPtr<StyleValue const>(PropertyID)> const& get_property_specified_value, ComputationContext const&, double device_pixels_per_css_pixel);
    static NonnullRefPtr<StyleValue const> compute_animation_name(NonnullRefPtr<StyleValue const> const& absolutized_value);
    static NonnullRefPtr<StyleValue const> compute_border_or_outline_width(NonnullRefPtr<StyleValue const> const& absolutized_value, double device_pixels_per_css_pixel);
    static NonnullRefPtr<StyleValue const> compute_corner_shape(NonnullRefPtr<StyleValue const> const& absolutized_value);
    static NonnullRefPtr<StyleValue const> compute_font_feature_tag_value_list(NonnullRefPtr<StyleValue const> const& absolutized_value, ComputationContext const&);
    static NonnullRefPtr<StyleValue const> compute_math_depth(NonnullRefPtr<StyleValue const> const& absolutized_value, Optional<DOM::AbstractElement> const& inheritance_parent);
    static NonnullRefPtr<StyleValue const> compute_font_size(NonnullRefPtr<StyleValue const> const& absolutized_value, int computed_math_depth, Optional<DOM::AbstractElement> const& inheritance_parent);
    static NonnullRefPtr<StyleValue const> compute_font_style(NonnullRefPtr<StyleValue const> const& absolutized_value);
    static NonnullRefPtr<StyleValue const> compute_font_weight(NonnullRefPtr<StyleValue const> const& absolutized_value, Optional<DOM::AbstractElement> const& inheritance_parent);
    static NonnullRefPtr<StyleValue const> compute_font_width(NonnullRefPtr<StyleValue const> const& absolutized_value);
    static NonnullRefPtr<StyleValue const> compute_font_variation_settings(NonnullRefPtr<StyleValue const> const& absolutized_value, ComputationContext const&);
    static NonnullRefPtr<StyleValue const> compute_line_height(NonnullRefPtr<StyleValue const> const& absolutized_value, CSSPixels computed_font_size);
    static NonnullRefPtr<StyleValue const> compute_opacity(NonnullRefPtr<StyleValue const> const& absolutized_value);
    static NonnullRefPtr<StyleValue const> compute_position_area(NonnullRefPtr<StyleValue const> const& absolutized_value);

private:
    virtual void visit_edges(Visitor&) override;

    enum class ComputeStyleMode {
        Normal,
        CreatePseudoElementStyleIfNeeded,
    };

    struct LayerMatchingRules {
        FlyString qualified_layer_name;
        Vector<MatchingRule const*> rules;
    };

    struct MatchingRuleSet {
        Vector<MatchingRule const*> user_agent_rules;
        Vector<MatchingRule const*> user_rules;
        Vector<LayerMatchingRules> author_rules;
    };

    [[nodiscard]] MatchingRuleSet build_matching_rule_set(DOM::AbstractElement, PseudoClassBitmap& attempted_pseudo_class_matches, bool& did_match_any_pseudo_element_rules, ComputeStyleMode, StyleScope const&) const;

    LogicalAliasMappingContext compute_logical_alias_mapping_context(DOM::AbstractElement, ComputeStyleMode, MatchingRuleSet const&) const;
    [[nodiscard]] GC::Ptr<ComputedProperties> compute_style_impl(DOM::AbstractElement, ComputeStyleMode, Optional<bool&> did_change_custom_properties, StyleScope const&) const;
    [[nodiscard]] GC::Ref<CascadedProperties> compute_cascaded_values(DOM::AbstractElement, bool did_match_any_pseudo_element_rules, ComputeStyleMode, MatchingRuleSet const&, Optional<LogicalAliasMappingContext>, ReadonlySpan<PropertyID> properties_to_cascade) const;
    void compute_custom_properties(ComputedProperties&, DOM::AbstractElement) const;
    void start_needed_transitions(ComputedProperties const& old_style, ComputedProperties& new_style, DOM::AbstractElement) const;
    void resolve_effective_overflow_values(ComputedProperties&) const;
    void transform_box_type_if_needed(ComputedProperties&, DOM::AbstractElement) const;

    [[nodiscard]] CSSPixelRect viewport_rect() const { return m_viewport_rect; }

    [[nodiscard]] Length::FontMetrics calculate_root_element_font_metrics(ComputedProperties const&) const;

    void cascade_declarations(
        CascadedProperties&,
        DOM::AbstractElement,
        Vector<MatchingRule const*> const&,
        CascadeOrigin,
        Important,
        Optional<FlyString> layer_name,
        Optional<LogicalAliasMappingContext>,
        ReadonlySpan<PropertyID> properties_to_cascade) const;

    GC::Ref<DOM::Document> m_document;

    [[nodiscard]] RuleCache const* rule_cache_for_cascade_origin(CascadeOrigin, Optional<FlyString const> qualified_layer_name, GC::Ptr<DOM::ShadowRoot const>) const;

    Length::FontMetrics m_default_font_metrics;
    mutable Length::FontMetrics m_root_element_font_metrics;

    mutable Optional<ComputationContext> m_cached_font_computation_context;
    mutable Optional<ComputationContext> m_cached_line_height_computation_context;
    mutable Optional<ComputationContext> m_cached_generic_computation_context;
    ComputationContext const& get_computation_context_for_property(PropertyID, ComputedProperties const&, Optional<DOM::AbstractElement>) const;
    void clear_computation_context_caches() const
    {
        const_cast<StyleComputer*>(this)->m_cached_font_computation_context = {};
        const_cast<StyleComputer*>(this)->m_cached_line_height_computation_context = {};
        const_cast<StyleComputer*>(this)->m_cached_generic_computation_context = {};
    }

    CSSPixelRect m_viewport_rect;

    OwnPtr<CountingBloomFilter<u8, 14>> m_ancestor_filter;
    OwnPtr<SelectorEngine::HasResultCache> m_has_result_cache;
};

inline bool StyleComputer::should_reject_with_ancestor_filter(Selector const& selector) const
{
    for (u32 hash : selector.ancestor_hashes()) {
        if (hash == 0)
            break;
        if (!m_ancestor_filter->may_contain(hash))
            return true;
    }
    return false;
}

}
