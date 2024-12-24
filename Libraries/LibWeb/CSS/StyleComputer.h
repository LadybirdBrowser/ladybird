/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <LibGfx/Font/Typeface.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/CSS/CSSFontFaceRule.h>
#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSStyleDeclaration.h>
#include <LibWeb/CSS/CascadeOrigin.h>
#include <LibWeb/CSS/CascadedProperties.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Loader/ResourceLoader.h>

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

struct MatchingRule {
    GC::Ptr<DOM::ShadowRoot const> shadow_root;
    GC::Ptr<CSSRule const> rule; // Either CSSStyleRule or CSSNestedDeclarations
    GC::Ptr<CSSStyleSheet const> sheet;
    size_t style_sheet_index { 0 };
    size_t rule_index { 0 };
    size_t selector_index { 0 };

    u32 specificity { 0 };
    CascadeOrigin cascade_origin;
    bool contains_pseudo_element { false };
    bool can_use_fast_matches { false };
    bool must_be_hovered { false };
    bool skip { false };

    // Helpers to deal with the fact that `rule` might be a CSSStyleRule or a CSSNestedDeclarations
    PropertyOwningCSSStyleDeclaration const& declaration() const;
    SelectorList const& absolutized_selectors() const;
    FlyString const& qualified_layer_name() const;
};

struct FontFaceKey;

struct OwnFontFaceKey {
    explicit OwnFontFaceKey(FontFaceKey const& other);

    operator FontFaceKey() const;

    [[nodiscard]] u32 hash() const { return pair_int_hash(family_name.hash(), pair_int_hash(weight, slope)); }
    [[nodiscard]] bool operator==(OwnFontFaceKey const& other) const = default;
    [[nodiscard]] bool operator==(FontFaceKey const& other) const;

    FlyString family_name;
    int weight { 0 };
    int slope { 0 };
};

class FontLoader;

class StyleComputer {
public:
    enum class AllowUnresolved {
        Yes,
        No,
    };
    static void for_each_property_expanding_shorthands(PropertyID, CSSStyleValue const&, AllowUnresolved, Function<void(PropertyID, CSSStyleValue const&)> const& set_longhand_property);
    static void set_property_expanding_shorthands(
        CascadedProperties&,
        PropertyID,
        CSSStyleValue const&,
        GC::Ptr<CSSStyleDeclaration const>,
        CascadeOrigin,
        Important,
        Optional<FlyString> layer_name);
    static NonnullRefPtr<CSSStyleValue const> get_inherit_value(CSS::PropertyID, DOM::Element const*, Optional<CSS::Selector::PseudoElement::Type> = {});

    static Optional<String> user_agent_style_sheet_source(StringView name);

    explicit StyleComputer(DOM::Document&);
    ~StyleComputer();

    DOM::Document& document() { return m_document; }
    DOM::Document const& document() const { return m_document; }

    void reset_ancestor_filter();
    void push_ancestor(DOM::Element const&);
    void pop_ancestor(DOM::Element const&);

    [[nodiscard]] GC::Ref<ComputedProperties> create_document_style() const;

    [[nodiscard]] GC::Ref<ComputedProperties> compute_style(DOM::Element&, Optional<CSS::Selector::PseudoElement::Type> = {}) const;
    [[nodiscard]] GC::Ptr<ComputedProperties> compute_pseudo_element_style_if_needed(DOM::Element&, Optional<CSS::Selector::PseudoElement::Type>) const;

    Vector<MatchingRule> collect_matching_rules(DOM::Element const&, CascadeOrigin, Optional<CSS::Selector::PseudoElement::Type>, FlyString const& qualified_layer_name = {}) const;

    void invalidate_rule_cache();

    Gfx::Font const& initial_font() const;

    void did_load_font(FlyString const& family_name);

    Optional<FontLoader&> load_font_face(ParsedFontFace const&, ESCAPING Function<void(FontLoader const&)> on_load = {}, ESCAPING Function<void()> on_fail = {});

    void load_fonts_from_sheet(CSSStyleSheet&);
    void unload_fonts_from_sheet(CSSStyleSheet&);

