/*
 * Copyright (c) 2018-2024, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
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
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleProperties.h>
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

// https://www.w3.org/TR/css-cascade/#origin
enum class CascadeOrigin : u8 {
    Author,
    User,
    UserAgent,
    Animation,
    Transition,
};

struct MatchingRule {
    JS::GCPtr<DOM::ShadowRoot const> shadow_root;
    JS::GCPtr<CSSStyleRule const> rule;
    JS::GCPtr<CSSStyleSheet const> sheet;
    size_t style_sheet_index { 0 };
    size_t rule_index { 0 };
    size_t selector_index { 0 };

    u32 specificity { 0 };
    CascadeOrigin cascade_origin;
    bool contains_pseudo_element { false };
    bool contains_root_pseudo_class { false };
    bool can_use_fast_matches { false };
};

struct FontFaceKey {
    FlyString family_name;
    int weight { 0 };
    int slope { 0 };

    [[nodiscard]] u32 hash() const { return pair_int_hash(family_name.hash(), pair_int_hash(weight, slope)); }
    [[nodiscard]] bool operator==(FontFaceKey const&) const = default;
};

class FontLoader;

class StyleComputer {
public:
    enum class AllowUnresolved {
        Yes,
        No,
    };
    static void for_each_property_expanding_shorthands(PropertyID, StyleValue const&, AllowUnresolved, Function<void(PropertyID, StyleValue const&)> const& set_longhand_property);
    static void set_property_expanding_shorthands(StyleProperties&, PropertyID, StyleValue const&, CSS::CSSStyleDeclaration const*, StyleProperties::PropertyValues const& properties_for_revert, StyleProperties::Important = StyleProperties::Important::No);
    static NonnullRefPtr<StyleValue const> get_inherit_value(JS::Realm& initial_value_context_realm, CSS::PropertyID, DOM::Element const*, Optional<CSS::Selector::PseudoElement::Type> = {});

    explicit StyleComputer(DOM::Document&);
    ~StyleComputer();

    DOM::Document& document() { return m_document; }
    DOM::Document const& document() const { return m_document; }

    void reset_ancestor_filter();
    void push_ancestor(DOM::Element const&);
    void pop_ancestor(DOM::Element const&);

    NonnullRefPtr<StyleProperties> create_document_style() const;

    NonnullRefPtr<StyleProperties> compute_style(DOM::Element&, Optional<CSS::Selector::PseudoElement::Type> = {}) const;
    RefPtr<StyleProperties> compute_pseudo_element_style_if_needed(DOM::Element&, Optional<CSS::Selector::PseudoElement::Type>) const;

    Vector<MatchingRule> collect_matching_rules(DOM::Element const&, CascadeOrigin, Optional<CSS::Selector::PseudoElement::Type>) const;

    void invalidate_rule_cache();

    Gfx::Font const& initial_font() const;

    void did_load_font(FlyString const& family_name);

    Optional<FontLoader&> load_font_face(ParsedFontFace const&, ESCAPING Function<void(FontLoader const&)> on_load = {}, ESCAPING Function<void()> on_fail = {});

    void load_fonts_from_sheet(CSSStyleSheet const&);

    RefPtr<Gfx::FontCascadeList const> compute_font_for_style_values(DOM::Element const* element, Optional<CSS::Selector::PseudoElement::Type> pseudo_element, StyleValue const& font_family, StyleValue const& font_size, StyleValue const& font_style, StyleValue const& font_weight, StyleValue const& font_stretch, int math_depth = 0) const;

    void set_viewport_rect(Badge<DOM::Document>, CSSPixelRect const& viewport_rect) { m_viewport_rect = viewport_rect; }

    enum class AnimationRefresh {
        No,
        Yes,
    };
    void collect_animation_into(DOM::Element&, Optional<CSS::Selector::PseudoElement::Type>, JS::NonnullGCPtr<Animations::KeyframeEffect> animation, StyleProperties& style_properties, AnimationRefresh = AnimationRefresh::No) const;

private:
    enum class ComputeStyleMode {
        Normal,
        CreatePseudoElementStyleIfNeeded,
    };

    struct MatchingFontCandidate;

    [[nodiscard]] bool should_reject_with_ancestor_filter(Selector const&) const;

    RefPtr<StyleProperties> compute_style_impl(DOM::Element&, Optional<CSS::Selector::PseudoElement::Type>, ComputeStyleMode) const;
    void compute_cascaded_values(StyleProperties&, DOM::Element&, Optional<CSS::Selector::PseudoElement::Type>, bool& did_match_any_pseudo_element_rules, ComputeStyleMode) const;
    static RefPtr<Gfx::FontCascadeList const> find_matching_font_weight_ascending(Vector<MatchingFontCandidate> const& candidates, int target_weight, float font_size_in_pt, bool inclusive);
    static RefPtr<Gfx::FontCascadeList const> find_matching_font_weight_descending(Vector<MatchingFontCandidate> const& candidates, int target_weight, float font_size_in_pt, bool inclusive);
    RefPtr<Gfx::FontCascadeList const> font_matching_algorithm(FontFaceKey const& key, float font_size_in_pt) const;
    void compute_font(StyleProperties&, DOM::Element const*, Optional<CSS::Selector::PseudoElement::Type>) const;
    void compute_math_depth(StyleProperties&, DOM::Element const*, Optional<CSS::Selector::PseudoElement::Type>) const;
    void compute_defaulted_values(StyleProperties&, DOM::Element const*, Optional<CSS::Selector::PseudoElement::Type>) const;
    void absolutize_values(StyleProperties&, DOM::Element const*) const;
    void resolve_effective_overflow_values(StyleProperties&) const;
    void transform_box_type_if_needed(StyleProperties&, DOM::Element const&, Optional<CSS::Selector::PseudoElement::Type>) const;

    void compute_defaulted_property_value(StyleProperties&, DOM::Element const*, CSS::PropertyID, Optional<CSS::Selector::PseudoElement::Type>) const;

    void set_all_properties(DOM::Element&, Optional<CSS::Selector::PseudoElement::Type>, StyleProperties&, StyleValue const&, DOM::Document&, CSS::CSSStyleDeclaration const*, StyleProperties::PropertyValues const& properties_for_revert, StyleProperties::Important = StyleProperties::Important::No) const;

    template<typename Callback>
    void for_each_stylesheet(CascadeOrigin, Callback) const;

    [[nodiscard]] CSSPixelRect viewport_rect() const { return m_viewport_rect; }

    [[nodiscard]] Length::FontMetrics calculate_root_element_font_metrics(StyleProperties const&) const;

    struct MatchingRuleSet {
        Vector<MatchingRule> user_agent_rules;
        Vector<MatchingRule> user_rules;
        Vector<MatchingRule> author_rules;
    };

    void cascade_declarations(StyleProperties&, DOM::Element&, Optional<CSS::Selector::PseudoElement::Type>, Vector<MatchingRule> const&, CascadeOrigin, Important) const;

    void build_rule_cache();
    void build_rule_cache_if_needed() const;

    JS::NonnullGCPtr<DOM::Document> m_document;

    struct RuleCache {
        HashMap<FlyString, Vector<MatchingRule>> rules_by_id;
        HashMap<FlyString, Vector<MatchingRule>> rules_by_class;
        HashMap<FlyString, Vector<MatchingRule>> rules_by_tag_name;
        HashMap<FlyString, Vector<MatchingRule>, AK::ASCIICaseInsensitiveFlyStringTraits> rules_by_attribute_name;
        Vector<MatchingRule> pseudo_element_rules;
        Vector<MatchingRule> root_rules;
        Vector<MatchingRule> other_rules;

        HashMap<FlyString, NonnullRefPtr<Animations::KeyframeEffect::KeyFrameSet>> rules_by_animation_keyframes;
    };

    NonnullOwnPtr<RuleCache> make_rule_cache_for_cascade_origin(CascadeOrigin);

    RuleCache const& rule_cache_for_cascade_origin(CascadeOrigin) const;

    OwnPtr<RuleCache> m_author_rule_cache;
    OwnPtr<RuleCache> m_user_rule_cache;
    OwnPtr<RuleCache> m_user_agent_rule_cache;
    JS::Handle<CSSStyleSheet> m_user_style_sheet;

    using FontLoaderList = Vector<NonnullOwnPtr<FontLoader>>;
    HashMap<FontFaceKey, FontLoaderList> m_loaded_fonts;

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

    virtual void resource_did_load() override;
    virtual void resource_did_fail() override;

    RefPtr<Gfx::Font> font_with_point_size(float point_size);
    void start_loading_next_url();

private:
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