    RefPtr<Gfx::FontCascadeList const> compute_font_for_style_values(DOM::Element const* element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element, CSSStyleValue const& font_family, CSSStyleValue const& font_size, CSSStyleValue const& font_style, CSSStyleValue const& font_weight, CSSStyleValue const& font_stretch, int math_depth = 0) const;

    void set_viewport_rect(Badge<DOM::Document>, CSSPixelRect const& viewport_rect) { m_viewport_rect = viewport_rect; }

    enum class AnimationRefresh {
        No,
        Yes,
    };
    void collect_animation_into(DOM::Element&, Optional<CSS::Selector::PseudoElement::Type>, GC::Ref<Animations::KeyframeEffect> animation, ComputedProperties&, AnimationRefresh = AnimationRefresh::No) const;

    [[nodiscard]] bool has_has_selectors() const;
    [[nodiscard]] bool has_defined_selectors() const;
    [[nodiscard]] bool has_attribute_selector(FlyString const& attribute_name) const;

    size_t number_of_css_font_faces_with_loading_in_progress() const;

    [[nodiscard]] GC::Ref<ComputedProperties> compute_properties(DOM::Element&, Optional<Selector::PseudoElement::Type>, CascadedProperties&) const;

    void absolutize_values(ComputedProperties&) const;
    void compute_font(ComputedProperties&, DOM::Element const*, Optional<CSS::Selector::PseudoElement::Type>) const;

private:
    enum class ComputeStyleMode {
        Normal,
        CreatePseudoElementStyleIfNeeded,
    };

    struct MatchingFontCandidate;

    [[nodiscard]] bool should_reject_with_ancestor_filter(Selector const&) const;

    [[nodiscard]] GC::Ptr<ComputedProperties> compute_style_impl(DOM::Element&, Optional<CSS::Selector::PseudoElement::Type>, ComputeStyleMode) const;
    [[nodiscard]] GC::Ref<CascadedProperties> compute_cascaded_values(DOM::Element&, Optional<CSS::Selector::PseudoElement::Type>, bool& did_match_any_pseudo_element_rules, ComputeStyleMode) const;
    static RefPtr<Gfx::FontCascadeList const> find_matching_font_weight_ascending(Vector<MatchingFontCandidate> const& candidates, int target_weight, float font_size_in_pt, bool inclusive);
    static RefPtr<Gfx::FontCascadeList const> find_matching_font_weight_descending(Vector<MatchingFontCandidate> const& candidates, int target_weight, float font_size_in_pt, bool inclusive);
    RefPtr<Gfx::FontCascadeList const> font_matching_algorithm(FlyString const& family_name, int weight, int slope, float font_size_in_pt) const;
    void compute_math_depth(ComputedProperties&, DOM::Element const*, Optional<CSS::Selector::PseudoElement::Type>) const;
    void compute_defaulted_values(ComputedProperties&, DOM::Element const*, Optional<CSS::Selector::PseudoElement::Type>) const;
    void start_needed_transitions(ComputedProperties const& old_style, ComputedProperties& new_style, DOM::Element&, Optional<Selector::PseudoElement::Type>) const;
    void resolve_effective_overflow_values(ComputedProperties&) const;
    void transform_box_type_if_needed(ComputedProperties&, DOM::Element const&, Optional<CSS::Selector::PseudoElement::Type>) const;

    void compute_defaulted_property_value(ComputedProperties&, DOM::Element const*, CSS::PropertyID, Optional<CSS::Selector::PseudoElement::Type>) const;

    void set_all_properties(
        CascadedProperties&,
        DOM::Element&,
        Optional<Selector::PseudoElement::Type>,
        CSSStyleValue const&,
        DOM::Document&,
        GC::Ptr<CSSStyleDeclaration const>,
        CascadeOrigin,
        Important,
        Optional<FlyString> layer_name) const;

    template<typename Callback>
    void for_each_stylesheet(CascadeOrigin, Callback) const;

    [[nodiscard]] CSSPixelRect viewport_rect() const { return m_viewport_rect; }

    [[nodiscard]] Length::FontMetrics calculate_root_element_font_metrics(ComputedProperties const&) const;

    Vector<FlyString> m_qualified_layer_names_in_order;
    void build_qualified_layer_names_cache();

    struct LayerMatchingRules {
        FlyString qualified_layer_name;
        Vector<MatchingRule> rules;
    };

    struct MatchingRuleSet {
        Vector<MatchingRule> user_agent_rules;
        Vector<MatchingRule> user_rules;
        Vector<LayerMatchingRules> author_rules;
    };

    void cascade_declarations(
        CascadedProperties&,
        DOM::Element&,
        Optional<CSS::Selector::PseudoElement::Type>,
        Vector<MatchingRule> const&,
        CascadeOrigin,
        Important,
        Optional<FlyString> layer_name) const;

    void build_rule_cache();
    void build_rule_cache_if_needed() const;

    GC::Ref<DOM::Document> m_document;

    struct SelectorInsights {
        bool has_has_selectors { false };
        bool has_defined_selectors { false };
        HashTable<FlyString> all_names_used_in_attribute_selectors;
    };

    struct RuleCache {
        HashMap<FlyString, Vector<MatchingRule>> rules_by_id;
        HashMap<FlyString, Vector<MatchingRule>> rules_by_class;
        HashMap<FlyString, Vector<MatchingRule>> rules_by_tag_name;
        HashMap<FlyString, Vector<MatchingRule>, AK::ASCIICaseInsensitiveFlyStringTraits> rules_by_attribute_name;
        Array<Vector<MatchingRule>, to_underlying(CSS::Selector::PseudoElement::Type::KnownPseudoElementCount)> rules_by_pseudo_element;
        Vector<MatchingRule> root_rules;
        Vector<MatchingRule> other_rules;

        HashMap<FlyString, NonnullRefPtr<Animations::KeyframeEffect::KeyFrameSet>> rules_by_animation_keyframes;
    };

    NonnullOwnPtr<RuleCache> make_rule_cache_for_cascade_origin(CascadeOrigin, SelectorInsights&);

    RuleCache const& rule_cache_for_cascade_origin(CascadeOrigin) const;

    static void collect_selector_insights(Selector const&, SelectorInsights&);

    OwnPtr<SelectorInsights> m_selector_insights;
    OwnPtr<RuleCache> m_author_rule_cache;
    OwnPtr<RuleCache> m_user_rule_cache;
    OwnPtr<RuleCache> m_user_agent_rule_cache;
    GC::Root<CSSStyleSheet> m_user_style_sheet;

    using FontLoaderList = Vector<NonnullOwnPtr<FontLoader>>;
    HashMap<OwnFontFaceKey, FontLoaderList> m_loaded_fonts;

    Length::FontMetrics m_default_font_metrics;
    Length::FontMetrics m_root_element_font_metrics;

    CSSPixelRect m_viewport_rect;

    CountingBloomFilter<u8, 14> m_ancestor_filter;
};

class FontLoader : public ResourceClient {
public:
    FontLoader(StyleComputer& style_computer, FlyString family_name, Vector<Gfx::UnicodeRange> unicode_ranges, Vector<URL::URL> urls, ESCAPING Function<void(FontLoader const&)> on_load = {}, ESCAPING Function<void()> on_fail = {});

    virtual ~FontLoader() override;

    Vector<Gfx::UnicodeRange> const& unicode_ranges() const { return m_unicode_ranges; }
    RefPtr<Gfx::Typeface> vector_font() const { return m_vector_font; }

    RefPtr<Gfx::Font> font_with_point_size(float point_size);
    void start_loading_next_url();

    bool is_loading() const { return resource() && resource()->is_pending(); }

private:
    // ^ResourceClient
    virtual void resource_did_load() override;
    virtual void resource_did_fail() override;

    void resource_did_load_or_fail();

    ErrorOr<NonnullRefPtr<Gfx::Typeface>> try_load_font();

    StyleComputer& m_style_computer;
    FlyString m_family_name;
    Vector<Gfx::UnicodeRange> m_unicode_ranges;
    RefPtr<Gfx::Typeface> m_vector_font;
    Vector<URL::URL> m_urls;
    Function<void(FontLoader const&)> m_on_load;
    Function<void()> m_on_fail;
};

}
