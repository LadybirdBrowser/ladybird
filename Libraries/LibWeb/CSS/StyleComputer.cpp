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
#include <AK/JsonObject.h>
#include <AK/Math.h>
#include <AK/NeverDestroyed.h>
#include <AK/NonnullRawPtr.h>
#include <AK/QuickSort.h>
#include <AK/Utf8View.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/CSS/AncestorFilter.h>
#include <LibWeb/CSS/AnimationEvent.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSContainerRule.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/CSS/CSSLayerStatementRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSScopeRule.h>
#include <LibWeb/CSS/CSSStyleProperties.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/CSS/CascadedProperties.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/FontComputer.h>
#include <LibWeb/CSS/HypotheticalElement.h>
#include <LibWeb/CSS/Interpolation.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/CSS/Parser/ArbitrarySubstitutionFunctions.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/SyntaxParsing.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/StyleComputeFFI.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleProperty.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheet.h>
#include <LibWeb/CSS/StyleSheetIdentifier.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/GuaranteedInvalidStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PendingSubstitutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/SuperellipseStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <math.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(StyleComputer);

static void for_each_element_hash(DOM::Element const& element, auto callback)
{
    callback(ancestor_filter_hash_for_tag_name(element.local_name().ascii_case_insensitive_hash()));
    if (element.id().has_value())
        callback(ancestor_filter_hash_for_id(element.id()->hash()));
    for (auto const& class_ : element.class_names())
        callback(ancestor_filter_hash_for_class(class_.hash()));
    element.for_each_attribute([&](auto& attribute) {
        callback(ancestor_filter_hash_for_attribute(attribute.name().ascii_case_insensitive_hash()));
    });
}

static bool property_affects_font_metrics(PropertyID property_id)
{
    return property_id == PropertyID::FontSize || property_id == PropertyID::LineHeight;
}

CSSStyleProperties const& MatchingRule::declaration() const
{
    if (rule->type() == CSSRule::Type::Style)
        return static_cast<CSSStyleRule const&>(*rule).declaration();
    if (rule->type() == CSSRule::Type::NestedDeclarations)
        return static_cast<CSSNestedDeclarations const&>(*rule).declaration();
    VERIFY_NOT_REACHED();
}

SelectorList const& MatchingRule::absolutized_selectors() const
{
    if (rule->type() == CSSRule::Type::Style)
        return static_cast<CSSStyleRule const&>(*rule).absolutized_selectors();
    if (rule->type() == CSSRule::Type::NestedDeclarations)
        return static_cast<CSSNestedDeclarations const&>(*rule).absolutized_selectors();
    VERIFY_NOT_REACHED();
}

Utf16FlyString const& MatchingRule::qualified_layer_name() const
{
    if (rule->type() == CSSRule::Type::Style)
        return static_cast<CSSStyleRule const&>(*rule).qualified_layer_name();
    if (rule->type() == CSSRule::Type::NestedDeclarations)
        return static_cast<CSSNestedDeclarations const&>(*rule).qualified_layer_name();
    VERIFY_NOT_REACHED();
}

StyleComputer::StyleComputer(DOM::Document& document)
    : m_document(document)
    , m_default_font_metrics(16, Platform::FontPlugin::the().default_font(16)->pixel_metrics(), InitialValues::line_height())
    , m_root_element_font_metrics(m_default_font_metrics)
{
    m_ancestor_filter = make<CountingBloomFilter<u8, 14>>();
}

StyleComputer::~StyleComputer() = default;

void StyleComputer::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    if (m_has_result_cache)
        visitor.visit(*m_has_result_cache);
    if (m_has_fast_reject_filter_cache)
        visitor.visit(*m_has_fast_reject_filter_cache);

    if (m_cached_font_computation_context.has_value())
        m_cached_font_computation_context->visit_edges(visitor);
    if (m_cached_line_height_computation_context.has_value())
        m_cached_line_height_computation_context->visit_edges(visitor);
    if (m_cached_generic_computation_context.has_value())
        m_cached_generic_computation_context->visit_edges(visitor);

    for (auto& rule : m_rules_to_run_scratch)
        rule.visit_edges(visitor);
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

RuleCache const* StyleComputer::rule_cache_for_cascade_origin(CascadeOrigin cascade_origin, Optional<Utf16FlyString const> qualified_layer_name, GC::Ptr<DOM::ShadowRoot const> shadow_root) const
{
    auto& style_scope = shadow_root ? shadow_root->style_scope() : document().style_scope();
    auto const& rule_cache = style_scope.rule_cache();

    auto const* rule_caches_by_layer = [&]() -> RuleCaches const* {
        switch (cascade_origin) {
        case CascadeOrigin::Author:
            return &rule_cache.author_rule_cache;
        case CascadeOrigin::User:
            return &rule_cache.user_rule_cache;
        case CascadeOrigin::UserAgent:
            return &rule_cache.user_agent_rule_cache;
        default:
            VERIFY_NOT_REACHED();
        }
    }();
    if (!rule_caches_by_layer)
        return nullptr;
    if (!qualified_layer_name.has_value())
        return &rule_caches_by_layer->main;
    return rule_caches_by_layer->by_layer.get(*qualified_layer_name).value_or(nullptr);
}

[[nodiscard]] static bool filter_namespace_rule(Optional<Utf16FlyString> const& element_namespace_uri, MatchingRule const& rule)
{
    if (!rule.element_namespace_filter.has_value())
        return true;

    if (rule.element_namespace_filter->is_empty())
        return !element_namespace_uri.has_value() || element_namespace_uri->is_empty();

    return element_namespace_uri.has_value()
        && element_namespace_uri->view() == rule.element_namespace_filter->view();
}

NonnullRefPtr<InvalidationPlan> StyleComputer::invalidation_plan_for_properties(Vector<InvalidationSet::Property> const& properties, StyleScope const& style_scope) const
{
    auto result = InvalidationPlan::create();
    if (properties.is_empty())
        return result;

    auto const& invalidation_plans = style_scope.style_invalidation_data().invalidation_plans();
    for (auto const& property : properties) {
        if (auto it = invalidation_plans.find(property); it != invalidation_plans.end()) {
            result->include_all_from(*it->value);
            if (result->invalidate_whole_subtree)
                break;
        }
    }
    return result;
}

Vector<HasInvalidationMetadata> const* StyleComputer::has_invalidation_metadata_for_property(InvalidationSet::Property const& property, StyleScope const& style_scope) const
{
    auto const& style_invalidation_data = style_scope.style_invalidation_data();

    auto return_bucket_if_present = [](auto const& map, auto const& key) -> Vector<HasInvalidationMetadata> const* {
        auto bucket = map.get(key);
        if (!bucket.has_value())
            return nullptr;
        return &bucket.value();
    };

    switch (property.type) {
    case InvalidationSet::Property::Type::Id:
        return return_bucket_if_present(style_invalidation_data.ids_used_in_has_selectors, property.id());
    case InvalidationSet::Property::Type::Class:
        return return_bucket_if_present(style_invalidation_data.class_names_used_in_has_selectors, property.class_name());
    case InvalidationSet::Property::Type::Attribute:
        return return_bucket_if_present(style_invalidation_data.attribute_names_used_in_has_selectors, property.name());
    case InvalidationSet::Property::Type::TagName:
        return return_bucket_if_present(style_invalidation_data.tag_names_used_in_has_selectors, property.name());
    case InvalidationSet::Property::Type::PseudoClass:
        return return_bucket_if_present(style_invalidation_data.pseudo_classes_used_in_has_selectors, property.value.get<PseudoClass>());
    default:
        break;
    }
    return nullptr;
}

static bool scope_selector_matches(Selector const& selector, DOM::Element const& element, DOM::Element const& subject, CSSStyleSheet const& scope_style_sheet, GC::Ptr<DOM::Element const> shadow_host, GC::Ptr<DOM::ShadowRoot const> rule_root, GC::Ptr<DOM::ParentNode const> scope)
{
    // A scope boundary match can activate or deactivate rules for descendants of the scope root.
    if (&element == &subject && selector.contains_pseudo_class(PseudoClass::Has))
        const_cast<DOM::Element&>(element).set_affected_by_has_pseudo_class_in_non_subject_position();

    SelectorMatching::MatchContext context {
        .style_sheet_for_rule = scope_style_sheet,
        .subject = subject,
        .rule_shadow_root = rule_root,
        .collect_per_element_selector_involvement_metadata = true,
    };
    return SelectorMatching::matches(selector, DOM::AbstractElement(element), shadow_host, context, scope);
}

struct ResolvedScope {
    GC::Ptr<DOM::Element const> root;
    size_t proximity { NumericLimits<size_t>::max() };
};

// https://drafts.csswg.org/css-cascade-6/#scope-limits
static bool subject_matches_scope_limit(
    DOM::AbstractElement abstract_element,
    GC::Ptr<DOM::Element const> shadow_host,
    GC::Ptr<DOM::ShadowRoot const> rule_root,
    CSSRule const& scope_rule,
    CSSStyleSheet const& owner_style_sheet,
    DOM::Element const& root)
{
    // Finding any scoping limits
    // For each scope created by a scoping root, its scoping limits are set to all elements that are descendants of
    // the scoping root and that match <scope-end>, interpreting :scope and & exactly as in scoped style rules.
    auto selectors = scope_end_selectors_for_matching(scope_rule);
    if (!selectors.has_value())
        return false;

    for (auto const* candidate = &abstract_element.element(); candidate; candidate = candidate->parent_or_shadow_host_element()) {
        for (auto const& selector : *selectors) {
            if (scope_selector_matches(selector, *candidate, abstract_element.element(), owner_style_sheet, shadow_host, rule_root, root))
                return true;
        }
        if (candidate == &root)
            break;
    }

    return false;
}

static Optional<ResolvedScope> resolve_single_scope(DOM::AbstractElement abstract_element, GC::Ptr<DOM::Element const> shadow_host, GC::Ptr<DOM::ShadowRoot const> rule_root, CSSRule const& scope_rule, GC::Ptr<DOM::Element const> outer_root)
{
    size_t proximity = 0;
    auto const* owner_style_sheet = scope_rule.parent_style_sheet();
    VERIFY(owner_style_sheet);

    // https://drafts.csswg.org/css-cascade-6/#scope-limits
    // Finding the scoping root(s)
    // For each element matched by <scope-start>, create a scope using that element as the scoping root.
    if (scope_start_selectors_for_matching(scope_rule).has_value()) {
        for (auto const* candidate = &abstract_element.element(); candidate; candidate = candidate->parent_or_shadow_host_element(), ++proximity) {
            if (outer_root && !outer_root->is_shadow_including_inclusive_ancestor_of(*candidate))
                break;
            for (auto const& selector : *scope_start_selectors_for_matching(scope_rule)) {
                if (scope_selector_matches(selector, *candidate, abstract_element.element(), *owner_style_sheet, shadow_host, rule_root, outer_root)) {
                    if (!subject_matches_scope_limit(abstract_element, shadow_host, rule_root, scope_rule, *owner_style_sheet, *candidate))
                        return ResolvedScope { candidate, proximity };
                }
            }
        }
        return {};
    }

    GC::Ptr<DOM::Element const> root = [&] -> GC::Ptr<DOM::Element const> {
        // If no <scope-start> is specified, the scoping root is the parent element of the owner node of the
        // stylesheet where the @scope rule is defined.
        if (auto* owner_node = const_cast<CSSStyleSheet&>(*owner_style_sheet).owner_node()) {
            if (auto parent = owner_node->parent_element())
                return parent;
        }

        // If no such element exists and the containing node tree is a shadow tree, then the scoping root is the
        // shadow host.
        if (rule_root)
            return rule_root->host();

        // Otherwise, the scoping root is the root of the containing node tree.
        if (auto document = owner_style_sheet->owning_document())
            return document->document_element();
        return nullptr;
    }();
    if (!root || !root->is_shadow_including_inclusive_ancestor_of(abstract_element.element()))
        return {};
    if (outer_root && !outer_root->is_shadow_including_inclusive_ancestor_of(*root))
        return {};
    for (auto const* candidate = &abstract_element.element(); candidate && candidate != root; candidate = candidate->parent_or_shadow_host_element())
        ++proximity;

    if (!root)
        return {};

    if (subject_matches_scope_limit(abstract_element, shadow_host, rule_root, scope_rule, *owner_style_sheet, *root))
        return {};

    return ResolvedScope { root, proximity };
}

static Optional<ResolvedScope> resolve_scope_chain(DOM::AbstractElement abstract_element, MatchingRule const& rule, GC::Ptr<DOM::Element const> shadow_host, GC::Ptr<DOM::ShadowRoot const> rule_root, CSSRule const& scope_rule)
{
    // https://drafts.csswg.org/css-cascade-6/#cascade-proximity
    // Scope Proximity
    // When comparing declarations that appear in style rules with different scoping roots, then the declaration with
    // the fewest generational or sibling-element hops between the scoping root and the scoped style rule subject wins.
    // For this purpose, style rules without a scoping root are considered to have infinite proximity hops.
    GC::Ptr<DOM::Element const> outer_root;
    if (auto ancestor_scope_rule = nearest_ancestor_scope_rule_for_matching(scope_rule)) {
        auto resolved_ancestor_scope = resolve_scope_chain(abstract_element, rule, shadow_host, rule_root, *ancestor_scope_rule);
        if (!resolved_ancestor_scope.has_value())
            return {};

        outer_root = resolved_ancestor_scope->root;
    }

    return resolve_single_scope(abstract_element, shadow_host, rule_root, scope_rule, outer_root);
}

static Optional<ResolvedScope> resolve_scope(DOM::AbstractElement abstract_element, MatchingRule const& rule, GC::Ptr<DOM::Element const> shadow_host, GC::Ptr<DOM::ShadowRoot const> rule_root)
{
    if (!rule.scope_rule)
        return ResolvedScope {};

    return resolve_scope_chain(abstract_element, rule, shadow_host, rule_root, *rule.scope_rule);
}

static u64 pseudo_element_style_bit(PseudoElement pseudo_element)
{
    VERIFY(to_underlying(pseudo_element) < to_underlying(PseudoElement::KnownPseudoElementCount));
    return 1ull << to_underlying(pseudo_element);
}

struct ParentFilterHashCollector {
    static bool contains_hash(Vector<u32> const& hashes, u32 hash)
    {
        for (auto existing_hash : hashes) {
            if (existing_hash == hash)
                return true;
        }
        return false;
    }

    static void append_unique_hash(Vector<u32>& hashes, u32 hash)
    {
        if (!contains_hash(hashes, hash))
            hashes.append(hash);
    }

    static void intersect_hashes(Vector<u32>& hashes, Vector<u32> const& other_hashes)
    {
        for (size_t i = 0; i < hashes.size();) {
            if (contains_hash(other_hashes, hashes[i])) {
                ++i;
                continue;
            }
            hashes.remove(i);
        }
    }

    static Vector<u32> hashes_from_simple_selector(Selector::SimpleSelector const& simple_selector)
    {
        Vector<u32> hashes;
        switch (simple_selector.type) {
        case Selector::SimpleSelector::Type::Id:
            hashes.append(ancestor_filter_hash_for_id(simple_selector.id_name().hash()));
            break;
        case Selector::SimpleSelector::Type::Class:
            hashes.append(ancestor_filter_hash_for_class(simple_selector.class_name().hash()));
            break;
        case Selector::SimpleSelector::Type::TagName:
            hashes.append(ancestor_filter_hash_for_tag_name(simple_selector.qualified_name().name.lowercase_name.hash()));
            break;
        case Selector::SimpleSelector::Type::Attribute:
            hashes.append(ancestor_filter_hash_for_attribute(simple_selector.attribute().qualified_name.name.lowercase_name.hash()));
            break;
        case Selector::SimpleSelector::Type::PseudoClass: {
            auto const& pseudo_class = simple_selector.pseudo_class();
            if (pseudo_class.type != PseudoClass::Is && pseudo_class.type != PseudoClass::Where)
                break;

            hashes = common_hashes_from_selector_list(pseudo_class.argument_selector_list);
            break;
        }
        default:
            break;
        }
        return hashes;
    }

    static Vector<u32> hashes_from_compound(Selector::CompoundSelector const& compound_selector)
    {
        Vector<u32> hashes;
        for (auto const& simple_selector : compound_selector.simple_selectors) {
            for (auto hash : hashes_from_simple_selector(simple_selector))
                append_unique_hash(hashes, hash);
        }
        return hashes;
    }

    static Vector<u32> hashes_from_selector_subject(Selector const& selector)
    {
        auto const& compound_selectors = selector.compound_selectors();
        if (compound_selectors.is_empty())
            return {};
        return hashes_from_compound(compound_selectors.last());
    }

    static Vector<u32> common_hashes_from_selector_list(SelectorList const& selector_list)
    {
        if (selector_list.is_empty())
            return {};

        Optional<Vector<u32>> common_hashes;
        for (auto const& argument_selector : selector_list) {
            auto hashes = hashes_from_selector_subject(*argument_selector);
            if (!common_hashes.has_value()) {
                common_hashes = move(hashes);
                continue;
            }

            intersect_hashes(common_hashes.value(), hashes);
            if (common_hashes->is_empty())
                break;
        }

        return common_hashes.release_value();
    }
};

static Vector<u32> parent_filter_hashes_for_selector(Selector const& selector)
{
    if (selector.target_pseudo_element().has_value())
        return {};

    auto const& compound_selectors = selector.compound_selectors();
    if (compound_selectors.size() < 2)
        return {};
    if (compound_selectors.last().combinator != Selector::Combinator::ImmediateChild)
        return {};

    // The compound immediately to the left of the subject must match the
    // subject's parent. Only collect hashes that are required on that parent
    // itself; ancestor requirements inside selector-list pseudos remain the
    // job of the normal ancestor filter.
    return ParentFilterHashCollector::hashes_from_compound(compound_selectors[compound_selectors.size() - 2]);
}

static bool parent_filter_may_contain_all(DOM::Element const& parent, Vector<u32> const& required_hashes)
{
    Vector<u32> parent_hashes;
    for_each_element_hash(parent, [&](u32 hash) {
        ParentFilterHashCollector::append_unique_hash(parent_hashes, hash);
    });

    for (auto hash : required_hashes) {
        if (!ParentFilterHashCollector::contains_hash(parent_hashes, hash))
            return false;
    }
    return true;
}

static bool should_reject_with_parent_filter(DOM::AbstractElement abstract_element, Selector const& selector)
{
    auto required_hashes = parent_filter_hashes_for_selector(selector);
    if (required_hashes.is_empty())
        return false;

    auto parent = abstract_element.parent_element();
    if (!parent)
        return true;

    return !parent_filter_may_contain_all(*parent, required_hashes);
}

Vector<StyleComputer::ScopedMatchingRule> StyleComputer::collect_matching_rules_from_context(DOM::AbstractElement abstract_element, CascadeOrigin cascade_origin, GC::Ptr<DOM::ShadowRoot const> context_shadow_root, Optional<Utf16FlyString const> qualified_layer_name, u64* matching_pseudo_element_styles) const
{
    auto const& root_node = abstract_element.element().root();
    auto shadow_root = as_if<DOM::ShadowRoot>(root_node);
    auto element_shadow_root = abstract_element.element().shadow_root();
    auto const& element_namespace_uri = abstract_element.element().namespace_uri();

    GC::Ptr<DOM::Element const> shadow_host;
    if (element_shadow_root)
        shadow_host = abstract_element.element();
    else if (shadow_root)
        shadow_host = shadow_root->host();

    auto& rules_to_run = m_rules_to_run_scratch;
    VERIFY(rules_to_run.is_empty());
    ScopeGuard clear_rules_to_run = [&] {
        rules_to_run.clear_with_capacity();
    };

    // Multi-bucketed pseudo-element rules can be reached through more than one
    // originating-element key, e.g. `:is(.foo, .bar)::before` on an element with
    // both classes. Use a generation stamp instead of a per-element HashSet so
    // duplicate suppression stays an indexed load/store in the hot path.
    u64 multi_bucket_rule_generation = 0;
    auto next_multi_bucket_rule_generation = [&]() {
        ++m_multi_bucket_rule_generation;
        if (m_multi_bucket_rule_generation == 0) {
            for (auto& generation : m_seen_multi_bucket_rule_generations)
                generation = 0;
            ++m_multi_bucket_rule_generation;
        }
        return m_multi_bucket_rule_generation;
    };
    auto was_multi_bucket_rule_seen = [&](MatchingRule const& rule) {
        if (rule.multi_bucket_rule_index == 0)
            return false;

        if (multi_bucket_rule_generation == 0)
            multi_bucket_rule_generation = next_multi_bucket_rule_generation();

        auto const index = static_cast<size_t>(rule.multi_bucket_rule_index - 1);
        if (m_seen_multi_bucket_rule_generations.size() <= index)
            m_seen_multi_bucket_rule_generations.resize(index + 1);
        if (m_seen_multi_bucket_rule_generations[index] == multi_bucket_rule_generation)
            return true;
        m_seen_multi_bucket_rule_generations[index] = multi_bucket_rule_generation;
        return false;
    };

    auto add_rule_to_run = [&](MatchingRule const& rule_to_run, GC::Ptr<DOM::ShadowRoot const> rule_root) {
        // FIXME: This needs to be revised when adding support for the ::shadow selector, as it needs to cross shadow boundaries.
        auto from_user_agent_or_user_stylesheet = rule_to_run.cascade_origin == CascadeOrigin::UserAgent || rule_to_run.cascade_origin == CascadeOrigin::User;

        // NOTE: Inside shadow trees, we only match rules that are defined in the shadow tree's style sheets.
        //       Exceptions are:
        //       - the shadow tree's *shadow host*, which needs to match :host rules from inside the shadow root.
        //       - ::slotted() rules, which need to match elements assigned to slots from inside the shadow root.
        //       - UA or User style sheets don't have a scope, so they are always relevant.
        // FIXME: We should reorganize the data so that the document-level StyleComputer doesn't cache *all* rules,
        //        but instead we'd have some kind of "style scope" at the document level, and also one for each shadow root.
        //        Then we could only evaluate rules from the current style scope.
        bool rule_is_relevant_for_current_scope = rule_root == shadow_root
            || (element_shadow_root && rule_root == element_shadow_root)
            || from_user_agent_or_user_stylesheet
            || rule_to_run.slotted
            || rule_to_run.contains_part_pseudo_element
            || (shadow_root && !rule_root && shadow_root->uses_document_style_sheets());

        if (!rule_is_relevant_for_current_scope)
            return;

        if (rule_to_run.container_rule
            && !rule_to_run.container_rule->contains_size_feature()
            && !rule_to_run.container_rule->contains_style_feature()
            && !rule_to_run.container_rule->matches(abstract_element))
            return;

        auto const& selector = rule_to_run.selector;
        if (selector.can_use_ancestor_filter() && should_reject_with_ancestor_filter(selector))
            return;
        if (should_reject_with_parent_filter(abstract_element, selector))
            return;

        rules_to_run.unchecked_append({
            .rule = &rule_to_run,
            .shadow_root = rule_root,
            .scope_root = nullptr,
            .scope_proximity = NumericLimits<size_t>::max(),
        });
    };

    auto add_rules_to_run = [&](Vector<MatchingRule> const& rules, GC::Ptr<DOM::ShadowRoot const> rule_root) {
        rules_to_run.grow_capacity(rules_to_run.size() + rules.size());
        if (abstract_element.pseudo_element().has_value()) {
            // Only consider rules whose target pseudo-element matches the one being queried. Rules with no target
            // pseudo-element, or with a different target pseudo-element can never match the query and would otherwise
            // waste work evaluating their compound selectors.
            // FIXME: Once exportparts can forward pseudo-elements as parts, a bare ::part(name) rule may need to match
            //        a query for a different pseudo-element type.
            auto queried_pseudo_element = *abstract_element.pseudo_element();
            for (auto const& rule : rules) {
                if (was_multi_bucket_rule_seen(rule))
                    continue;
                auto const& target_pseudo_element = rule.selector.target_pseudo_element();
                if (target_pseudo_element != queried_pseudo_element)
                    continue;
                if (!filter_namespace_rule(element_namespace_uri, rule))
                    continue;
                add_rule_to_run(rule, rule_root);
            }
        } else {
            for (auto const& rule : rules) {
                if (was_multi_bucket_rule_seen(rule))
                    continue;
                if (!filter_namespace_rule(element_namespace_uri, rule))
                    continue;
                if (rule.selector.target_pseudo_element().has_value()) {
                    if (matching_pseudo_element_styles)
                        add_rule_to_run(rule, rule_root);
                    continue;
                }
                if (rule.slotted || rule.contains_part_pseudo_element || !rule.contains_pseudo_element)
                    add_rule_to_run(rule, rule_root);
            }
        }
    };

    auto add_rules_from_cache = [&](RuleCache const& rule_cache, GC::Ptr<DOM::ShadowRoot const> rule_root) {
        multi_bucket_rule_generation = next_multi_bucket_rule_generation();
        Function<bool(u32)> may_contain_ancestor_hash = [&](u32 hash) { return m_ancestor_filter->may_contain(hash); };
        rule_cache.for_each_matching_rules(abstract_element, may_contain_ancestor_hash, [&](auto const& matching_rules) {
            add_rules_to_run(matching_rules, rule_root);
            return IterationDecision::Continue;
        });
        if (!abstract_element.pseudo_element().has_value() && matching_pseudo_element_styles) {
            rule_cache.for_each_matching_pseudo_element_rules(abstract_element, may_contain_ancestor_hash, [&](auto const& matching_rules) {
                add_rules_to_run(matching_rules, rule_root);
                return IterationDecision::Continue;
            });
        }
    };

    if (auto const* rule_cache = rule_cache_for_cascade_origin(cascade_origin, qualified_layer_name, context_shadow_root))
        add_rules_from_cache(*rule_cache, context_shadow_root);

    // Per "find flattened slotables" (https://dom.spec.whatwg.org/#find-flattened-slotables),
    // a <slot> element whose root is a shadow root recurses into its own slottables instead of
    // being appended itself, so ::slotted() must never match such an intermediate re-slotted slot.
    auto const* subject_as_slot = as_if<HTML::HTMLSlotElement>(abstract_element.element());
    bool const subject_is_reslotted_slot = subject_as_slot && subject_as_slot->root().is_shadow_root();

    // Walk up the slot chain for nested slots. An element can be assigned to a slot
    // which is itself assigned to another slot in a parent shadow root. The ::slotted()
    // pseudo-element matches elements assigned "after flattening", so we must collect
    // slotted rules from every shadow root in the chain.
    if (!subject_is_reslotted_slot) {
        for (GC::Ptr<HTML::HTMLSlotElement const> slot = abstract_element.element().assigned_slot_internal(); slot; slot = slot->assigned_slot_internal()) {
            if (auto const* slot_shadow_root = as_if<DOM::ShadowRoot>(slot->root())) {
                if (slot_shadow_root != context_shadow_root)
                    continue;
                if (auto const* rule_cache = rule_cache_for_cascade_origin(cascade_origin, qualified_layer_name, slot_shadow_root)) {
                    add_rules_to_run(rule_cache->slotted_rules, slot_shadow_root);
                }
            }
        }
    }

    // ::part() can apply to anything in a shadow tree, that is either an element with a `part` attribute or a pseudo-element.
    // Rules from any ancestor style scope can apply, including from the element's own shadow root
    // (for :host::part() within the shadow DOM's own stylesheet).
    if (shadow_root && (abstract_element.pseudo_element().has_value() || !abstract_element.element().part_names().is_empty())) {
        if (context_shadow_root == shadow_root) {
            if (auto const* rule_cache = rule_cache_for_cascade_origin(cascade_origin, qualified_layer_name, shadow_root)) {
                add_rules_to_run(rule_cache->part_rules, shadow_root);
            }
        }
        for (auto* part_shadow_root = abstract_element.element().first_flat_tree_ancestor_of_type<DOM::ShadowRoot>();
            part_shadow_root;
            part_shadow_root = part_shadow_root->first_flat_tree_ancestor_of_type<DOM::ShadowRoot>()) {

            if (context_shadow_root != part_shadow_root)
                continue;
            if (auto const* rule_cache = rule_cache_for_cascade_origin(cascade_origin, qualified_layer_name, part_shadow_root)) {
                add_rules_to_run(rule_cache->part_rules, part_shadow_root);
            }
        }
        if (!context_shadow_root) {
            if (auto const* rule_cache = rule_cache_for_cascade_origin(cascade_origin, qualified_layer_name, nullptr)) {
                add_rules_to_run(rule_cache->part_rules, nullptr);
            }
        }
    }

    Vector<ScopedMatchingRule> matching_rules;
    matching_rules.ensure_capacity(rules_to_run.size());

    for (auto rule_to_run : rules_to_run) {
        // NOTE: When matching an element that is itself a shadow host against a rule from
        //       outside its own shadow root, we must not use the element as the shadow host
        //       for traversal (which would confine traversal to the element itself).
        //       Instead, use the rule's shadow root's host, so that combinators can traverse
        //       up to the enclosing shadow host (e.g. for `:host(...) .descendant` selectors).
        auto const& rule = *rule_to_run.rule;
        auto rule_root = rule_to_run.shadow_root;
        auto shadow_host_to_use = shadow_host;
        if (abstract_element.element().is_shadow_host() && rule_root != abstract_element.element().shadow_root())
            shadow_host_to_use = rule_root ? rule_root->host() : nullptr;

        auto const& selector = rule.selector;

        auto resolved_scope = resolve_scope(abstract_element, rule, shadow_host_to_use, rule_root);
        if (!resolved_scope.has_value())
            continue;

        SelectorMatching::MatchContext context {
            .style_sheet_for_rule = *rule.sheet,
            .subject = abstract_element.element(),
            .rule_shadow_root = rule_root,
            .collect_per_element_selector_involvement_metadata = true,
            .has_result_cache = m_has_result_cache.ptr(),
            .has_fast_reject_filter_cache = m_has_fast_reject_filter_cache.ptr(),
        };
        if (!abstract_element.pseudo_element().has_value() && matching_pseudo_element_styles) {
            if (auto pseudo_element = selector.target_pseudo_element(); pseudo_element.has_value()) {
                if (is_synthetic_pseudo_element(*pseudo_element)) {
                    auto pseudo_element_bit = pseudo_element_style_bit(*pseudo_element);
                    if (*matching_pseudo_element_styles & pseudo_element_bit)
                        continue;
                    if (selector.contains_pseudo_class(PseudoClass::Has)
                        || SelectorMatching::matches_originating_element_for_pseudo_element(selector, *pseudo_element, abstract_element, shadow_host_to_use, context, resolved_scope->root)) {
                        *matching_pseudo_element_styles |= pseudo_element_bit;
                    }
                }
                continue;
            }
        }
        if (!SelectorMatching::matches(selector, abstract_element, shadow_host_to_use, context, resolved_scope->root))
            continue;
        if (resolved_scope->root == &abstract_element.element()
            && !selector.contains_pseudo_class(PseudoClass::Scope)
            && !selector.contains_the_nesting_selector())
            continue;
        if (rule.container_rule) {
            auto const contains_size_feature = rule.container_rule->contains_size_feature();
            auto const contains_style_feature = rule.container_rule->contains_style_feature();

            if (contains_size_feature || contains_style_feature) {
                rule.container_rule->mark_element_style_dependencies(abstract_element);

                if (!rule.container_rule->matches(abstract_element))
                    continue;
            }
        }
        rule_to_run.scope_root = resolved_scope->root;
        rule_to_run.scope_proximity = resolved_scope->proximity;
        matching_rules.append(rule_to_run);
    }

    return matching_rules;
}

static void sort_matching_rules(Vector<StyleComputer::ScopedMatchingRule>& matching_rules)
{
    quick_sort(matching_rules, [&](auto const& a, auto const& b) {
        auto const* a_rule = a.rule;
        auto const* b_rule = b.rule;
        if (a_rule->specificity == b_rule->specificity) {
            if (a.scope_proximity != b.scope_proximity)
                return a.scope_proximity > b.scope_proximity;
            if (a_rule->style_sheet_index == b_rule->style_sheet_index)
                return a_rule->rule_index < b_rule->rule_index;
            return a_rule->style_sheet_index < b_rule->style_sheet_index;
        }
        return a_rule->specificity < b_rule->specificity;
    });
}

void StyleComputer::for_each_property_expanding_shorthands(PropertyID property_id, StyleValue const& value, Function<void(PropertyID, StyleValue const&)> const& set_longhand_property)
{
    // The expansion recursion lives in the Rust style computation core; this wrapper provides
    // the shell-level callbacks and pins every pending-substitution value it creates until the
    // expansion returns.
    struct ExpansionContext {
        Function<void(PropertyID, StyleValue const&)> const& set_longhand_property;
        Vector<NonnullRefPtr<StyleValue const>> pinned_values;
    } expansion_context { set_longhand_property, {} };

    ComputedValuesFFI::FfiShorthandExpansionCallbacks const callbacks {
        .context = &expansion_context,
        .data_of = [](void*, void const* shell) -> void const* {
            return static_cast<StyleValue const*>(shell)->rust_style_value_data();
        },
        .create_pending_substitution = [](void* context, void const* shell) -> void const* {
            auto& expansion_context = *static_cast<ExpansionContext*>(context);
            auto pending_substitution_value = PendingSubstitutionStyleValue::create(*static_cast<StyleValue const*>(shell));
            auto const* pointer = pending_substitution_value.ptr();
            expansion_context.pinned_values.append(move(pending_substitution_value));
            return pointer;
        },
        .set_longhand_property = [](void* context, u16 property_id, void const* shell) {
            auto& expansion_context = *static_cast<ExpansionContext*>(context);
            expansion_context.set_longhand_property(static_cast<PropertyID>(property_id), *static_cast<StyleValue const*>(shell)); },
    };
    ComputedValuesFFI::rust_for_each_property_expanding_shorthands(&callbacks, to_underlying(property_id), &value, value.rust_style_value_data());
}

static RefPtr<CustomPropertyData const> inheritable_custom_property_data(DOM::AbstractElement abstract_element)
{
    auto data = abstract_element.custom_property_data();
    if (!data)
        return nullptr;
    return data->inheritable(abstract_element.document());
}

static Optional<CSS::EasingFunction> resolve_keyframe_easing(CSS::StyleValue const& style_value, DOM::AbstractElement abstract_element)
{
    RefPtr<CSS::StyleValue const> resolved = style_value;
    if (resolved->is_unresolved())
        resolved = Parser::Parser::resolve_unresolved_style_value(Parser::ParsingParams { abstract_element.document() }, abstract_element, {}, CSS::PropertyNameAndID::from_id(CSS::PropertyID::AnimationTimingFunction), resolved->as_unresolved());
    if (!resolved || resolved->is_guaranteed_invalid())
        return {};
    if (resolved->is_value_list()) {
        auto const& list = resolved->as_value_list();
        if (list.size() > 0)
            resolved = list.value_at(0, false);
        else
            return {};
    }
    if (resolved->is_easing() || resolved->is_keyword())
        return CSS::EasingFunction::from_style_value(*resolved);
    return {};
}

void StyleComputer::collect_animation_into(DOM::AbstractElement abstract_element, GC::Ref<Animations::KeyframeEffect> effect, ComputedProperties::Builder& builder) const
{
    collect_animation_into(abstract_element, effect, builder.style(), &builder);
}

void StyleComputer::collect_animation_into(DOM::AbstractElement abstract_element, GC::Ref<Animations::KeyframeEffect> effect, ComputedProperties& computed_properties) const
{
    collect_animation_into(abstract_element, effect, computed_properties, nullptr);
}

void StyleComputer::collect_animation_into(DOM::AbstractElement abstract_element, GC::Ref<Animations::KeyframeEffect> effect, ComputedProperties& computed_properties, ComputedProperties::Builder* builder) const
{
    auto animation = effect->associated_animation();
    if (!animation)
        return;

    auto output_progress = effect->transformed_progress();
    if (!output_progress.has_value())
        return;

    if (!effect->key_frame_set())
        return;

    auto& keyframes = effect->key_frame_set()->keyframes_by_key;
    if (keyframes.size() < 2) {
        if constexpr (LIBWEB_CSS_ANIMATION_DEBUG) {
            dbgln("    Did not find enough keyframes ({} keyframes)", keyframes.size());
            for (auto it = keyframes.begin(); it != keyframes.end(); ++it)
                dbgln("        - {}", it.key());
        }
        return;
    }

    double current_key = output_progress.value() * 100.0 * Animations::KeyframeEffect::AnimationKeyFrameKeyScaleFactor;
    current_key = clamp(current_key, static_cast<double>(NumericLimits<i64>::min()), static_cast<double>(NumericLimits<i64>::max()));

    // Each property is animated using its property-specific keyframes, so two properties in the same animation may be
    // interpolated across different intervals.
    // Collect the keyframes in ascending offset order, and index for each physical longhand the keyframes that specify
    // it, so that the interval endpoints can be found separately for every property.
    LogicalAliasMappingContext const logical_alias_mapping_context { computed_properties.writing_mode(), computed_properties.direction() };
    struct KeyframeInfo {
        i64 key { 0 };
        Animations::KeyframeEffect::KeyFrameSet::ResolvedKeyFrame const* frame { nullptr };
    };
    Vector<KeyframeInfo> ordered_keyframes;
    ordered_keyframes.ensure_capacity(keyframes.size());
    HashMap<PropertyID, Vector<size_t>> keyframes_specifying_property;
    for (auto it = keyframes.begin(); it != keyframes.end(); ++it) {
        auto keyframe_index = ordered_keyframes.size();
        auto add_physical_longhand = [&](PropertyID longhand_id) {
            auto physical_longhand_id = map_logical_alias_to_physical_property(longhand_id, logical_alias_mapping_context);
            auto& specifying_keyframes = keyframes_specifying_property.ensure(physical_longhand_id);
            if (specifying_keyframes.is_empty() || specifying_keyframes.last() != keyframe_index)
                specifying_keyframes.append(keyframe_index);
        };
        for (auto const& [property_id, value] : it->properties) {
            value.visit(
                [&](Animations::KeyframeEffect::KeyFrameSet::UseInitial) { add_physical_longhand(property_id); },
                [&](NonnullRefPtr<StyleValue const> const& keyframe_value) {
                    for_each_property_expanding_shorthands(property_id, *keyframe_value, [&](PropertyID longhand_id, StyleValue const&) {
                        add_physical_longhand(longhand_id);
                    });
                });
        }
        ordered_keyframes.append({ static_cast<i64>(it.key()), &*it });
    }

    // https://drafts.csswg.org/css-animations-1/#animation-timing-function
    // Apply the per-keyframe easing to the interval progress. The easing on a keyframe applies to the
    // interval from that keyframe to the next. If the keyframe doesn't specify an easing, use the
    // animation's default easing (from the animation-timing-function property).
    auto apply_keyframe_easing = [&](auto const& keyframe_easing, double interval_progress) {
        auto resolved_easing = keyframe_easing.visit(
            [](Empty) -> Optional<CSS::EasingFunction> { return {}; },
            [](CSS::EasingFunction const& easing) -> Optional<CSS::EasingFunction> { return easing; },
            [&](NonnullRefPtr<CSS::StyleValue const> const& value) -> Optional<CSS::EasingFunction> {
                return resolve_keyframe_easing(*value, abstract_element);
            });
        if (resolved_easing.has_value())
            return resolved_easing->evaluate_at(interval_progress, false);
        if (animation->is_css_animation())
            return static_cast<CSSAnimation const&>(*animation).default_easing().evaluate_at(interval_progress, false);
        return interval_progress;
    };

    // FIXME: Follow https://drafts.csswg.org/web-animations-1/#ref-for-computed-keyframes in whatever the right place is.
    auto compute_keyframe_values = [&computed_properties, &abstract_element, builder, this](auto const& keyframe_values) {
        HashMap<PropertyID, RefPtr<StyleValue const>> result;
        HashMap<PropertyID, PropertyID> longhands_set_by_property_id;
        AK::FixedBitmap<number_of_longhand_properties> property_is_set_by_use_initial(false);

        auto property_is_logical_alias_including_shorthands = [&](PropertyID property_id) {
            if (property_is_shorthand(property_id))
                // NOTE: All expanded longhands for a logical alias shorthand are logical aliases so we only need to check the first one.
                return property_is_logical_alias(expanded_longhands_for_shorthand(property_id)[0]);

            return property_is_logical_alias(property_id);
        };

        // https://drafts.csswg.org/web-animations-1/#ref-for-computed-keyframes
        auto is_property_preferred = [&](PropertyID a, PropertyID b) {
            // If conflicts arise when expanding shorthand properties or replacing logical properties with physical properties, apply the following rules in order until the conflict is resolved:
            // 1. Longhand properties override shorthand properties (e.g. border-top-color overrides border-top).
            if (property_is_shorthand(a) != property_is_shorthand(b))
                return !property_is_shorthand(a);

            // 2. Shorthand properties with fewer longhand components override those with more longhand components (e.g. border-top overrides border-color).
            if (property_is_shorthand(a)) {
                auto number_of_expanded_shorthands_a = expanded_longhands_for_shorthand(a).size();
                auto number_of_expanded_shorthands_b = expanded_longhands_for_shorthand(b).size();

                if (number_of_expanded_shorthands_a != number_of_expanded_shorthands_b)
                    return number_of_expanded_shorthands_a < number_of_expanded_shorthands_b;
            }

            auto property_a_is_logical_alias = property_is_logical_alias_including_shorthands(a);
            auto property_b_is_logical_alias = property_is_logical_alias_including_shorthands(b);

            // 3. Physical properties override logical properties.
            if (property_a_is_logical_alias != property_b_is_logical_alias)
                return !property_a_is_logical_alias;

            // 4. For shorthand properties with an equal number of longhand components, properties whose IDL name (see
            //    the CSS property to IDL attribute algorithm [CSSOM]) appears earlier when sorted in ascending order
            //    by the Unicode codepoints that make up each IDL name, override those who appear later.
            return camel_case_string_from_property_id(a) < camel_case_string_from_property_id(b);
        };

        HashMap<PropertyID, RefPtr<StyleValue const>> specified_values;

        for (auto const& [property_id, value] : keyframe_values.properties) {
            bool is_use_initial = false;

            auto style_value = value.visit(
                [&](Animations::KeyframeEffect::KeyFrameSet::UseInitial) -> RefPtr<StyleValue const> {
                    if (property_is_shorthand(property_id))
                        return {};
                    is_use_initial = true;
                    return computed_properties.property(property_id, ComputedProperties::WithAnimationsApplied::No);
                },
                [&](RefPtr<StyleValue const> value) -> RefPtr<StyleValue const> {
                    return value;
                });

            if (!style_value) {
                specified_values.set(property_id, nullptr);
                continue;
            }

            // If the style value is a PendingSubstitutionStyleValue we should skip it to avoid overwriting any value
            // already set by resolving the relevant shorthand's value.
            if (style_value->is_pending_substitution())
                continue;

            if (style_value->is_unresolved())
                style_value = Parser::Parser::resolve_unresolved_style_value(Parser::ParsingParams { abstract_element.document() }, abstract_element, {}, PropertyNameAndID::from_id(property_id), style_value->as_unresolved());

            // https://drafts.csswg.org/css-values-5/#invalid-at-computed-value-time
            // When substitution results in a guaranteed-invalid value, treat it as unset
            // (i.e. inherit for inherited properties, initial for non-inherited properties).
            if (style_value->is_guaranteed_invalid()) {
                specified_values.set(property_id, nullptr);
                continue;
            }

            for_each_property_expanding_shorthands(property_id, *style_value, [&](PropertyID longhand_id, StyleValue const& longhand_value) {
                auto physical_longhand_id = map_logical_alias_to_physical_property(longhand_id, LogicalAliasMappingContext { computed_properties.writing_mode(), computed_properties.direction() });
                auto physical_longhand_id_bitmap_index = to_underlying(physical_longhand_id) - to_underlying(first_longhand_property_id);

                // Don't overwrite values if this is the result of a UseInitial
                if (specified_values.contains(physical_longhand_id) && specified_values.get(physical_longhand_id) != nullptr && is_use_initial)
                    return;

                // Don't overwrite unless the value was originally set by a UseInitial or this property is preferred over the one that set it originally
                if (specified_values.contains(physical_longhand_id) && specified_values.get(physical_longhand_id) != nullptr && !property_is_set_by_use_initial.get(physical_longhand_id_bitmap_index) && !is_property_preferred(property_id, longhands_set_by_property_id.get(physical_longhand_id).value()))
                    return;

                auto specified_value_with_css_wide_keywords_applied = [&]() -> NonnullRefPtr<StyleValue const> {
                    if (longhand_value.is_inherit() || (longhand_value.is_unset() && is_inherited_property(longhand_id))) {
                        if (auto inherited_animated_value = get_animated_inherit_value(longhand_id, abstract_element); inherited_animated_value.has_value())
                            return inherited_animated_value->value;

                        return get_non_animated_inherit_value(longhand_id, abstract_element);
                    }

                    if (longhand_value.is_initial() || longhand_value.is_unset())
                        return property_initial_value(longhand_id);

                    if (longhand_value.is_revert() || longhand_value.is_revert_layer())
                        return computed_properties.property(longhand_id);

                    return NonnullRefPtr { longhand_value };
                }();

                longhands_set_by_property_id.set(physical_longhand_id, property_id);
                property_is_set_by_use_initial.set(physical_longhand_id_bitmap_index, is_use_initial);
                specified_values.set(physical_longhand_id, specified_value_with_css_wide_keywords_applied);
            });
        }

        // NOTE: This doesn't necessarily return the specified value if we reach into computed_properties but that
        //       doesn't matter as a computed value is always valid as a specified value.
        Function<NonnullRefPtr<StyleValue const>(PropertyID)> get_property_specified_value = [&](PropertyID property_id) -> NonnullRefPtr<StyleValue const> {
            if (auto keyframe_value = specified_values.get(property_id); keyframe_value.has_value() && keyframe_value.value())
                return *keyframe_value.value();

            return computed_properties.property(property_id);
        };

        for (auto const& [property_id, style_value] : specified_values) {
            if (!style_value)
                continue;

            auto const& computation_context = get_computation_context_for_property(property_id, computed_properties, abstract_element);

            computation_context.reset_viewport_metric_dependency_tracking();
            result.set(property_id, compute_value_of_property(property_id, *style_value, get_property_specified_value, computation_context, m_document->page().client().device_pixels_per_css_pixel()));
            if (computation_context.depends_on_viewport_metrics()) {
                if (builder) {
                    builder->set_depends_on_viewport_metrics();
                    if (property_affects_font_metrics(property_id))
                        builder->set_font_metrics_depend_on_viewport_metrics();
                } else {
                    computed_properties.set_depends_on_viewport_metrics(Badge<StyleComputer> {});
                    if (property_affects_font_metrics(property_id))
                        computed_properties.set_font_metrics_depend_on_viewport_metrics(Badge<StyleComputer> {});
                }
            }
        }

        return result;
    };

    auto to_composite_operation = [&](Bindings::CompositeOperationOrAuto composite_operation_or_auto) {
        switch (composite_operation_or_auto) {
        case Bindings::CompositeOperationOrAuto::Accumulate:
            return Bindings::CompositeOperation::Accumulate;
        case Bindings::CompositeOperationOrAuto::Add:
            return Bindings::CompositeOperation::Add;
        case Bindings::CompositeOperationOrAuto::Replace:
            return Bindings::CompositeOperation::Replace;
        case Bindings::CompositeOperationOrAuto::Auto:
            return effect->composite();
        }
        VERIFY_NOT_REACHED();
    };

    auto is_result_of_transition = animation->is_css_transition() ? AnimatedPropertyResultOfTransition::Yes : AnimatedPropertyResultOfTransition::No;

    Vector<HashMap<PropertyID, RefPtr<StyleValue const>>> keyframe_computed_values;
    keyframe_computed_values.resize(ordered_keyframes.size());
    auto computed_values_for_keyframe = [&](size_t index) -> HashMap<PropertyID, RefPtr<StyleValue const>> const& {
        if (keyframe_computed_values[index].is_empty())
            keyframe_computed_values[index] = compute_keyframe_values(*ordered_keyframes[index].frame);
        return keyframe_computed_values[index];
    };

    VERIFY(computation_context_cache_is_empty());
    auto const& color_computation_context = get_computation_context_for_property(PropertyID::Color, computed_properties, abstract_element);
    ColorResolutionContext color_resolution_context {
        .color_scheme = color_computation_context.color_scheme,
        .current_color = InitialValues::color(),
        .current_color_style_value = &computed_properties.property(PropertyID::Color),
        .calculation_resolution_context = { .length_resolution_context = color_computation_context.length_resolution_context },
    };
    color_resolution_context.current_color = computed_properties.color(PropertyID::Color, color_resolution_context);

    for (auto const& [property_id, specifying_keyframes] : keyframes_specifying_property) {
        // A property is usually specified by at least the initial and final keyframes, but a value that stays
        // unresolved may leave a property with only one specifying keyframe. Such a property cannot be interpolated, so skip it.
        if (specifying_keyframes.size() < 2)
            continue;

        auto start_keyframe = specifying_keyframes[0];
        auto end_keyframe = specifying_keyframes[1];
        for (size_t next = 2; next < specifying_keyframes.size(); ++next) {
            if (current_key < ordered_keyframes[end_keyframe].key)
                break;
            start_keyframe = end_keyframe;
            end_keyframe = specifying_keyframes[next];
        }

        auto start_key = ordered_keyframes[start_keyframe].key;
        auto end_key = ordered_keyframes[end_keyframe].key;
        double interval_progress = (static_cast<double>(current_key) - start_key) / static_cast<double>(end_key - start_key);
        interval_progress = apply_keyframe_easing(ordered_keyframes[start_keyframe].frame->easing, interval_progress);

        RefPtr<StyleValue const> resolved_start_property = computed_values_for_keyframe(start_keyframe).get(property_id).value_or(nullptr);
        RefPtr<StyleValue const> resolved_end_property = computed_values_for_keyframe(end_keyframe).get(property_id).value_or(nullptr);

        if (!resolved_end_property) {
            if (resolved_start_property) {
                computed_properties.set_animated_property(Badge<StyleComputer> {}, property_id, *resolved_start_property, is_result_of_transition);
                dbgln_if(LIBWEB_CSS_ANIMATION_DEBUG, "No end property for property {}, using {}", string_from_property_id(property_id), resolved_start_property->to_string(SerializationMode::Normal));
            }
            continue;
        }

        if (resolved_end_property && !resolved_start_property)
            resolved_start_property = property_initial_value(property_id);

        if (!resolved_start_property || !resolved_end_property)
            continue;

        auto start = resolved_start_property.release_nonnull();
        auto end = resolved_end_property.release_nonnull();

        // OPTIMIZATION: Values resulting from animations other than CSS transitions are overridden by important
        //               properties so there's no need to calculate them
        if (!animation->is_css_transition() && computed_properties.is_property_important(property_id)) {
            continue;
        }

        auto const& underlying_value = computed_properties.property(property_id);
        auto start_composite_operation = to_composite_operation(ordered_keyframes[start_keyframe].frame->composite);
        auto end_composite_operation = to_composite_operation(ordered_keyframes[end_keyframe].frame->composite);

        if (auto composited_start_value = composite_value(property_id, underlying_value, start, start_composite_operation, color_resolution_context))
            start = *composited_start_value;

        if (auto composited_end_value = composite_value(property_id, underlying_value, end, end_composite_operation, color_resolution_context))
            end = *composited_end_value;

        if (auto next_value = interpolate_property(*effect->target(), property_id, *start, *end, interval_progress, AllowDiscrete::Yes, &color_resolution_context)) {
            dbgln_if(LIBWEB_CSS_ANIMATION_DEBUG, "Interpolated value for property {} at {}: {} -> {} = {}", string_from_property_id(property_id), interval_progress, start->to_string(SerializationMode::Normal), end->to_string(SerializationMode::Normal), next_value->to_string(SerializationMode::Normal));
            computed_properties.set_animated_property(Badge<StyleComputer> {}, property_id, *next_value, is_result_of_transition);
        } else {
            // If interpolate_property() fails, the element should not be rendered
            dbgln_if(LIBWEB_CSS_ANIMATION_DEBUG, "Interpolated value for property {} at {}: {} -> {} is invalid", string_from_property_id(property_id), interval_progress, start->to_string(SerializationMode::Normal), end->to_string(SerializationMode::Normal));
            computed_properties.set_animated_property(Badge<StyleComputer> {}, PropertyID::Visibility, KeywordStyleValue::create(Keyword::Hidden), is_result_of_transition);
        }
    }

    clear_computation_context_caches();
}

void StyleComputer::process_animation_definitions(ComputedProperties const& computed_properties, CascadedProperties const& cascaded_properties, DOM::AbstractElement& abstract_element) const
{
    auto const& animation_definitions = computed_properties.animations(abstract_element);

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
    //     Element::play_or_cancel_animations_after_display_property_change().
    // OPTIMIZATION: This involves an ancestor walk, so it's computed lazily since it's only needed on the path that
    //               starts a brand new animation, not for the common case of an element without animations.
    Optional<bool> in_display_none_subtree;
    auto is_in_display_none_subtree = [&] {
        if (!in_display_none_subtree.has_value()) {
            bool result = computed_properties.display().is_none();
            if (!result) {
                if (abstract_element.pseudo_element().has_value())
                    result = abstract_element.element().has_inclusive_ancestor_with_display_none_ignoring_animations();
                else if (auto* parent = abstract_element.element().parent_or_shadow_host())
                    result = parent->has_inclusive_ancestor_with_display_none_ignoring_animations();
            }
            in_display_none_subtree = result;
        }
        return in_display_none_subtree.value();
    };

    // The same @keyframes rule name may be repeated within an animation-name. Changes to the animation-name update
    // existing animations by iterating over the new list of animations from last to first, and, for each animation,
    // finding the last matching animation in the list of existing animations. If a match is found, the existing
    // animation is updated using the animation properties corresponding to its position in the new list of animations,
    // whilst maintaining its current playback time as described above. The matching animation is removed from the
    // existing list of animations such that it will not match twice. If a match is not found, a new animation is
    // created. As a result, updating animation-name from ‘a’ to ‘a, a’ will cause the existing animation for ‘a’ to
    // become the second animation in the list and a new animation will be created for the first item in the list.

    auto existing_animations = *element_animations;
    Vector<GC::Ref<CSSAnimation>> new_animations;

    for (size_t i = animation_definitions.size(); i-- > 0;) {
        auto const& animation_properties = animation_definitions[i];
        auto const& animation_name = animation_properties.name;

        auto find_keyframes = [&](GC::Ptr<DOM::ShadowRoot const> shadow_root) -> RefPtr<Animations::KeyframeEffect::KeyFrameSet const> {
            if (auto const* rule_cache = rule_cache_for_cascade_origin(CascadeOrigin::Author, {}, shadow_root)) {
                if (auto keyframe_set = rule_cache->rules_by_animation_keyframes.get(animation_name); keyframe_set.has_value())
                    return keyframe_set.value();
            }
            return {};
        };

        auto resolve_keyframes = [&]() -> RefPtr<Animations::KeyframeEffect::KeyFrameSet const> {
            if (auto animation_name_source_shadow_root = cascaded_properties.property_source_shadow_root(PropertyID::AnimationName)) {
                // The winning animation-name declaration can come from a shadow-root rule even when the animated
                // element itself is outside that subtree, most notably for :host(...) and ::slotted(...). Resolve
                // @keyframes in the declaration's scope first so same-named document rules do not win.
                if (auto keyframe_set = find_keyframes(animation_name_source_shadow_root))
                    return keyframe_set;
            }

            if (auto shadow_root = as_if<DOM::ShadowRoot>(abstract_element.element().root())) {
                if (auto keyframe_set = find_keyframes(shadow_root))
                    return keyframe_set;
            }

            return find_keyframes(nullptr);
        };

        Optional<size_t> existing_animation_index;

        for (size_t i = existing_animations.size(); i-- > 0;) {
            if (existing_animations[i]->animation_name() == animation_name) {
                existing_animation_index = i;
                break;
            }
        }

        if (existing_animation_index.has_value()) {
            auto existing_animation = existing_animations.take(*existing_animation_index);

            if (auto effect = existing_animation->effect()) {
                as<Animations::KeyframeEffect>(*effect).set_key_frame_set(resolve_keyframes());
                existing_animation->apply_css_properties(animation_properties);
            }
            existing_animation->set_animation_name_index(i);
            new_animations.append(existing_animation);
            continue;
        }

        if (is_in_display_none_subtree())
            continue;

        // An animation applies to an element if its name appears as one of the identifiers in the computed value of the
        // animation-name property and the animation uses a valid @keyframes rule
        auto animation = CSSAnimation::create(document.realm());
        animation->set_animation_name(animation_properties.name);
        animation->set_owning_element(abstract_element);

        auto effect = Animations::KeyframeEffect::create(document.realm());
        animation->set_effect(effect);

        animation->apply_css_properties(animation_properties);
        animation->set_animation_name_index(i);

        effect->set_key_frame_set(resolve_keyframes());

        effect->set_target(abstract_element);
        new_animations.append(animation);
    }

    // Once an animation has started it continues until it ends or the animation-name is removed
    // NB: All animations that are matched by the new set of animations have been removed from `existing_animations` by
    //     this point so any still in the Vector have had their animation-name entries removed.
    for (auto const& existing_animation : existing_animations)
        existing_animation->cancel(Animations::Animation::ShouldInvalidate::No);

    // NB: We create animations in reverse definition order so flip it back.
    new_animations.reverse();

    abstract_element.set_css_defined_animations(move(new_animations));
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

static void compute_transitioned_properties(ComputedProperties const& style, DOM::AbstractElement abstract_element)
{
    // FIXME: For now we don't bother registering transitions on the first computation since they can't run (because
    //        there is nothing to transition from) but this will change once we implement @starting-style
    if (!abstract_element.computed_values())
        return;
    // FIXME: Add transition helpers on AbstractElement.
    auto& element = abstract_element.element();
    auto pseudo_element = abstract_element.pseudo_element();

    element.clear_registered_transitions(pseudo_element);

    auto const& delay = style.property(PropertyID::TransitionDelay);
    auto const& duration = style.property(PropertyID::TransitionDuration);

    auto const value_is_list_containing_a_single_time_of_zero_seconds = [](StyleValue const& value) -> bool {
        if (!value.is_value_list())
            return false;

        auto const& value_list = value.as_value_list().values();

        if (value_list.size() != 1)
            return false;

        if (!value_list[0]->is_time())
            return false;

        return value_list[0]->as_time().time().to_seconds() == 0;
    };

    // OPTIMIZATION: Registered transitions with a "combined duration" of less than or equal to 0s are equivalent to not
    //               having a transition registered at all, except in the case that we already have an associated
    //               transition for that property, so we can skip registering them. This implementation intentionally
    //               ignores some of those cases (e.g. transitions being registered but for other properties, multiple
    //               transitions, negative delays, etc) since it covers the common (initial property values) case and
    //               the other cases are rare enough that the cost of identifying them would likely more than offset any
    //               gains.
    if (
        element.property_ids_with_existing_transitions(pseudo_element).is_empty()
        && value_is_list_containing_a_single_time_of_zero_seconds(delay)
        && value_is_list_containing_a_single_time_of_zero_seconds(duration)) {
        return;
    }

    element.add_transitioned_properties(pseudo_element, style.transitions());
}

// https://drafts.csswg.org/css-transitions/#starting
void StyleComputer::start_needed_transitions(ComputedValues const& previous_style, ComputedProperties::Builder& new_style_builder, DOM::AbstractElement abstract_element) const
{
    auto& new_style = new_style_builder.style();

    // https://drafts.csswg.org/css-transitions/#transition-combined-duration
    auto combined_duration = [](Animations::Animatable::TransitionAttributes const& transition_attributes) {
        // Define the combined duration of the transition as the sum of max(matching transition duration, 0s) and the matching transition delay.
        return max(transition_attributes.duration, 0) + transition_attributes.delay;
    };

    // For each element and property, the implementation must act as follows:
    // NB: We know that a DocumentTimeline's current time is always in milliseconds
    auto current_time = m_document->timeline()->current_time();
    if (!current_time.has_value())
        return;
    VERIFY(current_time->type == Animations::TimeValue::Type::Milliseconds);
    auto style_change_event_time = current_time->value;

    auto after_change_style = build_computed_values(new_style, abstract_element, abstract_element.style_scope());

    // FIXME: Add some transition helpers to AbstractElement.
    auto& element = abstract_element.element();
    auto pseudo_element = abstract_element.pseudo_element();

    // OPTIMIZATION: Instead of iterating over all properties we split the logic into two loops, one for the properties
    //               which appear in transition-property and one for those which have existing transitions
    for (auto property_id : element.property_ids_with_matching_transition_property_entry(pseudo_element)) {
        auto matching_transition_properties = element.property_transition_attributes(pseudo_element, property_id).value();
        auto before_change_style_value = previous_style.computed_style_value(property_id, ComputedValues::WithAnimationsApplied::Yes);
        auto after_change_style_value = after_change_style->computed_style_value(property_id, ComputedValues::WithAnimationsApplied::No);
        VERIFY(before_change_style_value);
        VERIFY(after_change_style_value);
        auto const& before_change_value = *before_change_style_value;
        auto const& after_change_value = *after_change_style_value;
        auto originates_from_current_color = [](ComputedValues const& style, PropertyID property_id) {
            auto value = style.inheritance_dependent_specified_values().get(property_id);
            return value.has_value() && value.value()->to_keyword() == Keyword::Currentcolor;
        };
        bool before_change_style_is_different = !before_change_value.equals(after_change_value);
        if (originates_from_current_color(previous_style, property_id) && originates_from_current_color(*after_change_style, property_id))
            before_change_style_is_different = false;

        auto existing_transition = element.property_transition(pseudo_element, property_id);
        bool has_running_transition = existing_transition && !existing_transition->is_finished() && !existing_transition->is_idle();
        bool has_completed_transition = existing_transition && (existing_transition->is_finished() || existing_transition->is_idle());

        auto start_a_transition = [&](auto delay, auto start_time, auto end_time, auto const& start_value, auto const& end_value, auto const& reversing_adjusted_start_value, auto reversing_shortening_factor) {
            dbgln_if(CSS_TRANSITIONS_DEBUG, "Starting a transition of {} from {} to {}", string_from_property_id(property_id), start_value.to_string(SerializationMode::Normal), end_value.to_string(SerializationMode::Normal));

            auto transition = CSSTransition::start_a_transition(abstract_element, property_id,
                document().transition_generation(), delay, start_time, end_time, start_value, end_value, reversing_adjusted_start_value, reversing_shortening_factor);
            // Immediately set the property's value to the transition's current value, to prevent single-frame jumps.
            collect_animation_into(abstract_element, as<Animations::KeyframeEffect>(*transition->effect()), new_style_builder);
        };

        // 1. If all of the following are true:
        if (
            // - the element does not have a running transition for the property,
            (!has_running_transition) &&
            // - there is a matching transition-property value, and
            // NOTE: We only iterate over properties for which this is true
            // - the before-change style is different from the after-change style for that property, and the values for the property are transitionable,
            (before_change_style_is_different && property_values_are_transitionable(property_id, before_change_value, after_change_value, element, matching_transition_properties.transition_behavior)) &&
            // - the element does not have a completed transition for the property
            //   or the end value of the completed transition is different from the after-change style for the property,
            (!has_completed_transition || !existing_transition->transition_end_value()->equals(after_change_value)) &&
            // - the combined duration is greater than 0s,
            (combined_duration(matching_transition_properties) > 0)) {

            dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 1.");

            // then implementations must remove the completed transition (if present) from the set of completed transitions
            if (has_completed_transition)
                element.remove_transition(pseudo_element, property_id);
            // and start a transition whose:

            // AD-HOC: We pass delay to the constructor separately so we can use it to construct the contained KeyframeEffect
            auto delay = matching_transition_properties.delay;

            // - start time is the time of the style change event plus the matching transition delay,
            auto start_time = style_change_event_time;

            // - end time is the start time plus the matching transition duration,
            auto end_time = start_time + matching_transition_properties.duration;

            // - start value is the value of the transitioning property in the before-change style,
            auto const& start_value = before_change_value;

            // - end value is the value of the transitioning property in the after-change style,
            auto const& end_value = after_change_value;

            // - reversing-adjusted start value is the same as the start value, and
            auto const& reversing_adjusted_start_value = start_value;

            // - reversing shortening factor is 1.
            double reversing_shortening_factor = 1;

            start_a_transition(delay, start_time, end_time, start_value, end_value, reversing_adjusted_start_value, reversing_shortening_factor);
        }

        // 2. Otherwise, if the element has a completed transition for the property
        //    and the end value of the completed transition is different from the after-change style for the property,
        //    then implementations must remove the completed transition from the set of completed transitions.
        else if (has_completed_transition && !existing_transition->transition_end_value()->equals(after_change_value)) {
            dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 2.");
            element.remove_transition(pseudo_element, property_id);
        }

        // NOTE: Step 3 is handled in a separate loop below for performance reasons

        // 4. If the element has a running transition for the property,
        //    there is a matching transition-property value,
        //    and the end value of the running transition is not equal to the value of the property in the after-change style, then:
        if (has_running_transition && !existing_transition->transition_end_value()->equals(after_change_value)) {
            dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 4. existing end value = {}, after change value = {}", existing_transition->transition_end_value()->to_string(SerializationMode::Normal), after_change_value.to_string(SerializationMode::Normal));
            // 1. If the current value of the property in the running transition is equal to the value of the property in the after-change style,
            //    or if these two values are not transitionable,
            //    then implementations must cancel the running transition.
            auto current_style_value = after_change_style->computed_style_value(property_id, ComputedValues::WithAnimationsApplied::Yes);
            VERIFY(current_style_value);
            auto const& current_value = *current_style_value;
            if (current_value.equals(after_change_value) || !property_values_are_transitionable(property_id, current_value, after_change_value, element, matching_transition_properties.transition_behavior)) {
                dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 4.1");
                existing_transition->cancel();
            }

            // 2. Otherwise, if the combined duration is less than or equal to 0s,
            //    or if the current value of the property in the running transition is not transitionable with the value of the property in the after-change style,
            //    then implementations must cancel the running transition.
            else if ((combined_duration(matching_transition_properties) <= 0)
                || !property_values_are_transitionable(property_id, current_value, after_change_value, element, matching_transition_properties.transition_behavior)) {
                dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 4.2");
                existing_transition->cancel();
            }

            // 3. Otherwise, if the reversing-adjusted start value of the running transition is the same as the value of the property in the after-change style
            //    (see the section on reversing of transitions for why these case exists),
            else if (existing_transition->reversing_adjusted_start_value()->equals(after_change_value)) {
                dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 4.3");
                // implementations must cancel the running transition and start a new transition whose:
                existing_transition->cancel();
                // AD-HOC: Remove the cancelled transition, otherwise it breaks the invariant that there is only one
                // running or completed transition for a property at once.
                element.remove_transition(pseudo_element, property_id);

                // - reversing-adjusted start value is the end value of the running transition,
                auto reversing_adjusted_start_value = existing_transition->transition_end_value();

                // - reversing shortening factor is the absolute value, clamped to the range [0, 1], of the sum of:
                //   1. the output of the timing function of the old transition at the time of the style change event,
                //      times the reversing shortening factor of the old transition
                auto term_1 = existing_transition->timing_function_output_at_time(style_change_event_time) * existing_transition->reversing_shortening_factor();
                //   2. 1 minus the reversing shortening factor of the old transition.
                auto term_2 = 1 - existing_transition->reversing_shortening_factor();
                double reversing_shortening_factor = clamp(abs(term_1 + term_2), 0.0, 1.0);

                // AD-HOC: We pass delay to the constructor separately so we can use it to construct the contained KeyframeEffect
                auto delay = (matching_transition_properties.delay >= 0
                        ? (matching_transition_properties.delay)
                        : (reversing_shortening_factor * matching_transition_properties.delay));

                // - start time is the time of the style change event plus:
                //   1. if the matching transition delay is nonnegative, the matching transition delay, or
                //   2. if the matching transition delay is negative, the product of the new transition’s reversing shortening factor and the matching transition delay,
                auto start_time = style_change_event_time;

                // - end time is the start time plus the product of the matching transition duration and the new transition’s reversing shortening factor,
                auto end_time = start_time + (matching_transition_properties.duration * reversing_shortening_factor);

                // - start value is the current value of the property in the running transition,
                auto const& start_value = current_value;

                // - end value is the value of the property in the after-change style,
                auto const& end_value = after_change_value;

                start_a_transition(delay, start_time, end_time, start_value, end_value, reversing_adjusted_start_value, reversing_shortening_factor);
            }

            // 4. Otherwise,
            else {
                dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 4.4");
                // implementations must cancel the running transition and start a new transition whose:
                existing_transition->cancel();
                // AD-HOC: Remove the cancelled transition, otherwise it breaks the invariant that there is only one
                // running or completed transition for a property at once.
                element.remove_transition(pseudo_element, property_id);

                // AD-HOC: We pass delay to the constructor separately so we can use it to construct the contained KeyframeEffect
                auto delay = matching_transition_properties.delay;

                // - start time is the time of the style change event plus the matching transition delay,
                auto start_time = style_change_event_time;

                // - end time is the start time plus the matching transition duration,
                auto end_time = start_time + matching_transition_properties.duration;

                // - start value is the current value of the property in the running transition,
                auto const& start_value = current_value;

                // - end value is the value of the property in the after-change style,
                auto const& end_value = after_change_value;

                // - reversing-adjusted start value is the same as the start value, and
                auto const& reversing_adjusted_start_value = start_value;

                // - reversing shortening factor is 1.
                double reversing_shortening_factor = 1;

                start_a_transition(delay, start_time, end_time, start_value, end_value, reversing_adjusted_start_value, reversing_shortening_factor);
            }
        }
    }

    for (auto property_id : element.property_ids_with_existing_transitions(pseudo_element)) {
        // 3. If the element has a running transition or completed transition for the property, and there is not a
        //    matching transition-property value, then implementations must cancel the running transition or remove the
        //    completed transition from the set of completed transitions.
        if (element.property_transition_attributes(pseudo_element, property_id).has_value())
            continue;

        auto const& existing_transition = element.property_transition(pseudo_element, property_id);

        dbgln_if(CSS_TRANSITIONS_DEBUG, "Transition step 3.");
        if (!existing_transition->is_finished() && !existing_transition->is_idle())
            existing_transition->cancel();
        else
            element.remove_transition(pseudo_element, property_id);
    }
}

StyleComputer::MatchingRuleSet StyleComputer::build_matching_rule_set(DOM::AbstractElement abstract_element, bool& did_match_any_pseudo_element_rules, ComputeStyleMode mode) const
{
    MatchingRuleSet matching_rule_set;
    u64* matching_pseudo_element_styles = nullptr;
    if (mode == ComputeStyleMode::Normal && !abstract_element.pseudo_element().has_value())
        matching_pseudo_element_styles = &matching_rule_set.matching_pseudo_element_styles;

    auto collect_author_contexts = [&] {
        Vector<GC::Ptr<DOM::ShadowRoot const>, 4> context_shadow_roots;
        auto append_context = [&](GC::Ptr<DOM::ShadowRoot const> shadow_root) {
            if (context_shadow_roots.contains_slow(shadow_root))
                return;
            context_shadow_roots.append(shadow_root);
        };

        append_context(nullptr);

        // https://drafts.csswg.org/css-cascade-5/#cascade-context
        // Keep contexts in outer-to-inner order so the cascade can apply them in the spec order without re-sorting
        // individual declarations. Most elements only have the document context, and the small vector avoids heap
        // storage for the common shadow-depth cases.
        if (auto const* shadow_root = as_if<DOM::ShadowRoot>(abstract_element.element().root())) {
            Vector<GC::Ref<DOM::ShadowRoot const>, 4> ancestor_shadow_roots;
            for (auto const* current_shadow_root = shadow_root; current_shadow_root;) {
                ancestor_shadow_roots.append(*current_shadow_root);
                auto const* host = current_shadow_root->host();
                if (!host)
                    break;
                current_shadow_root = as_if<DOM::ShadowRoot>(host->root());
            }
            for (auto& ancestor_shadow_root : ancestor_shadow_roots.in_reverse())
                append_context(ancestor_shadow_root);
        }

        if (!is<HTML::HTMLSlotElement>(abstract_element.element()) || !abstract_element.element().root().is_shadow_root()) {
            for (GC::Ptr<HTML::HTMLSlotElement const> slot = abstract_element.element().assigned_slot_internal(); slot; slot = slot->assigned_slot_internal()) {
                if (auto const* slot_shadow_root = as_if<DOM::ShadowRoot>(slot->root()))
                    append_context(slot_shadow_root);
            }
        }

        if (auto element_shadow_root = abstract_element.element().shadow_root())
            append_context(element_shadow_root);

        Vector<ContextMatchingRules> author_contexts;
        author_contexts.ensure_capacity(context_shadow_roots.size());

        for (auto shadow_root : context_shadow_roots) {
            auto& context_style_scope = shadow_root ? shadow_root->style_scope() : document().style_scope();
            auto const& context_rule_cache = context_style_scope.rule_cache();

            ContextMatchingRules context {
                .shadow_root = shadow_root,
                .author_rules = {},
            };

            for (auto const& layer_name : context_rule_cache.qualified_layer_names_in_order) {
                auto layer_rules = collect_matching_rules_from_context(abstract_element, CascadeOrigin::Author, shadow_root, layer_name, matching_pseudo_element_styles);
                sort_matching_rules(layer_rules);
                context.author_rules.append({ layer_name, layer_rules });
            }

            auto unlayered_author_rules = collect_matching_rules_from_context(abstract_element, CascadeOrigin::Author, shadow_root, {}, matching_pseudo_element_styles);
            sort_matching_rules(unlayered_author_rules);
            context.author_rules.append({ {}, unlayered_author_rules });

            author_contexts.append(move(context));
        }

        return author_contexts;
    };

    // First, we collect all the CSS rules whose selectors match `element`:
    matching_rule_set.user_agent_rules = collect_matching_rules_from_context(abstract_element, CascadeOrigin::UserAgent, nullptr, {}, matching_pseudo_element_styles);
    sort_matching_rules(matching_rule_set.user_agent_rules);
    matching_rule_set.user_rules = collect_matching_rules_from_context(abstract_element, CascadeOrigin::User, nullptr, {}, matching_pseudo_element_styles);
    sort_matching_rules(matching_rule_set.user_rules);
    matching_rule_set.author_contexts = collect_author_contexts();

    if (mode == ComputeStyleMode::CreatePseudoElementStyleIfNeeded) {
        VERIFY(abstract_element.pseudo_element().has_value());
        auto author_rules_has_any_rules = any_of(matching_rule_set.author_contexts, [](auto const& context) {
            return any_of(context.author_rules, [](auto const& layer) {
                return !layer.rules.is_empty();
            });
        });
        did_match_any_pseudo_element_rules = author_rules_has_any_rules
            || !matching_rule_set.user_rules.is_empty()
            || !matching_rule_set.user_agent_rules.is_empty();
    }
    return matching_rule_set;
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

static JsonArray serialize_devtools_style_declarations(DOM::Document const& document, CSSStyleProperties const& declaration)
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
            declaration.value,
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

static bool has_inherited_declaration(DOM::Document const& document, CSSStyleProperties const& declaration)
{
    if (any_of(declaration.properties(), [](auto const& property) {
            return CSS::is_inherited_property(property.property_id);
        })) {
        return true;
    }

    return any_of(declaration.custom_properties(), [&](auto const& custom_property) {
        return custom_property_inherits(document, custom_property.key);
    });
}

static JsonArray serialize_devtools_selectors(MatchingRule const& rule)
{
    JsonArray selectors;
    for (auto const& selector : rule.absolutized_selectors())
        selectors.must_append(selector->serialize().to_utf8());
    return selectors;
}

static JsonArray serialize_devtools_selector_specificities(MatchingRule const& rule)
{
    JsonArray specificities;
    for (auto const& selector : rule.absolutized_selectors())
        specificities.must_append(selector->specificity());
    return specificities;
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

static Optional<StyleSheetIdentifier> devtools_style_sheet_identifier_for_matching_rule(MatchingRule const& rule)
{
    if (rule.cascade_origin == CascadeOrigin::User) {
        return StyleSheetIdentifier {
            .type = StyleSheetIdentifier::Type::UserStyle,
        };
    }

    if (rule.cascade_origin == CascadeOrigin::UserAgent) {
        if (!rule.sheet)
            return {};
        return StyleScope::user_agent_style_sheet_identifier(*rule.sheet);
    }

    if (auto const* style_sheet = rule.rule->parent_style_sheet())
        return style_sheet_identifier_for(*style_sheet);

    return {};
}

static JsonObject serialize_devtools_matching_rule(DOM::Document const& document, MatchingRule const& rule)
{
    auto const& declaration = rule.declaration();
    auto authored_text = extract_css_declaration_block_from_source(*rule.rule);

    JsonArray matched_selector_indexes;
    matched_selector_indexes.must_append(rule.selector_index);

    JsonObject serialized_rule;
    serialized_rule.set("type"sv, to_underlying(rule.rule->type()));
    serialized_rule.set("className"sv, rule.rule->type() == CSSRule::Type::Style ? "CSSStyleRule"sv : "CSSNestedDeclarations"sv);
    serialized_rule.set("selectors"sv, serialize_devtools_selectors(rule));
    serialized_rule.set("selectorsSpecificity"sv, serialize_devtools_selector_specificities(rule));
    serialized_rule.set("matchedSelectorIndexes"sv, move(matched_selector_indexes));
    serialized_rule.set("cssText"sv, rule.rule->serialized().to_utf8());
    if (authored_text.has_value()) {
        serialized_rule.set("authoredText"sv, *authored_text);
        serialized_rule.set("declarations"sv, serialize_devtools_style_declarations(document, parse_devtools_style_declarations(document, authored_text->bytes_as_string_view())));
    } else {
        serialized_rule.set("authoredText"sv, declaration.serialized().to_utf8());
        serialized_rule.set("declarations"sv, serialize_devtools_style_declarations(document, declaration));
    }
    serialized_rule.set("styleSheetIndex"sv, rule.style_sheet_index);
    serialized_rule.set("ruleIndex"sv, rule.rule_index);
    serialized_rule.set("isSystem"sv, rule.cascade_origin == CascadeOrigin::UserAgent);

    if (auto const& source_location = rule.rule->source_location(); source_location.has_value()) {
        // Our positions are 0-based, but DevTools expects them to be 1-based.
        serialized_rule.set("line"sv, source_location->line + 1);
        serialized_rule.set("column"sv, source_location->column + 1);
    }

    if (auto identifier = devtools_style_sheet_identifier_for_matching_rule(rule); identifier.has_value())
        serialized_rule.set("styleSheet"sv, serialize_devtools_style_sheet_identifier(identifier.release_value()));

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
        serialized_rule.set("declarations"sv, serialize_devtools_style_declarations(document, declaration));
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

static void append_devtools_rules_for_element(DOM::Document const& document, JsonArray& entries, auto const& matching_rule_set, bool include_user_agent_styles, Optional<UniqueNodeID> inherited_node_id = {})
{
    auto should_include_rule = [&](MatchingRule const& rule) {
        return !inherited_node_id.has_value() || has_inherited_declaration(document, rule.declaration());
    };

    auto append_rules = [&](auto const& matching_rules) {
        for (auto const& matching_rule : matching_rules.in_reverse()) {
            auto const& rule = *matching_rule.rule;
            if (!should_include_rule(rule))
                continue;
            append_devtools_applied_style_entry(entries, serialize_devtools_matching_rule(document, rule), inherited_node_id);
        }
    };

    for (auto const& context : matching_rule_set.author_contexts.in_reverse()) {
        for (auto const& layer : context.author_rules.in_reverse())
            append_rules(layer.rules);
    }
    append_rules(matching_rule_set.user_rules);
    if (include_user_agent_styles)
        append_rules(matching_rule_set.user_agent_rules);
}

JsonArray StyleComputer::collect_devtools_applied_style_rules(DOM::AbstractElement abstract_element, bool include_inherited, bool include_user_agent_styles)
{
    JsonArray entries;

    auto append_rules_for_abstract_element = [&](DOM::AbstractElement current_element, Optional<UniqueNodeID> inherited_node_id) {
        if (auto inline_style = current_element.inline_style()) {
            if (!inherited_node_id.has_value() || has_inherited_declaration(m_document, *inline_style))
                append_devtools_applied_style_entry(entries, serialize_devtools_inline_style(m_document, current_element, *inline_style), inherited_node_id);
        }

        auto const first_ancestor = [&] -> GC::Ptr<DOM::Element const> {
            if (current_element.pseudo_element().has_value())
                return &current_element.element();
            return current_element.element().parent_or_shadow_host_element();
        }();

        for (auto ancestor = first_ancestor; ancestor; ancestor = ancestor->parent_or_shadow_host_element())
            push_ancestor(*ancestor);

        ScopeGuard pop_ancestors = [&] {
            for (auto ancestor = first_ancestor; ancestor; ancestor = ancestor->parent_or_shadow_host_element())
                pop_ancestor(*ancestor);
        };

        bool did_match_any_pseudo_element_rules = false;
        auto matching_rule_set = build_matching_rule_set(current_element, did_match_any_pseudo_element_rules, ComputeStyleMode::Normal);
        append_devtools_rules_for_element(m_document, entries, matching_rule_set, include_user_agent_styles, inherited_node_id);
    };

    append_rules_for_abstract_element(abstract_element, {});

    if (!include_inherited)
        return entries;

    for (auto current_element = abstract_element.element_to_inherit_style_from(); current_element.has_value(); current_element = current_element->element_to_inherit_style_from())
        append_rules_for_abstract_element(*current_element, current_element->element().unique_id());

    return entries;
}

// https://www.w3.org/TR/css-cascade/#cascading
// https://drafts.csswg.org/css-cascade-5/#layering
NonnullRefPtr<CascadedProperties> StyleComputer::compute_cascaded_values(DOM::AbstractElement abstract_element, MatchingRuleSet const& matching_rule_set, IncludeInlineStyle include_inline_style) const
{
    auto cascaded_properties = CascadedProperties::create();

    auto element_context_shadow_root = as_if<DOM::ShadowRoot>(abstract_element.element().root());

    // The whole css-cascade-5 origin sequence runs in the Rust style computation core over
    // one bulk description of the matched declaration blocks. Declarations, layer names and
    // sources are collected and pinned here; the core derives the application order from
    // the origin and the author context and layer indices.
    struct BlockSource {
        GC::Ptr<CSSStyleDeclaration const> source;
        GC::Ptr<DOM::ShadowRoot const> source_shadow_root;
    };
    Vector<ComputedValuesFFI::FfiCascadeDeclaration> all_declarations;
    Vector<ComputedValuesFFI::FfiCustomPropertyDeclaration> all_custom_property_declarations;
    struct PendingBlock {
        ComputedValuesFFI::FfiCascadeBlock block;
        size_t declarations_offset { 0 };
        size_t custom_property_declarations_offset { 0 };
    };
    Vector<PendingBlock> pending_blocks;
    Vector<BlockSource> block_sources;
    Vector<FlatPtr> leaked_layer_names;
    Vector<FlatPtr> leaked_custom_property_names;

    auto add_block = [&](ReadonlySpan<StyleProperty> properties, OrderedHashMap<Utf16FlyString, StyleProperty> const* custom_properties, CascadeOrigin origin, u32 author_context_index, u32 layer_index, bool is_inline_style, bool bypass_pseudo_element_property_whitelist, Optional<Utf16FlyString> const& layer_name, GC::Ptr<CSSStyleDeclaration const> source, GC::Ptr<DOM::ShadowRoot const> source_shadow_root) {
        auto declarations_offset = all_declarations.size();
        all_declarations.ensure_capacity(all_declarations.size() + properties.size());
        for (auto const& property : properties) {
            all_declarations.unchecked_append({
                .property_id = to_underlying(property.property_id),
                .important = property.important == Important::Yes,
                .shell = property.value.ptr(),
                .data = property.value->rust_style_value_data(),
            });
        }
        auto custom_property_declarations_offset = all_custom_property_declarations.size();
        if (custom_properties) {
            all_custom_property_declarations.ensure_capacity(all_custom_property_declarations.size() + custom_properties->size());
            for (auto const& [name, property] : *custom_properties) {
                auto name_raw = name.to_raw_leaked();
                leaked_custom_property_names.append(name_raw);
                all_custom_property_declarations.unchecked_append({
                    .name_raw = name_raw,
                    .important = property.important == Important::Yes,
                    .is_revert_layer = property.value->is_revert_layer(),
                    .shell = property.value.ptr(),
                });
            }
        }
        FlatPtr layer_name_raw = 0;
        if (layer_name.has_value()) {
            layer_name_raw = layer_name->to_raw_leaked();
            leaked_layer_names.append(layer_name_raw);
        }
        pending_blocks.append({
            .block = {
                .origin = static_cast<ComputedValuesFFI::CascadeOrigin>(to_underlying(origin)),
                .author_context_index = author_context_index,
                .layer_index = layer_index,
                .is_inline_style = is_inline_style,
                .bypass_pseudo_element_property_whitelist = bypass_pseudo_element_property_whitelist,
                .has_layer_name = layer_name.has_value(),
                .layer_name_raw = layer_name_raw,
                .source_shadow_root_identity = bit_cast<FlatPtr>(source_shadow_root.ptr()),
                .source_id = static_cast<u32>(block_sources.size()),
                .declarations = nullptr,
                .declaration_count = properties.size(),
                .custom_property_declarations = nullptr,
                .custom_property_declaration_count = custom_properties ? custom_properties->size() : 0,
            },
            .declarations_offset = declarations_offset,
            .custom_property_declarations_offset = custom_property_declarations_offset,
        });
        block_sources.append({ source, source_shadow_root });
    };

    for (auto const& match : matching_rule_set.user_agent_rules) {
        auto const& declaration = match.rule->declaration();
        add_block(declaration.properties(), nullptr, CascadeOrigin::UserAgent, 0, 0, false, false, {}, &declaration, match.shadow_root);
    }
    for (auto const& match : matching_rule_set.user_rules) {
        auto const& declaration = match.rule->declaration();
        add_block(declaration.properties(), nullptr, CascadeOrigin::User, 0, 0, false, false, {}, &declaration, match.shadow_root);
    }

    // Author presentational hints
    // The spec calls this a special "Author presentational hint origin":
    // "For the purpose of cascading this author presentational hint origin is treated as an independent origin;
    // however for the purpose of the revert keyword (but not for the revert-layer keyword) it is considered
    // part of the author origin."
    // https://drafts.csswg.org/css-cascade-5/#author-presentational-hint-origin
    Vector<StyleProperty> presentational_hint_properties;
    if (!abstract_element.pseudo_element().has_value()) {
        auto& element = abstract_element.element();
        element.apply_presentational_hints(presentational_hint_properties);
        if (element.supports_dimension_attributes()) {
            auto const& dimension_source = is<HTML::HTMLImageElement>(element)
                ? static_cast<HTML::HTMLImageElement const&>(element).dimension_attribute_source()
                : element;
            collect_dimension_attribute(presentational_hint_properties, dimension_source, HTML::AttributeNames::width, CSS::PropertyID::Width);
            collect_dimension_attribute(presentational_hint_properties, dimension_source, HTML::AttributeNames::height, CSS::PropertyID::Height);
        }
        if (!presentational_hint_properties.is_empty())
            add_block(presentational_hint_properties, nullptr, CascadeOrigin::AuthorPresentationalHint, 0, 0, false, false, {}, nullptr, nullptr);
    }

    for (u32 context_index = 0; context_index < matching_rule_set.author_contexts.size(); ++context_index) {
        auto const& author_context = matching_rule_set.author_contexts[context_index];
        for (u32 layer_index = 0; layer_index < author_context.author_rules.size(); ++layer_index) {
            auto const& layer = author_context.author_rules[layer_index];
            for (auto const& match : layer.rules) {
                auto const& declaration = match.rule->declaration();
                Optional<Utf16FlyString> layer_name;
                if (!layer.qualified_layer_name.is_empty())
                    layer_name = layer.qualified_layer_name;
                add_block(declaration.properties(), &declaration.custom_properties(), CascadeOrigin::Author, context_index, layer_index, false, false, layer_name, &declaration, match.shadow_root);
            }
        }
        if (include_inline_style == IncludeInlineStyle::Yes && author_context.shadow_root == element_context_shadow_root) {
            // NB: Inline style bypasses the pseudo-element property whitelist since inline style is used
            //     internally to style element-reference pseudo-elements and sometimes contains disallowed
            //     properties (e.g. input::placeholder has height set); authors can't set inline style on
            //     pseudo-elements so this doesn't cause any spec compliance issues.
            if (auto const inline_style = abstract_element.inline_style())
                add_block(inline_style->properties(), &inline_style->custom_properties(), CascadeOrigin::Author, context_index, 0, true, true, {}, inline_style, nullptr);
        }
    }

    Vector<ComputedValuesFFI::FfiCascadeBlock> blocks;
    blocks.ensure_capacity(pending_blocks.size());
    for (auto& pending : pending_blocks) {
        pending.block.declarations = all_declarations.data() + pending.declarations_offset;
        if (pending.block.custom_property_declaration_count > 0)
            pending.block.custom_property_declarations = all_custom_property_declarations.data() + pending.custom_property_declarations_offset;
        blocks.unchecked_append(pending.block);
    }

    struct BulkCascadeContext {
        CascadedProperties& cascaded_properties;
        DOM::AbstractElement& abstract_element;
        Vector<BlockSource> const& block_sources;
        Vector<NonnullRefPtr<StyleValue const>> pinned_values;
    } bulk_context {
        .cascaded_properties = *cascaded_properties,
        .abstract_element = abstract_element,
        .block_sources = block_sources,
        .pinned_values = {},
    };

    auto unset_value = KeywordStyleValue::create(Keyword::Unset);

    ComputedValuesFFI::FfiBulkCascadeCallbacks const callbacks {
        .context = &bulk_context,
        .resolve_unresolved = [](void* context, u16 property_id, void const* shell) -> ComputedValuesFFI::FfiResolvedStyleValue {
            auto& bulk_context = *static_cast<BulkCascadeContext*>(context);
            auto resolved = Parser::Parser::resolve_unresolved_style_value(
                Parser::ParsingParams { bulk_context.abstract_element.document() },
                bulk_context.abstract_element,
                {},
                PropertyNameAndID::from_id(static_cast<PropertyID>(property_id)),
                static_cast<StyleValue const*>(shell)->as_unresolved());
            ComputedValuesFFI::FfiResolvedStyleValue result {
                .shell = resolved.ptr(),
                .data = resolved->rust_style_value_data(),
            };
            bulk_context.pinned_values.append(move(resolved));
            return result;
        },
        .parse_substituted = [](void* context, u16 property_id, u8 const* source, size_t source_length) -> ComputedValuesFFI::FfiResolvedStyleValue {
            auto& bulk_context = *static_cast<BulkCascadeContext*>(context);
            bulk_context.abstract_element.element().set_style_uses_var_css_function();
            auto parsed = parse_css_value(
                Parser::ParsingParams { bulk_context.abstract_element.document() },
                StringView { reinterpret_cast<char const*>(source), source_length },
                static_cast<PropertyID>(property_id));
            NonnullRefPtr<StyleValue const> resolved = parsed
                ? parsed.release_nonnull()
                : GuaranteedInvalidStyleValue::create();
            ComputedValuesFFI::FfiResolvedStyleValue result {
                .shell = resolved.ptr(),
                .data = resolved->rust_style_value_data(),
            };
            bulk_context.pinned_values.append(move(resolved));
            return result;
        },
        .data_of = [](void*, void const* shell) -> void const* {
            return static_cast<StyleValue const*>(shell)->rust_style_value_data();
        },
        .create_pending_substitution = [](void* context, void const* shell) -> void const* {
            auto& bulk_context = *static_cast<BulkCascadeContext*>(context);
            auto pending_substitution_value = PendingSubstitutionStyleValue::create(*static_cast<StyleValue const*>(shell));
            auto const* pointer = pending_substitution_value.ptr();
            bulk_context.pinned_values.append(move(pending_substitution_value));
            return pointer;
        },
        .pseudo_element_rejects_property = [](void* context, u16 property_id) -> bool {
            auto& bulk_context = *static_cast<BulkCascadeContext*>(context);
            return !pseudo_element_supports_property(*bulk_context.abstract_element.pseudo_element(), static_cast<PropertyID>(property_id));
        },
        .assign_source_slots = [](void* context, ComputedValuesFFI::FfiSourceSlotAssignment const* assignments, size_t count) {
            auto& bulk_context = *static_cast<BulkCascadeContext*>(context);
            for (size_t i = 0; i < count; ++i) {
                auto const& source = bulk_context.block_sources[assignments[i].source_id];
                bulk_context.cascaded_properties.assign_source_slot(assignments[i].slot, source.source, source.source_shadow_root);
            } },
        .set_custom_properties = [](void* context, ComputedValuesFFI::FfiCascadedCustomProperty const* properties, size_t count) -> void const* {
            auto& bulk_context = *static_cast<BulkCascadeContext*>(context);
            OrderedHashMap<Utf16FlyString, StyleProperty> cascaded_all;
            cascaded_all.ensure_capacity(count);
            for (size_t i = 0; i < count; ++i) {
                auto const& property = properties[i];
                cascaded_all.set(
                    Utf16FlyString::from_raw(property.name_raw),
                    StyleProperty {
                        .important = property.important ? Important::Yes : Important::No,
                        .property_id = PropertyID::Custom,
                        .value = *static_cast<StyleValue const*>(property.shell),
                    });
            }

            RefPtr<CustomPropertyData const> parent_data;
            auto inherit_from = bulk_context.abstract_element.element_to_inherit_style_from();
            if (inherit_from.has_value())
                parent_data = inheritable_custom_property_data(*inherit_from);

            OrderedHashMap<Utf16FlyString, StyleProperty> cascaded_own;
            for (auto& [name, property] : cascaded_all) {
                if (parent_data) {
                    auto const* parent_property = parent_data->get(name);
                    if (parent_property && parent_property->value.ptr() == property.value.ptr())
                        continue;
                }
                cascaded_own.set(name, move(property));
            }

            if (cascaded_own.is_empty() && parent_data) {
                bulk_context.abstract_element.set_custom_property_data(parent_data);
            } else if (cascaded_own.is_empty()) {
                bulk_context.abstract_element.set_custom_property_data(nullptr);
            } else {
                bulk_context.abstract_element.set_custom_property_data(
                    CustomPropertyData::create(move(cascaded_own), move(parent_data)));
            }

            auto custom_property_data = bulk_context.abstract_element.custom_property_data();
            return custom_property_data ? custom_property_data->rust_store() : nullptr;
        },
    };

    auto cascade_custom_properties = !abstract_element.pseudo_element().has_value()
        || pseudo_element_supports_property(*abstract_element.pseudo_element(), PropertyID::Custom);

    ComputedValuesFFI::rust_cascade_matched_blocks(
        cascaded_properties->rust_store(),
        blocks.data(),
        blocks.size(),
        static_cast<u32>(matching_rule_set.author_contexts.size()),
        abstract_element.pseudo_element().has_value(),
        cascade_custom_properties,
        abstract_element.document().rust_custom_property_registry(),
        unset_value.ptr(),
        unset_value->rust_style_value_data(),
        &callbacks);

    for (auto layer_name_raw : leaked_layer_names)
        Utf16FlyString::unref_raw(layer_name_raw);
    for (auto custom_property_name_raw : leaked_custom_property_names)
        Utf16FlyString::unref_raw(custom_property_name_raw);

    // Transition declarations [css-transitions-1]
    // Note that we have to do these after finishing computing the style,
    // so they're not done here, but as the final step in compute_properties()

    return cascaded_properties;
}

NonnullRefPtr<StyleValue const> StyleComputer::get_non_animated_inherit_value(PropertyID property_id, DOM::AbstractElement abstract_element)
{
    auto parent_element = abstract_element.element_to_inherit_style_from();

    if (!parent_element.has_value() || !parent_element->computed_values())
        return property_initial_value(property_id);

    auto value = parent_element->computed_values()->computed_style_value_for_inheritance(property_id, ComputedValues::WithAnimationsApplied::No);
    VERIFY(value);

    return value.release_nonnull();
}

Optional<StyleComputer::AnimatedInheritValue> StyleComputer::get_animated_inherit_value(PropertyID property_id, DOM::AbstractElement abstract_element)
{
    auto parent_element = abstract_element.element_to_inherit_style_from();

    if (!parent_element.has_value() || !parent_element->computed_values())
        return {};

    auto const* animated_properties = parent_element->computed_values()->animated_properties();
    if (!animated_properties || !animated_properties->has_property(property_id))
        return {};

    if (auto animated_value = animated_properties->values().get(property_id); animated_value.has_value()) {
        return AnimatedInheritValue {
            .value = *animated_value.value(),
            .is_result_of_transition = animated_properties->is_property_result_of_transition(property_id)
                ? AnimatedPropertyResultOfTransition::Yes
                : AnimatedPropertyResultOfTransition::No
        };
    }

    return {};
}

Length::FontMetrics StyleComputer::calculate_root_element_font_metrics(ComputedProperties const& style) const
{
    auto const& root_value = style.property(CSS::PropertyID::FontSize);

    auto font_pixel_metrics = style.first_available_computed_font(document().font_computer())->pixel_metrics();
    Length::FontMetrics font_metrics { m_default_font_metrics.font_size, font_pixel_metrics, InitialValues::line_height() };
    font_metrics.font_size = root_value.as_length().length().to_px(viewport_rect(), font_metrics, font_metrics);
    font_metrics.line_height = style.line_height(document().font_computer());

    return font_metrics;
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

void StyleComputer::compute_property_values(ComputedProperties::Builder& builder, Optional<DOM::AbstractElement> abstract_element) const
{
    VERIFY(computation_context_cache_is_empty());
    auto& style = builder.style();
    // NOTE: This doesn't necessarily return the specified value if we have already computed this property but that
    //       doesn't matter as a computed value is always valid as a specified value.
    Function<NonnullRefPtr<StyleValue const>(PropertyID)> const get_property_specified_value = [&](auto property_id) -> NonnullRefPtr<StyleValue const> {
        return style.property(property_id);
    };

    auto device_pixels_per_css_pixel = m_document->page().client().device_pixels_per_css_pixel();
    for (auto const& property_id : property_computation_order()) {
        auto const& computation_context = get_computation_context_for_property(property_id, style, abstract_element);

        auto const& specified_value = style.property(property_id, ComputedProperties::WithAnimationsApplied::No);

        computation_context.reset_viewport_metric_dependency_tracking();
        auto const& computed_value = compute_value_of_property(property_id, specified_value, get_property_specified_value, computation_context, device_pixels_per_css_pixel);
        if (computation_context.depends_on_viewport_metrics()) {
            builder.set_depends_on_viewport_metrics();
            if (property_affects_font_metrics(property_id))
                builder.set_font_metrics_depend_on_viewport_metrics();
        }

        builder.set_property_without_modifying_flags(property_id, computed_value);
    }

    clear_computation_context_caches();

    if (abstract_element.has_value() && is<HTML::HTMLHtmlElement>(abstract_element->element())) {
        m_root_element_font_metrics = calculate_root_element_font_metrics(style);
        m_root_element_font_metrics_depend_on_viewport_metrics = builder.font_metrics_depend_on_viewport_metrics();
    }
}

ComputationContext StyleComputer::make_computation_context_for_property(PropertyID property_id, ComputedProperties const& style, Optional<DOM::AbstractElement> abstract_element) const
{
    auto subject_inline_axis_is_horizontal = [&]() {
        if (!abstract_element.has_value())
            return true;
        if (auto computed_values = abstract_element->computed_values(); computed_values)
            return computed_values->writing_mode() == WritingMode::HorizontalTb;
        if (auto inheritance_parent = abstract_element->element_to_inherit_style_from(); inheritance_parent.has_value() && inheritance_parent->computed_values())
            return inheritance_parent->computed_values()->writing_mode() == WritingMode::HorizontalTb;
        return true;
    }();

    switch (property_id) {
    // FIXME: While `color-scheme` doesn't actually require a computation context (since it only takes keyword values)
    //        we still try to generate one in `compute_property_values()` and since we need `color-scheme` to be
    //        computed before creating a generic computation context we use the font one instead.
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
        auto length_resolution_context = inheritance_parent.has_value()
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
            inheritance_parent.has_value() ? inheritance_parent->computed_values()->line_height() : InitialValues::line_height()
        };

        return {
            .length_resolution_context = {
                .viewport_rect = viewport_rect(),
                .font_metrics = line_height_font_metrics,
                .root_font_metrics = abstract_element.has_value() && abstract_element->element().is_html_html_element()
                    ? line_height_font_metrics
                    : m_root_element_font_metrics,
                .font_metrics_depend_on_viewport_metrics = style.font_metrics_depend_on_viewport_metrics(),
                .root_font_metrics_depend_on_viewport_metrics = abstract_element.has_value() && abstract_element->element().is_html_html_element()
                    ? style.font_metrics_depend_on_viewport_metrics()
                    : m_root_element_font_metrics_depend_on_viewport_metrics,
                .subject_inline_axis_is_horizontal = subject_inline_axis_is_horizontal,
                .subject_element = abstract_element.has_value() ? &abstract_element->element() : nullptr,
            },
            .abstract_element = abstract_element
        };
    }
    default: {
        return {
            .length_resolution_context = {
                .viewport_rect = viewport_rect(),
                .font_metrics = {
                    style.font_size(),
                    style.first_available_computed_font(document().font_computer())->pixel_metrics(),
                    style.line_height(document().font_computer()) },
                .root_font_metrics = m_root_element_font_metrics,
                .font_metrics_depend_on_viewport_metrics = style.font_metrics_depend_on_viewport_metrics(),
                .root_font_metrics_depend_on_viewport_metrics = abstract_element.has_value() && abstract_element->element().is_html_html_element() ? style.font_metrics_depend_on_viewport_metrics() : m_root_element_font_metrics_depend_on_viewport_metrics,
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

ComputationContext const& StyleComputer::get_computation_context_for_property(PropertyID property_id, ComputedProperties const& style, Optional<DOM::AbstractElement> abstract_element) const
{
    switch (property_id) {
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
    case PropertyID::TextRendering:
        if (!m_cached_font_computation_context.has_value())
            m_cached_font_computation_context = make_computation_context_for_property(property_id, style, abstract_element);
        return m_cached_font_computation_context.value();
    case PropertyID::LineHeight:
        if (!m_cached_line_height_computation_context.has_value())
            m_cached_line_height_computation_context = make_computation_context_for_property(property_id, style, abstract_element);
        return m_cached_line_height_computation_context.value();
    default:
        if (!m_cached_generic_computation_context.has_value())
            m_cached_generic_computation_context = make_computation_context_for_property(property_id, style, abstract_element);
        return m_cached_generic_computation_context.value();
    }
}

void StyleComputer::resolve_effective_overflow_values(ComputedProperties::Builder& builder) const
{
    auto& style = builder.style();
    // The css-overflow-3 rule pairing the two axes lives in the Rust style computation core.
    auto effective_overflow = ComputedValuesFFI::rust_resolve_effective_overflow_keywords(
        to_underlying(style.property(PropertyID::OverflowX).to_keyword()),
        to_underlying(style.property(PropertyID::OverflowY).to_keyword()));
    if (effective_overflow.changed_x)
        builder.set_property(PropertyID::OverflowX, KeywordStyleValue::create(static_cast<Keyword>(effective_overflow.x_keyword)));
    if (effective_overflow.changed_y)
        builder.set_property(PropertyID::OverflowY, KeywordStyleValue::create(static_cast<Keyword>(effective_overflow.y_keyword)));
}

static void compute_text_align(ComputedProperties::Builder& builder, DOM::AbstractElement abstract_element)
{
    auto& style = builder.style();
    auto text_align_keyword = style.property(PropertyID::TextAlign).to_keyword();

    // NB: Only these two keywords trigger an adjustment in the Rust decision below; the early
    //     return avoids fetching the parent's computed values for every element.
    if (text_align_keyword != Keyword::MatchParent && text_align_keyword != Keyword::LibwebInheritOrCenter)
        return;

    auto const parent = abstract_element.element_to_inherit_style_from();
    bool has_parent_with_computed_values = parent.has_value() && parent->computed_values();
    u16 parent_text_align = 0;
    bool parent_direction_is_ltr = true;
    if (has_parent_with_computed_values) {
        auto const& parent_values = *parent->computed_values();
        parent_text_align = to_underlying(to_keyword(parent_values.text_align()));
        parent_direction_is_ltr = parent_values.direction() == Direction::Ltr;
    }

    // The adjustment decision lives in the Rust style computation core.
    auto adjustment = ComputedValuesFFI::rust_compute_text_align(
        to_underlying(text_align_keyword),
        abstract_element.element().local_name() == HTML::TagNames::th,
        has_parent_with_computed_values,
        parent_text_align,
        parent_direction_is_ltr);
    if (adjustment.changed) {
        builder.set_property(PropertyID::TextAlign, KeywordStyleValue::create(static_cast<Keyword>(adjustment.keyword)),
            adjustment.inherited ? ComputedProperties::Inherited::Yes : ComputedProperties::Inherited::No);
    }
}

static ComputedValuesFFI::FfiDisplay to_ffi_display(Display const& display)
{
    // The Rust mirror uses the same tag discriminants as Display::Type.
    static_assert(to_underlying(Display::Type::OutsideAndInside) == 0);
    static_assert(to_underlying(Display::Type::Internal) == 1);
    static_assert(to_underlying(Display::Type::Box) == 2);

    ComputedValuesFFI::FfiDisplay result {};
    result.tag = to_underlying(display.type());
    switch (display.type()) {
    case Display::Type::OutsideAndInside:
        result.outside = to_underlying(display.outside());
        result.inside = to_underlying(display.inside());
        result.list_item = display.is_list_item();
        break;
    case Display::Type::Internal:
        result.internal = to_underlying(display.internal());
        break;
    case Display::Type::Box:
        result.box_value = display.is_none() ? to_underlying(DisplayBox::None) : to_underlying(DisplayBox::Contents);
        break;
    }
    return result;
}

static Display from_ffi_display(ComputedValuesFFI::FfiDisplay const& display)
{
    switch (static_cast<Display::Type>(display.tag)) {
    case Display::Type::OutsideAndInside:
        return Display { static_cast<DisplayOutside>(display.outside), static_cast<DisplayInside>(display.inside), display.list_item ? Display::ListItem::Yes : Display::ListItem::No };
    case Display::Type::Internal:
        return Display { static_cast<DisplayInternal>(display.internal) };
    case Display::Type::Box:
        return Display { static_cast<DisplayBox>(display.box_value) };
    }
    VERIFY_NOT_REACHED();
}

static ComputedValuesFFI::FfiDisplay decode_ffi_display(u32 encoded)
{
    ComputedValuesFFI::FfiDisplay display {};
    display.tag = encoded & 0xff;
    auto first = static_cast<u8>((encoded >> 8) & 0xff);
    auto second = static_cast<u8>((encoded >> 16) & 0xff);
    auto third = static_cast<u8>((encoded >> 24) & 0xff);
    switch (static_cast<Display::Type>(display.tag)) {
    case Display::Type::OutsideAndInside:
        display.outside = first;
        display.inside = second;
        display.list_item = third != 0;
        break;
    case Display::Type::Internal:
        display.internal = first;
        break;
    case Display::Type::Box:
        display.box_value = first;
        break;
    }
    return display;
}

static ComputedValuesFFI::FfiBoxTypeTransformationInput make_box_type_transformation_input(
    DOM::AbstractElement abstract_element, Display display, Keyword position, Keyword float_value)
{
    auto& element = abstract_element.element();

    // NOTE: If we're computing style for a pseudo-element, the effective parent will be the originating element itself, not its parent.
    auto parent = abstract_element.element_to_inherit_style_from();

    // Climb out of `display: contents` context.
    while (parent.has_value() && parent->computed_values() && parent->computed_values()->display().is_contents())
        parent = parent->element_to_inherit_style_from();

    bool has_parent_display = parent.has_value() && parent->computed_values();
    bool is_html_element = element.namespace_uri() == Namespace::HTML;
    bool should_adjust_element = !abstract_element.pseudo_element().has_value();
    auto local_name = element.local_name();

    bool input_allows_adjustment = false;
    bool input_is_single_line = false;
    if (is<HTML::HTMLInputElement>(element)) {
        auto const& input = static_cast<HTML::HTMLInputElement const&>(element);
        input_allows_adjustment = !first_is_one_of(
            input.type_state(),
            HTML::HTMLInputElement::TypeAttributeState::Hidden,
            HTML::HTMLInputElement::TypeAttributeState::SubmitButton,
            HTML::HTMLInputElement::TypeAttributeState::Button,
            HTML::HTMLInputElement::TypeAttributeState::ResetButton,
            HTML::HTMLInputElement::TypeAttributeState::ImageButton,
            HTML::HTMLInputElement::TypeAttributeState::Checkbox,
            HTML::HTMLInputElement::TypeAttributeState::RadioButton);
        input_is_single_line = input_allows_adjustment && input.is_single_line();
    }

    bool force_position_static = false;
    if (element.namespace_uri() == Namespace::SVG) {
        force_position_static = true;
        if (local_name == "svg"sv) {
            force_position_static = false;
            for (auto ancestor = element.parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (ancestor->namespace_uri() == Namespace::SVG && ancestor->local_name() == "foreignObject"sv)
                    break;
                if (ancestor->namespace_uri() == Namespace::SVG && ancestor->local_name() == "svg"sv) {
                    force_position_static = true;
                    break;
                }
            }
        }
    }

    bool force_symbol_display_inline = false;
    if (element.namespace_uri() == Namespace::SVG && local_name == "symbol"sv) {
        if (auto* shadow_root = as_if<DOM::ShadowRoot>(element.parent())) {
            auto* host = shadow_root->host();
            force_symbol_display_inline = host->namespace_uri() == Namespace::SVG && host->local_name() == "use"sv;
        }
    }

    return {
        .display = to_ffi_display(display),
        .position = to_underlying(position),
        .float_value = to_underlying(float_value),
        .is_br_element = !abstract_element.pseudo_element().has_value() && is<HTML::HTMLBRElement>(element),
        .is_document_element = element.is_document_element(),
        .is_mathml_element = element.namespace_uri() == Namespace::MathML,
        .is_mathml_mtable = element.tag_name().equals_ignoring_ascii_case("mtable"sv),
        .is_mathml_mtr = element.tag_name().equals_ignoring_ascii_case("mtr"sv),
        .is_mathml_mtd = element.tag_name().equals_ignoring_ascii_case("mtd"sv),
        .has_parent_display = has_parent_display,
        .parent_display = has_parent_display ? to_ffi_display(parent->computed_values()->display()) : ComputedValuesFFI::FfiDisplay {},
        .is_wbr_element = should_adjust_element && is_html_element && local_name == HTML::TagNames::wbr,
        .disallow_display_contents = should_adjust_element && (input_allows_adjustment || (is_html_element && first_is_one_of(local_name, HTML::TagNames::textarea, HTML::TagNames::audio, HTML::TagNames::video, HTML::TagNames::canvas, HTML::TagNames::object, HTML::TagNames::iframe, HTML::TagNames::progress, HTML::TagNames::embed, HTML::TagNames::frame, HTML::TagNames::meter, HTML::TagNames::frameset, HTML::TagNames::img))),
        .rewrite_inline_flow = should_adjust_element && (input_allows_adjustment || (is_html_element && first_is_one_of(local_name, HTML::TagNames::textarea, HTML::TagNames::audio, HTML::TagNames::video, HTML::TagNames::select))),
        .is_button_element = should_adjust_element && is_html_element && local_name == HTML::TagNames::button,
        .force_line_height_normal = should_adjust_element && is_html_element && local_name == HTML::TagNames::select,
        .check_input_line_height = should_adjust_element && input_is_single_line,
        .hide_audio_without_controls = should_adjust_element && is_html_element && local_name == HTML::TagNames::audio && !element.has_attribute(HTML::AttributeNames::controls),
        .is_table_element = should_adjust_element && is_html_element && local_name == HTML::TagNames::table,
        .force_position_static = should_adjust_element && force_position_static,
        .force_symbol_display_inline = should_adjust_element && force_symbol_display_inline,
    };
}

static void apply_box_type_transformation(ComputedProperties::Builder& builder, ComputedValuesFFI::FfiDisplay const& display_before, ComputedValuesFFI::FfiBoxTypeTransformation const& transformation)
{
    builder.set_display_before_box_type_transformation(from_ffi_display(display_before));
    if (transformation.set_float_none)
        builder.set_property(PropertyID::Float, KeywordStyleValue::create(Keyword::None));
    if (transformation.changed_display)
        builder.set_property(PropertyID::Display, DisplayStyleValue::create(from_ffi_display(transformation.display)));
}

static ComputedValuesFFI::FfiInputLineHeightMetrics input_line_height_metrics(ComputedProperties::Builder& builder, DOM::AbstractElement abstract_element, bool should_measure)
{
    ComputedValuesFFI::FfiInputLineHeightMetrics line_height_metrics {};
    if (should_measure) {
        line_height_metrics.current_line_height = builder.line_height(abstract_element.element().document().font_computer()).to_double();
        line_height_metrics.minimum_line_height = ComputedProperties::normal_line_height(builder.first_available_computed_font(abstract_element.element().document().font_computer())->pixel_metrics()).to_double();
    }
    return line_height_metrics;
}

// https://drafts.csswg.org/css-display/#transformations
void StyleComputer::transform_box_type_if_needed(ComputedProperties::Builder& builder, DOM::AbstractElement abstract_element) const
{
    auto& style = builder.style();
    auto input = make_box_type_transformation_input(
        abstract_element,
        style.display(),
        style.property(PropertyID::Position).to_keyword(),
        style.property(PropertyID::Float).to_keyword());
    auto transformation = ComputedValuesFFI::rust_transform_box_type(&input);
    apply_box_type_transformation(builder, input.display, transformation);
}

NonnullRefPtr<ComputedValues const> StyleComputer::create_document_style() const
{
    auto builder = CSS::ComputedProperties::create_builder();
    for (auto i = to_underlying(CSS::first_longhand_property_id); i <= to_underlying(CSS::last_longhand_property_id); ++i) {
        auto property_id = static_cast<PropertyID>(i);
        builder.set_property(property_id, property_initial_value(property_id));
    }

    compute_property_values(builder, {});
    builder.set_property(CSS::PropertyID::Width, CSS::LengthStyleValue::create(CSS::Length::make_px(viewport_rect().width())));
    builder.set_property(CSS::PropertyID::Height, CSS::LengthStyleValue::create(CSS::Length::make_px(viewport_rect().height())));
    builder.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::Block)));
    auto computed_properties = CSS::ComputedProperties::create(move(builder));
    CSS::ColorResolutionContext color_resolution_context {
        .color_scheme = document().page().preferred_color_scheme(),
        .current_color = CSS::InitialValues::color(),
        .current_color_style_value = &computed_properties->property(PropertyID::Color),
        .calculation_resolution_context = { .length_resolution_context = CSS::Length::ResolutionContext::for_document(document()) },
    };
    auto computed_values = CSS::ComputedValues::create(*computed_properties, document(), document().style_scope(), move(color_resolution_context));
    return computed_values;
}

NonnullRefPtr<ComputedValues const> StyleComputer::compute_style(DOM::AbstractElement abstract_element, Optional<bool&> did_change_custom_properties) const
{
    auto& style_scope = abstract_element.style_scope();
    auto computed_properties = compute_style_impl(abstract_element, ComputeStyleMode::Normal, did_change_custom_properties, style_scope, IncludeInlineStyle::Yes);
    VERIFY(computed_properties);
    return build_computed_values(*computed_properties, abstract_element, style_scope);
}

NonnullRefPtr<ComputedProperties> StyleComputer::compute_properties_without_inline_style(DOM::AbstractElement abstract_element) const
{
    // Computing custom properties normally caches them on the element. Preserve the real cache while asking the
    // cascade what this element would look like without its inline declaration.
    auto custom_property_data = abstract_element.custom_property_data();
    auto needs_style_update = abstract_element.element().needs_style_update();
    ScopeGuard restore_custom_property_data = [&] {
        abstract_element.set_custom_property_data(move(custom_property_data));
        abstract_element.element().set_needs_style_update(needs_style_update);
    };

    auto& style_scope = abstract_element.style_scope();
    auto computed_properties = compute_style_impl(abstract_element, ComputeStyleMode::Normal, {}, style_scope, IncludeInlineStyle::No);
    VERIFY(computed_properties);
    return computed_properties.release_nonnull();
}

NonnullRefPtr<ComputedValues const> StyleComputer::compute_style_with_seeded_ancestors(DOM::AbstractElement abstract_element)
{
    auto const first_ancestor = [&] -> GC::Ptr<DOM::Element const> {
        if (abstract_element.pseudo_element().has_value())
            return &abstract_element.element();
        return abstract_element.element().parent_or_shadow_host_element();
    }();

    for (auto parent = first_ancestor; parent; parent = parent->parent_or_shadow_host_element()) {
        push_ancestor(*parent);
    }

    ScopeGuard pop_ancestors = [&] {
        for (auto parent = first_ancestor; parent; parent = parent->parent_or_shadow_host_element())
            pop_ancestor(*parent);
    };

    return compute_style(abstract_element);
}

RefPtr<ComputedValues const> StyleComputer::compute_pseudo_element_style_if_needed(DOM::AbstractElement abstract_element, Optional<bool&> did_change_custom_properties) const
{
    auto& style_scope = abstract_element.style_scope();
    auto computed_properties = compute_style_impl(abstract_element, ComputeStyleMode::CreatePseudoElementStyleIfNeeded, did_change_custom_properties, style_scope, IncludeInlineStyle::Yes);
    if (computed_properties) {
        return build_computed_values(*computed_properties, abstract_element, style_scope);
    }
    return {};
}

NonnullRefPtr<ComputedValues const> StyleComputer::build_computed_values(ComputedProperties& computed_properties, DOM::AbstractElement abstract_element, StyleScope const& style_scope) const
{
    VERIFY(computation_context_cache_is_empty());
    ScopeGuard clear_computation_context_cache = [&] { clear_computation_context_caches(); };

    auto const& computation_context = get_computation_context_for_property(PropertyID::Color, computed_properties, abstract_element);
    ColorResolutionContext color_resolution_context {
        .color_scheme = computation_context.color_scheme,
        .current_color = InitialValues::color(),
        .current_color_style_value = &computed_properties.property(PropertyID::Color),
        .calculation_resolution_context = { .length_resolution_context = computation_context.length_resolution_context },
    };
    // NB: Sharing group payloads with the parent costs almost nothing for groups that already
    //     share the leaked defaults (a pointer compare each) and lets children reference their
    //     parent's payloads for everything they inherit unchanged, including values that can
    //     never match the process-wide defaults, like scope-resolved counter styles.
    auto adopt_group_payloads_from_parent = [&](ComputedValues const& style) {
        if (auto parent = abstract_element.element_to_inherit_style_from(); parent.has_value()) {
            if (auto parent_values = parent->computed_values())
                style.adopt_identical_group_payloads(*parent_values);
        }
    };

    auto const inherit_parent = abstract_element.element_to_inherit_style_from();
    auto const* inherit_parent_values = inherit_parent.has_value() ? inherit_parent->computed_values() : nullptr;

    auto base_properties = computed_properties.copy_without_animations();
    auto base_values = ComputedValues::create(*base_properties, document(), style_scope, color_resolution_context, inherit_parent_values);
    auto animated_properties = computed_properties.animated_properties_snapshot();
    if (!animated_properties || animated_properties->is_empty()) {
        adopt_group_payloads_from_parent(*base_values);
        return base_values;
    }

    ComputedValues::Builder builder(*ComputedValues::create(computed_properties, document(), style_scope, move(color_resolution_context), inherit_parent_values));
    builder->set_base_values(move(base_values));
    builder->set_animated_properties(animated_properties.ptr());
    auto style = move(builder).build();
    adopt_group_payloads_from_parent(*style);
    return style;
}

NonnullRefPtr<ComputedProperties> StyleComputer::reconstruct_computed_properties(ComputedValues const& computed_values) const
{
    auto builder = ComputedProperties::create_builder();
    auto const& base_values = computed_values.base_values();

    for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
        auto property_id = static_cast<PropertyID>(i);
        auto value = base_values.computed_style_value_for_inheritance(property_id);
        VERIFY(value);
        builder.set_property(
            property_id,
            value.release_nonnull(),
            computed_values.is_property_inherited(property_id) ? ComputedProperties::Inherited::Yes : ComputedProperties::Inherited::No,
            computed_values.is_property_important(property_id) ? Important::Yes : Important::No);
    }

    builder.set_display_before_box_type_transformation(base_values.display_before_box_type_transformation());
    if (computed_values.depends_on_viewport_metrics())
        builder.set_depends_on_viewport_metrics();
    if (computed_values.font_metrics_depend_on_viewport_metrics())
        builder.set_font_metrics_depend_on_viewport_metrics();
    if (computed_values.in_display_none_subtree())
        builder.set_in_display_none_subtree();
    for (auto i = 0; i < to_underlying(PseudoElement::KnownPseudoElementCount); ++i) {
        auto pseudo_element = static_cast<PseudoElement>(i);
        if (computed_values.has_pseudo_element_style(pseudo_element))
            builder.set_has_pseudo_element_style(pseudo_element);
    }
    for (auto const& [property_id, value] : computed_values.inheritance_dependent_specified_values())
        builder.add_inheritance_dependent_specified_value(property_id, value);
    if (auto raw_cascaded_font_size = computed_values.raw_cascaded_font_size())
        builder.set_raw_cascaded_font_size(raw_cascaded_font_size.release_nonnull());

    auto style = ComputedProperties::create(move(builder));
    if (auto animated_properties = computed_values.animated_properties()) {
        for (auto const& [property_id, value] : animated_properties->values()) {
            style->set_animated_property(
                Badge<StyleComputer> {}, property_id, value,
                animated_properties->is_property_result_of_transition(property_id) ? AnimatedPropertyResultOfTransition::Yes : AnimatedPropertyResultOfTransition::No,
                animated_properties->is_property_inherited(property_id) ? ComputedProperties::Inherited::Yes : ComputedProperties::Inherited::No);
        }
    }
    return style;
}

RefPtr<ComputedProperties> StyleComputer::compute_style_impl(DOM::AbstractElement abstract_element, ComputeStyleMode mode, Optional<bool&> did_change_custom_properties, StyleScope const& style_scope, IncludeInlineStyle include_inline_style) const
{
    style_scope.build_rule_cache_if_needed();

    // Special path for elements that represent a pseudo-element in some element's internal shadow tree.
    // FirstLetter is excluded so that ::first-letter rules can match against such elements normally.
    if (abstract_element.element().associated_shadow_host_pseudo_element().has_value() && abstract_element.pseudo_element() != CSS::PseudoElement::FirstLetter) {
        auto& element = abstract_element.element();
        auto& host_element = *element.root().parent_or_shadow_host_element();

        // We have to decide where to inherit from. If the pseudo-element has a parent element,
        // we inherit from that. Otherwise, we inherit from the host element in the light DOM.
        DOM::AbstractElement abstract_element_for_pseudo_element { host_element, element.associated_shadow_host_pseudo_element() };
        if (auto parent_element = element.parent_element())
            abstract_element_for_pseudo_element.set_inheritance_override(*parent_element);
        else
            abstract_element_for_pseudo_element.set_inheritance_override(host_element);

        auto& inherited_style_scope = abstract_element_for_pseudo_element.style_scope();
        auto inherited_pseudo_element_style = compute_style_impl(abstract_element_for_pseudo_element, ComputeStyleMode::Normal, {}, inherited_style_scope, include_inline_style);
        VERIFY(inherited_pseudo_element_style);
        auto builder = ComputedProperties::create_builder_with_base_values_from(*inherited_pseudo_element_style);

        abstract_element.element().adjust_computed_style(builder);
        return ComputedProperties::create(move(builder));
    }

    ScopeGuard guard { [&abstract_element]() { abstract_element.element().set_needs_style_update(false); } };

    // 1. Perform the cascade. This produces the "specified style"
    bool did_match_any_pseudo_element_rules = false;
    auto matching_rule_set = build_matching_rule_set(abstract_element, did_match_any_pseudo_element_rules, mode);

    if (mode == ComputeStyleMode::CreatePseudoElementStyleIfNeeded) {
        // NOTE: If we're computing style for a pseudo-element, we look for a number of reasons to bail early.

        // Some pseudo-elements are generated regardless of CSS rules, so we need to compute their styles even when no
        // rules matched.
        auto has_implicit_style = ComputedValuesFFI::rust_pseudo_element_has_implicit_style(to_underlying(*abstract_element.pseudo_element()));

        // Bail if no pseudo-element rules matched. Clear any stale custom property data so
        // getComputedStyle() doesn't return values from a previous match.
        if (!did_match_any_pseudo_element_rules && !has_implicit_style) {
            abstract_element.set_custom_property_data(nullptr);
            return {};
        }
    }

    auto old_custom_property_data = abstract_element.custom_property_data();

    auto cascaded_properties = compute_cascaded_values(abstract_element, matching_rule_set, include_inline_style);

    if (mode == ComputeStyleMode::CreatePseudoElementStyleIfNeeded) {
        // Bail if no pseudo-element would be generated due to...
        // - content: none
        // - content: normal (for ::before and ::after)
        auto content_value = cascaded_properties->property(CSS::PropertyID::Content);
        if (ComputedValuesFFI::rust_pseudo_element_content_bails(content_value ? content_value->rust_style_value_data() : nullptr, to_underlying(*abstract_element.pseudo_element())))
            return {};
    }

    auto computed_properties = compute_properties(abstract_element, cascaded_properties, matching_rule_set.matching_pseudo_element_styles);

    if (did_change_custom_properties.has_value()) {
        auto new_custom_property_data = abstract_element.custom_property_data();
        if (old_custom_property_data.ptr() != new_custom_property_data.ptr()) {
            static NeverDestroyed<OrderedHashMap<Utf16FlyString, StyleProperty>> empty_own_values;
            auto const& old_own = old_custom_property_data ? old_custom_property_data->own_values() : *empty_own_values;
            auto const& new_own = new_custom_property_data ? new_custom_property_data->own_values() : *empty_own_values;
            if (old_own != new_own)
                *did_change_custom_properties = true;
        }
    }

    return computed_properties;
}

static bool is_monospace(StyleValue const& value)
{
    return ComputedValuesFFI::rust_font_family_is_monospace(
        value.rust_style_value_data(),
        [](void const* shell) -> void const* { return static_cast<StyleValue const*>(shell)->rust_style_value_data(); });
}

// HACK: This function implements time-travelling inheritance for the font-size property
//       in situations where the cascade ended up with `font-family: monospace`.
//       In such cases, other browsers will magically change the meaning of keyword font sizes
//       *even in earlier stages of the cascade!!* to be relative to the default monospace font size (13px)
//       instead of the default font size (16px).
//       See this blog post for a lot more details about this weirdness:
//       https://manishearth.github.io/blog/2017/08/10/font-size-an-unexpectedly-complex-css-property/
RefPtr<StyleValue const> StyleComputer::recascade_font_size_if_needed(DOM::AbstractElement abstract_element, CascadedProperties& cascaded_properties, bool& depends_on_viewport_metrics) const
{
    // Check for `font-family: monospace`. Note that `font-family: monospace, AnythingElse` does not trigger this path.
    // Some CSS frameworks use `font-family: monospace, monospace` to work around this behavior.
    auto font_family_value = cascaded_properties.property(CSS::PropertyID::FontFamily);
    if (!font_family_value || !is_monospace(*font_family_value))
        return nullptr;

    // FIXME: This should be configurable.
    constexpr CSSPixels default_monospace_font_size_in_px = 13;
    static auto const& monospace_font_family_name = *new String(Platform::FontPlugin::the().generic_font_name(Platform::GenericFont::Monospace, 400, 0));
    static auto const& monospace_font = Gfx::FontDatabase::the().get(monospace_font_family_name, default_monospace_font_size_in_px * 0.75f, 400, Gfx::FontWidth::Normal, 0).release_nonnull().leak_ref();

    // Reconstruct the line of ancestor elements we need to inherit style from, and then do the cascade again
    // but only for the font-size property.
    GC::ConservativeVector<DOM::AbstractElement> ancestors;
    for (auto ancestor = abstract_element.element_to_inherit_style_from(); ancestor.has_value(); ancestor = ancestor->element_to_inherit_style_from())
        ancestors.append(*ancestor);

    NonnullRefPtr<StyleValue const> new_font_size = CSS::LengthStyleValue::create(CSS::Length::make_px(default_monospace_font_size_in_px));
    CSSPixels current_size_in_px = default_monospace_font_size_in_px;
    bool current_size_depends_on_viewport_metrics = false;

    for (auto& ancestor : ancestors.in_reverse()) {
        auto ancestor_computed_values = ancestor.computed_values();
        if (!ancestor_computed_values)
            continue;
        auto font_size_value = ancestor_computed_values->raw_cascaded_font_size();

        if (!font_size_value)
            continue;

        // The per-value step lives in the Rust style computation core. A length value needs a
        // resolution context, which is built lazily on request since it involves font work the
        // other value types never need.
        auto step = ComputedValuesFFI::rust_recascade_font_size_step(
            font_size_value->rust_style_value_data(),
            current_size_in_px.raw_value(),
            current_size_depends_on_viewport_metrics,
            default_monospace_font_size_in_px.raw_value(),
            nullptr);

        if (step.action == ComputedValuesFFI::FontSizeRecascadeAction::NeedsLengthResolution) {
            bool inherited_font_metrics_depend_on_viewport_metrics = false;
            auto inherited_line_height = ancestor.element_to_inherit_style_from()
                                             .map([&](auto&& parent_element) {
                                                 inherited_font_metrics_depend_on_viewport_metrics = parent_element.computed_values()->font_metrics_depend_on_viewport_metrics();
                                                 return parent_element.computed_values()->line_height();
                                             })
                                             .value_or(InitialValues::line_height());

            bool did_resolve_viewport_relative_length = false;
            Length::ResolutionContext resolution_context {
                .viewport_rect = viewport_rect(),
                .font_metrics = { current_size_in_px, monospace_font.with_size(current_size_in_px * 0.75f)->pixel_metrics(), inherited_line_height },
                .root_font_metrics = m_root_element_font_metrics,
                .font_metrics_depend_on_viewport_metrics = current_size_depends_on_viewport_metrics || inherited_font_metrics_depend_on_viewport_metrics,
                .root_font_metrics_depend_on_viewport_metrics = m_root_element_font_metrics_depend_on_viewport_metrics,
                .subject_inline_axis_is_horizontal = ancestor.computed_values()->writing_mode() == WritingMode::HorizontalTb,
                .subject_element = &ancestor.element(),
            };
            auto ffi_resolution_context = to_ffi_length_resolution_context(resolution_context);
            step = ComputedValuesFFI::rust_recascade_font_size_step(
                font_size_value->rust_style_value_data(),
                current_size_in_px.raw_value(),
                current_size_depends_on_viewport_metrics,
                default_monospace_font_size_in_px.raw_value(),
                &ffi_resolution_context);

            if (step.action == ComputedValuesFFI::FontSizeRecascadeAction::NeedsLengthResolution) {
                // A length unit the core cannot resolve; resolve it here instead.
                VERIFY(font_size_value->is_length());
                resolution_context.set_did_resolve_viewport_relative_length(did_resolve_viewport_relative_length);
                current_size_in_px = font_size_value->as_length().length().to_px(resolution_context);
                current_size_depends_on_viewport_metrics = did_resolve_viewport_relative_length;
                continue;
            }
        }

        switch (step.action) {
        case ComputedValuesFFI::FontSizeRecascadeAction::Unchanged:
            break;
        case ComputedValuesFFI::FontSizeRecascadeAction::Set:
            current_size_in_px = CSSPixels::from_raw(step.new_size_raw);
            current_size_depends_on_viewport_metrics = step.depends_on_viewport_metrics;
            break;
        case ComputedValuesFFI::FontSizeRecascadeAction::CalcSkipped:
            dbgln("FIXME: Support calc() when time-traveling for monospace font-size");
            break;
        case ComputedValuesFFI::FontSizeRecascadeAction::NeedsLengthResolution:
            VERIFY_NOT_REACHED();
        }
    };

    depends_on_viewport_metrics = current_size_depends_on_viewport_metrics;
    return CSS::LengthStyleValue::create(CSS::Length::make_px(current_size_in_px));
}

void StyleComputer::ensure_style_metadata_tables_installed()
{
    // Marshal the generated logical-alias-to-physical mapping into the Rust style
    // computation core as a flat table, so the core uses exactly the C++ mapping.
    static bool const installed = [] {
        constexpr size_t writing_mode_count = 5;
        constexpr size_t direction_count = 2;
        Vector<u16> table;
        table.resize(number_of_longhand_properties * writing_mode_count * direction_count);
        size_t index = 0;
        for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
            auto property_id = static_cast<PropertyID>(i);
            for (size_t writing_mode = 0; writing_mode < writing_mode_count; ++writing_mode) {
                for (size_t direction = 0; direction < direction_count; ++direction) {
                    u16 physical = 0;
                    if (property_is_logical_alias(property_id))
                        physical = to_underlying(map_logical_alias_to_physical_property(property_id, LogicalAliasMappingContext { static_cast<WritingMode>(writing_mode), static_cast<Direction>(direction) }));
                    table[index++] = physical;
                }
            }
        }
        ComputedValuesFFI::rust_style_metadata_set_logical_alias_table(table.data(), table.size());

        Vector<u16> reverse_table;
        reverse_table.resize(number_of_longhand_properties * writing_mode_count * direction_count);
        index = 0;
        for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
            auto property_id = static_cast<PropertyID>(i);
            for (size_t writing_mode = 0; writing_mode < writing_mode_count; ++writing_mode) {
                for (size_t direction = 0; direction < direction_count; ++direction) {
                    u16 logical = 0;
                    if (!property_is_logical_alias(property_id)) {
                        auto mapped = map_physical_property_to_logical_alias(property_id, LogicalAliasMappingContext { static_cast<WritingMode>(writing_mode), static_cast<Direction>(direction) });
                        if (mapped != property_id)
                            logical = to_underlying(mapped);
                    }
                    reverse_table[index++] = logical;
                }
            }
        }
        ComputedValuesFFI::rust_style_metadata_set_physical_to_logical_table(reverse_table.data(), reverse_table.size());

        // Pin every longhand's initial value for the process lifetime and hand the
        // (shell, data) pointer pairs to the core, so initial-value selection never
        // crosses the FFI.
        static NeverDestroyed<Vector<NonnullRefPtr<StyleValue const>>> initial_value_pins;
        Vector<ComputedValuesFFI::FfiShellAndData> initial_value_entries;
        initial_value_entries.ensure_capacity(number_of_longhand_properties);
        for (auto i = to_underlying(first_longhand_property_id); i <= to_underlying(last_longhand_property_id); ++i) {
            auto initial_value = property_initial_value(static_cast<PropertyID>(i));
            initial_value_entries.unchecked_append({ initial_value.ptr(), initial_value->rust_style_value_data() });
            initial_value_pins->append(move(initial_value));
        }
        ComputedValuesFFI::rust_style_metadata_set_initial_value_table(initial_value_entries.data(), initial_value_entries.size());

        // Mark the color keywords, so the core knows which keyword values resolve to
        // something other than themselves at computed-value time.
        Vector<u64> color_keyword_words;
        color_keyword_words.resize((number_of_keywords + 63) / 64);
        for (size_t keyword = 0; keyword < number_of_keywords; ++keyword) {
            if (KeywordStyleValue::is_color(static_cast<Keyword>(keyword)))
                color_keyword_words[keyword / 64] |= 1ull << (keyword % 64);
        }
        ComputedValuesFFI::rust_style_metadata_set_color_keyword_bitmap(color_keyword_words.data(), color_keyword_words.size());
        return true;
    }();
    (void)installed;
}

NonnullRefPtr<ComputedProperties> StyleComputer::compute_properties(DOM::AbstractElement abstract_element, CascadedProperties& cascaded_properties, u64 matching_pseudo_element_styles) const
{
    ensure_style_metadata_tables_installed();
    VERIFY(computation_context_cache_is_empty());

    auto builder = CSS::ComputedProperties::create_builder();
    auto& computed_style = builder.style();
    builder.set_has_pseudo_element_styles(matching_pseudo_element_styles);

    bool recascaded_font_size_depends_on_viewport_metrics = false;
    auto new_font_size = recascade_font_size_if_needed(abstract_element, cascaded_properties, recascaded_font_size_depends_on_viewport_metrics);
    if (new_font_size) {
        builder.set_property(PropertyID::FontSize, *new_font_size, ComputedProperties::Inherited::No, Important::No);
        if (recascaded_font_size_depends_on_viewport_metrics) {
            builder.set_depends_on_viewport_metrics();
            builder.set_font_metrics_depend_on_viewport_metrics();
        }
    }

    auto const& computed_values_to_inherit_from = abstract_element.element_to_inherit_style_from().map([](auto const& element) { return element.computed_values(); }).value_or(nullptr);

    Function<NonnullRefPtr<StyleValue const>(PropertyID)> const get_property_specified_value = [&](auto property_id) -> NonnullRefPtr<StyleValue const> {
        return computed_style.property(property_id);
    };

    auto const device_pixels_per_css_pixel = m_document->page().client().device_pixels_per_css_pixel();

    Vector<u8> document_supported_color_scheme_codes;
    auto document_supported_color_schemes = document().supported_color_schemes();
    if (document_supported_color_schemes.has_value()) {
        document_supported_color_scheme_codes.ensure_capacity(document_supported_color_schemes->size());
        for (auto const& scheme : *document_supported_color_schemes)
            document_supported_color_scheme_codes.unchecked_append(to_underlying(preferred_color_scheme_from_string(scheme)));
    }
    ComputedValuesFFI::FfiEffectiveColorSchemeInput const effective_color_scheme_input {
        .preferred_color_scheme = static_cast<u8>(to_underlying(document().page().preferred_color_scheme())),
        .has_document_supported_schemes = document_supported_color_schemes.has_value(),
        .document_supported_scheme_codes = document_supported_color_scheme_codes.data(),
        .document_supported_scheme_count = document_supported_color_scheme_codes.size(),
    };
    builder.clear_effective_color_scheme();

    auto const compute_property = [&](PropertyID property_id, NonnullRefPtr<StyleValue const> const& style_value, bool& depends_on_viewport_metrics) {
        auto const& computation_context = get_computation_context_for_property(property_id, computed_style, abstract_element);
        computation_context.reset_viewport_metric_dependency_tracking();
        auto computed_value = compute_value_of_property(property_id, style_value, get_property_specified_value, computation_context, device_pixels_per_css_pixel);
        if (computation_context.depends_on_viewport_metrics())
            depends_on_viewport_metrics = true;
        return computed_value;
    };

    Optional<LogicalAliasMappingContext> logical_alias_mapping_context;
    auto const get_logical_alias_mapping_context = [&]() {
        if (!logical_alias_mapping_context.has_value())
            logical_alias_mapping_context = LogicalAliasMappingContext { computed_style.writing_mode(), computed_style.direction() };

        return *logical_alias_mapping_context;
    };

    // The parent's inheritable computed values, prepared once so the driver's inherit
    // path never crosses the FFI. Every entry is pinned for the duration of the drive.
    constexpr size_t inherited_longhand_count = to_underlying(last_inherited_property_id) - to_underlying(first_inherited_property_id) + 1;
    Array<RefPtr<StyleValue const>, inherited_longhand_count> parent_snapshot_pins;
    Array<ComputedValuesFFI::FfiShellAndData, inherited_longhand_count> parent_snapshot_entries {};
    Optional<ComputedValuesFFI::FfiParentSnapshot> parent_snapshot;
    if (computed_values_to_inherit_from) {
        for (size_t index = 0; index < inherited_longhand_count; ++index) {
            auto property_id = static_cast<PropertyID>(to_underlying(first_inherited_property_id) + index);
            auto value = computed_values_to_inherit_from->computed_style_value_for_inheritance(property_id, ComputedValues::WithAnimationsApplied::No);
            VERIFY(value);
            parent_snapshot_entries[index] = { value.ptr(), value->rust_style_value_data() };
            parent_snapshot_pins[index] = move(value);
        }
        parent_snapshot = ComputedValuesFFI::FfiParentSnapshot {
            .entries = parent_snapshot_entries.data(),
            .entry_count = inherited_longhand_count,
            .font_metrics_depend_on_viewport_metrics = computed_values_to_inherit_from->font_metrics_depend_on_viewport_metrics(),
        };
    }

    bool animation_values_applied = false;

    // Copies the parent's animated value when a longhand inherits, as its store is
    // applied, so later properties' computation contexts observe the animated value.
    // FIXME: Do we need to recompute animated inherited values?
    auto copy_animated_inherited_value = [&](PropertyID property_id, PropertyID inherited_property_id) {
        if (auto const* animated_properties = computed_values_to_inherit_from->animated_properties(); animated_properties && animated_properties->has_property(inherited_property_id)) {
            auto animated_value = animated_properties->values().get(inherited_property_id);
            VERIFY(animated_value.has_value());
            computed_style.set_animated_property(
                Badge<StyleComputer> {},
                property_id,
                *animated_value.value(),
                animated_properties->is_property_result_of_transition(inherited_property_id)
                    ? AnimatedPropertyResultOfTransition::Yes
                    : AnimatedPropertyResultOfTransition::No,
                ComputedProperties::Inherited::Yes);
            animation_values_applied = true;
        }
    };

    // Hands the driver the length resolution context a property's computation would
    // use, so plain lengths can absolutize natively; the driver caches one per
    // context kind, like get_computation_context_for_property does here.
    auto fetch_length_resolution_context = [&](PropertyID property_id) {
        auto const& computation_context = get_computation_context_for_property(property_id, computed_style, abstract_element);
        return to_ffi_length_resolution_context(computation_context.length_resolution_context);
    };

    // Applies a batch of store operations the driver queued, in property order,
    // replicating the per-property side effects and performing any computation the
    // driver deferred to C++.
    auto store_computed_batch = [&](ComputedValuesFFI::FfiComputedStoreEntry const* entries, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            auto const& entry = entries[i];
            auto property_id = static_cast<PropertyID>(entry.property_id);
            auto inherited_property_id = static_cast<PropertyID>(entry.inherited_property_id);
            auto const& value = *static_cast<StyleValue const*>(entry.shell);
            if (entry.inherited)
                copy_animated_inherited_value(property_id, inherited_property_id);
            // Store the resolved specified value for properties whose computation depends on
            // inherited info, so they can be re-resolved when an ancestor changes without
            // keeping CascadedProperties alive on the element.
            if (entry.inheritance_dependent)
                builder.add_inheritance_dependent_specified_value(property_id, value);
            switch (entry.computed_kind) {
            case ComputedValuesFFI::COMPUTED_KIND_COMPUTE_IN_CPP: {
                // NB: We compute using the inherited (physical) property to avoid having to add cases for all the
                //     logical alias properties in `compute_value_of_property`
                bool depends_on_viewport_metrics = false;
                auto computed_value = compute_property(inherited_property_id, value, depends_on_viewport_metrics);
                if (depends_on_viewport_metrics) {
                    builder.set_depends_on_viewport_metrics();
                    if (property_affects_font_metrics(inherited_property_id))
                        builder.set_font_metrics_depend_on_viewport_metrics();
                }
                builder.set_property_without_modifying_flags(property_id, move(computed_value));
                break;
            }
            case ComputedValuesFFI::COMPUTED_KIND_PX_LENGTH:
                builder.set_property_without_modifying_flags(property_id, LengthStyleValue::create(Length::make_px(entry.value)));
                break;
            case ComputedValuesFFI::COMPUTED_KIND_INTEGER:
                builder.set_property_without_modifying_flags(property_id, IntegerStyleValue::create(static_cast<i64>(entry.value)));
                break;
            case ComputedValuesFFI::COMPUTED_KIND_NUMBER:
                builder.set_property_without_modifying_flags(property_id, NumberStyleValue::create(entry.value));
                break;
            case ComputedValuesFFI::COMPUTED_KIND_PERCENTAGE:
                builder.set_property_without_modifying_flags(property_id, PercentageStyleValue::create(Percentage(entry.value)));
                break;
            case ComputedValuesFFI::COMPUTED_KIND_FONT_STYLE:
                builder.set_property_without_modifying_flags(property_id, FontStyleStyleValue::create(static_cast<FontStyleKeyword>(entry.value)));
                break;
            case ComputedValuesFFI::COMPUTED_KIND_KEYWORD:
                builder.set_property_without_modifying_flags(property_id, KeywordStyleValue::create(static_cast<Keyword>(static_cast<u16>(entry.value))));
                break;
            case ComputedValuesFFI::COMPUTED_KIND_DISPLAY:
                builder.set_property_without_modifying_flags(property_id, DisplayStyleValue::create(from_ffi_display(decode_ffi_display(static_cast<u32>(entry.value)))));
                break;
            case ComputedValuesFFI::COMPUTED_KIND_SUPERELLIPSE: {
                // NB: The round value is cached since it is the initial value of the corner-*-shape properties.
                if (entry.value == 1) {
                    static auto const& cached_round_value = SuperellipseStyleValue::create(NumberStyleValue::create(1)).leak_ref();
                    builder.set_property_without_modifying_flags(property_id, cached_round_value);
                } else {
                    builder.set_property_without_modifying_flags(property_id, SuperellipseStyleValue::create(NumberStyleValue::create(entry.value)));
                }
                break;
            }
            default:
                builder.set_property_without_modifying_flags(property_id, value);
                break;
            }
            if (property_id == PropertyID::ColorScheme && !builder.has_effective_color_scheme()) {
                auto effective_color_scheme = ComputedValuesFFI::rust_resolve_effective_color_scheme(builder.property(PropertyID::ColorScheme).rust_style_value_data(), &effective_color_scheme_input);
                builder.set_effective_color_scheme(static_cast<PreferredColorScheme>(effective_color_scheme));
            }
        }
    };

    // The property computation flow is driven from the Rust style computation core: it
    // iterates the longhands in computation order, resolves logical pairing through its
    // mapping tables, and selects the cascaded, inherited or initial value natively.
    struct LonghandLoopContext {
        ComputedProperties::Builder& builder;
        decltype(store_computed_batch)& store_computed_batch_callback;
        decltype(fetch_length_resolution_context)& fetch_length_resolution_context_callback;
        decltype(get_logical_alias_mapping_context)& get_logical_alias_mapping_context_callback;
        DOM::AbstractElement abstract_element;
        Optional<ComputedValuesFFI::FfiDisplay> display_before_adjustments;
        Optional<Keyword> float_before_adjustments;
        Optional<Keyword> overflow_x_before_adjustments;
        Optional<Keyword> overflow_y_before_adjustments;
        Optional<Keyword> text_align_before_adjustments;
        Optional<Keyword> position_before_adjustments;
        RefPtr<StyleValue const> line_height_before_adjustments;
        // Pins every parent value handed out by the explicit-inherit fetch until the end
        // of the drive; the driver may queue the shells in deferred store batches.
        Vector<NonnullRefPtr<StyleValue const>> pinned_parent_values;
    } loop_context {
        .builder = builder,
        .store_computed_batch_callback = store_computed_batch,
        .fetch_length_resolution_context_callback = fetch_length_resolution_context,
        .get_logical_alias_mapping_context_callback = get_logical_alias_mapping_context,
        .abstract_element = abstract_element,
        .display_before_adjustments = {},
        .float_before_adjustments = {},
        .overflow_x_before_adjustments = {},
        .overflow_y_before_adjustments = {},
        .text_align_before_adjustments = {},
        .position_before_adjustments = {},
        .line_height_before_adjustments = {},
        .pinned_parent_values = {},
    };

    ComputedValuesFFI::FfiLonghandCallbacks const callbacks {
        .context = &loop_context,
        .store_computed_batch = [](void* context, ComputedValuesFFI::FfiComputedStoreEntry const* entries, size_t count) {
            auto& loop_context = *static_cast<LonghandLoopContext*>(context);
            loop_context.store_computed_batch_callback(entries, count); },
        .store_effective_color_scheme = [](void* context, u8 color_scheme) {
            auto& loop_context = *static_cast<LonghandLoopContext*>(context);
            loop_context.builder.set_effective_color_scheme(static_cast<PreferredColorScheme>(color_scheme)); },
        .prepare_post_compute_adjustments = [](void* context, ComputedValuesFFI::FfiDisplay const* display_before, u16 float_before, u16 overflow_x_before, u16 overflow_y_before, u16 text_align_before, u16 position_before, bool check_input_line_height) -> ComputedValuesFFI::FfiInputLineHeightMetrics {
            auto& loop_context = *static_cast<LonghandLoopContext*>(context);
            loop_context.display_before_adjustments = *display_before;
            loop_context.float_before_adjustments = static_cast<Keyword>(float_before);
            loop_context.overflow_x_before_adjustments = static_cast<Keyword>(overflow_x_before);
            loop_context.overflow_y_before_adjustments = static_cast<Keyword>(overflow_y_before);
            loop_context.text_align_before_adjustments = static_cast<Keyword>(text_align_before);
            loop_context.position_before_adjustments = static_cast<Keyword>(position_before);
            loop_context.line_height_before_adjustments = loop_context.builder.style().property(PropertyID::LineHeight);
            loop_context.builder.set_display_before_box_type_transformation(from_ffi_display(*display_before));
            return input_line_height_metrics(loop_context.builder, loop_context.abstract_element, check_input_line_height); },
        .fetch_non_inherited_parent_value = [](void* context, u16 inherited_property_id) -> ComputedValuesFFI::FfiShellAndData {
            auto& loop_context = *static_cast<LonghandLoopContext*>(context);
            auto value = get_non_animated_inherit_value(static_cast<PropertyID>(inherited_property_id), loop_context.abstract_element);
            ComputedValuesFFI::FfiShellAndData entry { value.ptr(), value->rust_style_value_data() };
            loop_context.pinned_parent_values.append(move(value));
            return entry;
        },
        .data_of = [](void const* shell) -> void const* { return static_cast<StyleValue const*>(shell)->rust_style_value_data(); },
        .computational_independence_fallback = [](void const* shell) -> bool { return static_cast<StyleValue const*>(shell)->decide_computational_independence_fallback(); },
        .writing_mode_and_direction = [](void* context) -> u16 {
            auto& loop_context = *static_cast<LonghandLoopContext*>(context);
            auto mapping_context = loop_context.get_logical_alias_mapping_context_callback();
            return static_cast<u16>(to_underlying(mapping_context.writing_mode)) | static_cast<u16>(to_underlying(mapping_context.direction)) << 8;
        },
        .length_resolution_context = [](void* context, u16 property_id, ComputedValuesFFI::FfiLengthResolutionContext* out) {
            auto& loop_context = *static_cast<LonghandLoopContext*>(context);
            *out = loop_context.fetch_length_resolution_context_callback(static_cast<PropertyID>(property_id)); },
    };

    constexpr size_t longhand_bitmap_words = (number_of_longhand_properties + 63) / 64;
    Array<u64, longhand_bitmap_words> important_words {};
    Array<u64, longhand_bitmap_words> inherited_words {};
    ComputedValuesFFI::FfiLonghandDriverResults driver_results {
        .important_words = important_words.data(),
        .inherited_words = inherited_words.data(),
        .word_count = longhand_bitmap_words,
        .raw_cascaded_font_size_shell = nullptr,
        .depends_on_viewport_metrics = false,
        .font_metrics_depend_on_viewport_metrics = false,
        .explicitly_inherited_non_inherited_property = false,
    };
    auto box_type_input = make_box_type_transformation_input(
        abstract_element, InitialValues::display(), Keyword::Static, Keyword::None);
    ComputedValuesFFI::rust_drive_property_computation(&callbacks, cascaded_properties.rust_store(), parent_snapshot.has_value() ? &*parent_snapshot : nullptr, &box_type_input, &effective_color_scheme_input, abstract_element.element().local_name() == HTML::TagNames::th, new_font_size != nullptr, device_pixels_per_css_pixel, InitialValues::font_size().raw_value(), default_user_font_size().raw_value(), &driver_results);

    // Apply the driver's bulk results.
    auto longhand_bit_is_set = [](Array<u64, longhand_bitmap_words> const& words, size_t index) {
        return (words[index / 64] & (1ull << (index % 64))) != 0;
    };
    for (size_t index = 0; index < number_of_longhand_properties; ++index) {
        auto property_id = static_cast<PropertyID>(to_underlying(first_longhand_property_id) + index);
        if (longhand_bit_is_set(important_words, index))
            builder.set_property_important(property_id, Important::Yes);
        if (longhand_bit_is_set(inherited_words, index))
            builder.set_property_inherited(property_id, ComputedProperties::Inherited::Yes);
    }
    // Store the raw winning cascaded font-size. This is needed to implement the time-traveling inheritance for
    // font-size when font-family is monospace.
    // See the recascade_font_size_if_needed() function for further details.
    if (driver_results.raw_cascaded_font_size_shell)
        builder.set_raw_cascaded_font_size(*static_cast<StyleValue const*>(driver_results.raw_cascaded_font_size_shell));
    if (driver_results.depends_on_viewport_metrics)
        builder.set_depends_on_viewport_metrics();
    if (driver_results.font_metrics_depend_on_viewport_metrics)
        builder.set_font_metrics_depend_on_viewport_metrics();
    if (driver_results.explicitly_inherited_non_inherited_property) {
        if (auto* parent = abstract_element.element().parent(); parent && is<DOM::ShadowRoot>(*parent))
            parent->set_children_may_depend_on_non_inherited_property_inheritance();
    }

    if (is<HTML::HTMLHtmlElement>(abstract_element.element())) {
        m_root_element_font_metrics = calculate_root_element_font_metrics(computed_style);
        m_root_element_font_metrics_depend_on_viewport_metrics = builder.font_metrics_depend_on_viewport_metrics();
    }

    // Compute the value of custom properties
    compute_custom_properties(computed_style, abstract_element);

    clear_computation_context_caches();

    // Add or modify CSS-defined animations
    process_animation_definitions(computed_style, cascaded_properties, abstract_element);

    auto restore_values_before_post_compute_adjustments = [&] {
        VERIFY(loop_context.display_before_adjustments.has_value());
        VERIFY(loop_context.float_before_adjustments.has_value());
        VERIFY(loop_context.overflow_x_before_adjustments.has_value());
        VERIFY(loop_context.overflow_y_before_adjustments.has_value());
        VERIFY(loop_context.text_align_before_adjustments.has_value());
        VERIFY(loop_context.position_before_adjustments.has_value());
        VERIFY(loop_context.line_height_before_adjustments);
        builder.set_property_without_modifying_flags(PropertyID::Display, DisplayStyleValue::create(from_ffi_display(*loop_context.display_before_adjustments)));
        builder.set_property_without_modifying_flags(PropertyID::Float, KeywordStyleValue::create(*loop_context.float_before_adjustments));
        builder.set_property_without_modifying_flags(PropertyID::OverflowX, KeywordStyleValue::create(*loop_context.overflow_x_before_adjustments));
        builder.set_property_without_modifying_flags(PropertyID::OverflowY, KeywordStyleValue::create(*loop_context.overflow_y_before_adjustments));
        builder.set_property_without_modifying_flags(PropertyID::TextAlign, KeywordStyleValue::create(*loop_context.text_align_before_adjustments));
        builder.set_property_without_modifying_flags(PropertyID::Position, KeywordStyleValue::create(*loop_context.position_before_adjustments));
        builder.set_property_without_modifying_flags(PropertyID::LineHeight, *loop_context.line_height_before_adjustments);
    };
    if (animation_values_applied)
        restore_values_before_post_compute_adjustments();

    auto animations = abstract_element.element().get_animations_internal(
        Animations::Animatable::GetAnimationsSorted::Yes,
        Bindings::GetAnimationsOptions { .subtree = false });
    if (animations.is_exception()) {
        dbgln("Error getting animations for element {}", abstract_element.debug_description());
    } else {
        for (auto& animation : animations.value()) {
            if (auto effect = animation->effect(); effect && effect->is_keyframe_effect()) {
                auto& keyframe_effect = *static_cast<Animations::KeyframeEffect*>(effect.ptr());
                if (keyframe_effect.pseudo_element_type() == abstract_element.pseudo_element()) {
                    if (!animation_values_applied)
                        restore_values_before_post_compute_adjustments();
                    animation_values_applied = true;
                    collect_animation_into(abstract_element, keyframe_effect, builder);
                }
            }
        }
    }

    bool parent_text_align_input_is_animated = false;
    if (computed_values_to_inherit_from) {
        if (auto const* animated_properties = computed_values_to_inherit_from->animated_properties()) {
            parent_text_align_input_is_animated = animated_properties->has_property(PropertyID::TextAlign)
                || animated_properties->has_property(PropertyID::Direction);
        }
    }
    if (parent_text_align_input_is_animated && !animation_values_applied) {
        VERIFY(loop_context.text_align_before_adjustments.has_value());
        builder.set_property_without_modifying_flags(PropertyID::TextAlign, KeywordStyleValue::create(*loop_context.text_align_before_adjustments));
    }

    // Run automatic box type transformations again after animations have been applied.
    if (animation_values_applied)
        transform_box_type_if_needed(builder, abstract_element);

    // Apply any property-specific computed value logic
    if (animation_values_applied)
        resolve_effective_overflow_values(builder);
    if (animation_values_applied || parent_text_align_input_is_animated)
        compute_text_align(builder, abstract_element);

    // Let the element adjust computed style
    if (animation_values_applied && !abstract_element.pseudo_element().has_value())
        abstract_element.element().adjust_computed_style(builder);

    bool parent_style_in_display_none_subtree = false;
    if (auto parent = abstract_element.element_to_inherit_style_from(); parent.has_value()) {
        if (auto parent_style = parent->computed_values())
            parent_style_in_display_none_subtree = parent_style->in_display_none_subtree();
    }

    // Transition declarations [css-transitions-1]
    // Theoretically this should be part of the cascade, but it works with computed values, which we don't have until now.
    compute_transitioned_properties(computed_style, abstract_element);
    if (auto previous_style = abstract_element.computed_values()) {
        // https://drafts.csswg.org/css-transitions-2/#defining-before-change-style
        // In Level 1 of this specification, transitions can only start during a style change event for elements which
        // have a defined before-change style established by the previous style change event. That means a transition
        // could not be started on an element that was not being rendered for the previous style change event.
        // FIXME: If an element does not have a before-change style for a given style change event, the starting style
        //        is used instead of the before-change style to compare with the after-change style to start
        //        transitions.
        if (!previous_style->in_display_none_subtree() && !parent_style_in_display_none_subtree)
            start_needed_transitions(*previous_style, builder, abstract_element);
    }

    if (parent_style_in_display_none_subtree || builder.display().is_none())
        builder.set_in_display_none_subtree();

    return CSS::ComputedProperties::create(move(builder));
}

struct SimplifiedSelectorForBucketing {
    CSS::Selector::SimpleSelector::Type type;
    Utf16FlyString name;
};

static Optional<SimplifiedSelectorForBucketing> bucket_for_is_or_where_selector(CSS::Selector::SimpleSelector const&);
static Optional<Vector<SimplifiedSelectorForBucketing>> buckets_for_is_or_where_selector(CSS::Selector::SimpleSelector const&);
static Optional<PseudoClass> subject_pseudo_class_bucket_for_is_or_where_selector(CSS::Selector::SimpleSelector const&);

static bool simplified_selectors_for_bucketing_are_equal(SimplifiedSelectorForBucketing const& a, SimplifiedSelectorForBucketing const& b)
{
    return a.type == b.type && a.name == b.name;
}

static void append_unique_simplified_selector_for_bucketing(Vector<SimplifiedSelectorForBucketing>& buckets, SimplifiedSelectorForBucketing bucket)
{
    for (auto const& existing_bucket : buckets) {
        if (simplified_selectors_for_bucketing_are_equal(existing_bucket, bucket))
            return;
    }
    buckets.append(move(bucket));
}

static bool subject_pseudo_class_is_bucketable(PseudoClass pseudo_class)
{
    switch (pseudo_class) {
    case PseudoClass::Active:
    case PseudoClass::AnyLink:
    case PseudoClass::Checked:
    case PseudoClass::Disabled:
    case PseudoClass::Enabled:
    case PseudoClass::Focus:
    case PseudoClass::FocusVisible:
    case PseudoClass::FocusWithin:
    case PseudoClass::Fullscreen:
    case PseudoClass::Heading:
    case PseudoClass::Host:
    case PseudoClass::Hover:
    case PseudoClass::Link:
    case PseudoClass::LocalLink:
    case PseudoClass::PlaceholderShown:
    case PseudoClass::Target:
    case PseudoClass::Unchecked:
    case PseudoClass::Visited:
        return true;
    default:
        return false;
    }
}

static u8 subject_pseudo_class_bucket_priority(PseudoClass pseudo_class)
{
    switch (pseudo_class) {
    case PseudoClass::Host:
        return 110;
    case PseudoClass::FocusVisible:
        return 100;
    case PseudoClass::Focus:
        return 90;
    case PseudoClass::Active:
    case PseudoClass::Fullscreen:
    case PseudoClass::Target:
    case PseudoClass::Heading:
        return 80;
    case PseudoClass::Checked:
    case PseudoClass::Disabled:
    case PseudoClass::LocalLink:
    case PseudoClass::PlaceholderShown:
    case PseudoClass::Visited:
        return 70;
    case PseudoClass::AnyLink:
    case PseudoClass::Link:
        return 60;
    case PseudoClass::Hover:
        return 50;
    case PseudoClass::FocusWithin:
        return 40;
    case PseudoClass::Enabled:
    case PseudoClass::Unchecked:
        return 30;
    default:
        VERIFY_NOT_REACHED();
    }
}

static Optional<PseudoClass> subject_pseudo_class_bucket_for_compound_selector(CSS::Selector::CompoundSelector const& compound_selector)
{
    Optional<PseudoClass> best_bucket;

    auto consider_bucket = [&](PseudoClass pseudo_class) {
        if (!best_bucket.has_value()
            || subject_pseudo_class_bucket_priority(pseudo_class) > subject_pseudo_class_bucket_priority(best_bucket.value())) {
            best_bucket = pseudo_class;
        }
    };

    for (auto const& simple_selector : compound_selector.simple_selectors.in_reverse()) {
        if (simple_selector.type != CSS::Selector::SimpleSelector::Type::PseudoClass)
            continue;

        auto const pseudo_class = simple_selector.pseudo_class().type;
        if (subject_pseudo_class_is_bucketable(pseudo_class)) {
            consider_bucket(pseudo_class);
            continue;
        }

        if (auto bucket = subject_pseudo_class_bucket_for_is_or_where_selector(simple_selector); bucket.has_value())
            consider_bucket(bucket.value());
    }

    return best_bucket;
}

static Optional<SimplifiedSelectorForBucketing> bucket_for_compound_selector(CSS::Selector::CompoundSelector const& compound_selector)
{
    Optional<SimplifiedSelectorForBucketing> attribute_bucket;
    for (auto const& simple_selector : compound_selector.simple_selectors.in_reverse()) {
        if (simple_selector.type == CSS::Selector::SimpleSelector::Type::Id) {
            return SimplifiedSelectorForBucketing { .type = simple_selector.type, .name = simple_selector.id_name() };
        }
        if (simple_selector.type == CSS::Selector::SimpleSelector::Type::Class) {
            return SimplifiedSelectorForBucketing { .type = simple_selector.type, .name = simple_selector.class_name() };
        }

        if (simple_selector.type == CSS::Selector::SimpleSelector::Type::TagName) {
            return SimplifiedSelectorForBucketing { .type = simple_selector.type, .name = simple_selector.qualified_name().name.lowercase_name };
        }

        if (simple_selector.type == CSS::Selector::SimpleSelector::Type::Attribute) {
            if (!attribute_bucket.has_value())
                attribute_bucket = SimplifiedSelectorForBucketing { .type = simple_selector.type, .name = simple_selector.attribute().qualified_name.name.lowercase_name };
            continue;
        }

        if (auto bucket = bucket_for_is_or_where_selector(simple_selector); bucket.has_value()) {
            if (bucket->type != CSS::Selector::SimpleSelector::Type::Attribute)
                return bucket;
            if (!attribute_bucket.has_value())
                attribute_bucket = bucket.release_value();
        }
    }

    return attribute_bucket;
}

static Optional<SimplifiedSelectorForBucketing> bucket_for_is_or_where_selector(CSS::Selector::SimpleSelector const& simple_selector)
{
    if (simple_selector.type != CSS::Selector::SimpleSelector::Type::PseudoClass)
        return {};

    if (simple_selector.pseudo_class().type != CSS::PseudoClass::Is
        && simple_selector.pseudo_class().type != CSS::PseudoClass::Where)
        return {};

    auto const& selector_list = simple_selector.pseudo_class().argument_selector_list;
    if (selector_list.is_empty())
        return {};

    Optional<SimplifiedSelectorForBucketing> common_bucket;
    for (auto const& argument_selector : selector_list) {
        auto bucket = bucket_for_compound_selector(argument_selector->compound_selectors().last());
        if (!bucket.has_value())
            return {};
        if (!common_bucket.has_value()) {
            common_bucket = bucket.release_value();
            continue;
        }
        if (!simplified_selectors_for_bucketing_are_equal(*common_bucket, *bucket))
            return {};
    }
    return common_bucket;
}

static Optional<Vector<SimplifiedSelectorForBucketing>> buckets_for_is_or_where_selector(CSS::Selector::SimpleSelector const& simple_selector)
{
    if (simple_selector.type != CSS::Selector::SimpleSelector::Type::PseudoClass)
        return {};

    if (simple_selector.pseudo_class().type != CSS::PseudoClass::Is
        && simple_selector.pseudo_class().type != CSS::PseudoClass::Where)
        return {};

    auto const& selector_list = simple_selector.pseudo_class().argument_selector_list;
    if (selector_list.is_empty())
        return {};

    Vector<SimplifiedSelectorForBucketing> buckets;
    for (auto const& argument_selector : selector_list) {
        auto bucket = bucket_for_compound_selector(argument_selector->compound_selectors().last());
        if (!bucket.has_value())
            return {};
        append_unique_simplified_selector_for_bucketing(buckets, bucket.release_value());
    }

    if (buckets.size() < 2)
        return {};
    return buckets;
}

static Optional<PseudoClass> subject_pseudo_class_bucket_for_is_or_where_selector(CSS::Selector::SimpleSelector const& simple_selector)
{
    if (simple_selector.type != CSS::Selector::SimpleSelector::Type::PseudoClass)
        return {};

    if (simple_selector.pseudo_class().type != CSS::PseudoClass::Is
        && simple_selector.pseudo_class().type != CSS::PseudoClass::Where)
        return {};

    auto const& selector_list = simple_selector.pseudo_class().argument_selector_list;
    if (selector_list.is_empty())
        return {};

    Optional<PseudoClass> common_bucket;
    for (auto const& argument_selector : selector_list) {
        auto bucket = subject_pseudo_class_bucket_for_compound_selector(argument_selector->compound_selectors().last());
        if (!bucket.has_value())
            return {};
        if (!common_bucket.has_value()) {
            common_bucket = bucket.value();
            continue;
        }
        if (common_bucket.value() != bucket.value())
            return {};
    }
    return common_bucket;
}

static Optional<u32> ancestor_hash_bucket_for_selector(Selector const& selector)
{
    if (!selector.can_use_ancestor_filter())
        return {};

    for (auto hash : selector.ancestor_hashes()) {
        if (hash == 0)
            break;
        return hash;
    }
    return {};
}

static bool matches_hover_pseudo_class_for_rule_bucket(DOM::Element const& element)
{
    auto* hovered_node = element.document().hovered_node();
    if (!hovered_node)
        return false;
    if (&element == hovered_node)
        return true;
    return element.is_shadow_including_ancestor_of(*hovered_node);
}

static bool matches_subject_pseudo_class_bucket(PseudoClass pseudo_class, DOM::Element const& element)
{
    switch (pseudo_class) {
    case PseudoClass::Active:
        return element.is_being_activated();
    case PseudoClass::AnyLink:
    case PseudoClass::Link:
        return element.matches_link_pseudo_class();
    case PseudoClass::Checked:
        return element.matches_checked_pseudo_class();
    case PseudoClass::Disabled:
        return element.matches_disabled_pseudo_class();
    case PseudoClass::Enabled:
        return element.matches_enabled_pseudo_class();
    case PseudoClass::Focus:
        return element.is_focused();
    case PseudoClass::FocusVisible:
        return element.is_focused() && element.should_indicate_focus();
    case PseudoClass::FocusWithin:
        return element.matches_focus_within_pseudo_class();
    case PseudoClass::Fullscreen:
        return element.is_fullscreen_element();
    case PseudoClass::Heading:
        return element.is_html_heading_element();
    case PseudoClass::Host:
        return element.is_shadow_host();
    case PseudoClass::Hover:
        return matches_hover_pseudo_class_for_rule_bucket(element);
    case PseudoClass::LocalLink:
        return element.matches_local_link_pseudo_class();
    case PseudoClass::PlaceholderShown:
        return element.matches_placeholder_shown_pseudo_class();
    case PseudoClass::Target:
        return element.is_target();
    case PseudoClass::Unchecked:
        return element.matches_unchecked_pseudo_class();
    case PseudoClass::Visited:
        return element.matches_visited_pseudo_class();
    default:
        VERIFY_NOT_REACHED();
    }
}

static NonnullRefPtr<StyleValue const> resolve_css_wide_keyword_for_custom_property(Optional<CustomPropertyRegistration const&> registration, AbstractOrHypotheticalElement const& element, Utf16FlyString const& name, NonnullRefPtr<StyleValue const> keyword_value, ComputedProperties const* computed_style_for_custom_property_resolution, Optional<Parser::GuardedSubstitutionContexts&> guarded_contexts)
{
    VERIFY(keyword_value->is_css_wide_keyword());

    // https://drafts.csswg.org/css-mixins/#resolve-function-styles
    // On result, all CSS-wide keywords are left unresolved.
    if (name == "result"_utf16_fly_string)
        return keyword_value;

    if (keyword_value->is_initial())
        return initial_custom_property_value(registration, element.document());

    if (keyword_value->is_inherit())
        return inherited_custom_property_value(registration, element, name, computed_style_for_custom_property_resolution, guarded_contexts);

    // https://drafts.csswg.org/css-mixins/#resolve-function-styles
    // NB: When resolving function styles (i.e. when we have a hypothetical element), all CSS-wide keywords other than
    //     inherit and initial resolve to the guaranteed-invalid value.
    if (element.has<HypotheticalElement*>())
        return GuaranteedInvalidStyleValue::create();

    // Unset is the same as inherit for inherited properties, and by default all unregistered custom properties inherit.
    if (keyword_value->is_unset())
        return registration.has_value() && !registration->inherit
            ? initial_custom_property_value(registration, element.document())
            : inherited_custom_property_value(registration, element, name, computed_style_for_custom_property_resolution, guarded_contexts);

    if (keyword_value->is_revert()) {
        // FIXME: Implement reverting custom properties.
        return keyword_value;
    }
    if (keyword_value->is_revert_layer()) {
        // FIXME: Implement reverting custom properties.
        return keyword_value;
    }

    VERIFY_NOT_REACHED();
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_value_of_custom_property(ComputedProperties const* computed_style_for_custom_property_resolution, AbstractOrHypotheticalElement const& element, Utf16FlyString const& name, Optional<Parser::GuardedSubstitutionContexts&> guarded_contexts) const
{
    // https://drafts.csswg.org/css-variables/#propdef-
    // The computed value of a custom property is its specified value with any arbitrary-substitution functions replaced.
    // FIXME: These should probably be part of ComputedProperties.
    auto& document = element.document();
    auto registration = element.get_registered_custom_property(name);

    auto value = element.get_custom_property(name);
    auto resolved_value = value ? value.release_nonnull() : initial_custom_property_value(registration, document);

    if (resolved_value->is_css_wide_keyword())
        resolved_value = resolve_css_wide_keyword_for_custom_property(registration, element, name, move(resolved_value), computed_style_for_custom_property_resolution, guarded_contexts);

    if (resolved_value->is_unresolved() && resolved_value->as_unresolved().contains_arbitrary_substitution_function()) {
        auto& unresolved = resolved_value->as_unresolved();
        Parser::ArbitrarySubstitutionReplacementContext arbitrary_substitution_context {
            .computed_style_for_custom_property_resolution = computed_style_for_custom_property_resolution,
        };
        resolved_value = Parser::Parser::resolve_unresolved_style_value(Parser::ParsingParams { document }, element, arbitrary_substitution_context, PropertyNameAndID { {}, PropertyID::Custom, name }, unresolved, guarded_contexts);

        // A CSS-wide keyword produced by substitution takes on that keyword's meaning for the custom property,
        // exactly as a literally-specified one would (handled above before substitution).
        if (resolved_value->is_css_wide_keyword())
            resolved_value = resolve_css_wide_keyword_for_custom_property(registration, element, name, move(resolved_value), computed_style_for_custom_property_resolution, guarded_contexts);
    }

    auto invalid_custom_property_fallback_value = [&](NonnullRefPtr<StyleValue const> invalid_value) -> NonnullRefPtr<StyleValue const> {
        // https://drafts.csswg.org/css-values-5/#invalid-substitution
        // When property replacement results in a property’s value containing the guaranteed-invalid value, this makes
        // the declaration invalid at computed-value time. When this happens, the computed value is one of the
        // following depending on the property’s type:

        // -> The property is a non-registered custom property
        // -> The property is a registered custom property with universal syntax
        if (!registration.has_value() || registration->syntax->type() == Parser::SyntaxNode::NodeType::Universal) {
            // The computed value is the guaranteed-invalid value.
            return invalid_value;
        }

        // -> Otherwise
        {
            // Either the property’s inherited value or its initial value depending on whether the property is
            // inherited or not, respectively, as if the property’s value had been specified as the unset keyword.

            // https://drafts.csswg.org/css-mixins/#resolve-function-styles
            // NB: When resolving function styles (i.e. when we have a hypothetical element), all CSS-wide keywords other than
            //     inherit and initial (including unset) resolve to the guaranteed-invalid value.
            if (element.has<HypotheticalElement*>())
                return invalid_value;

            if (registration->inherit)
                return inherited_custom_property_value(registration, element, name, computed_style_for_custom_property_resolution, guarded_contexts);
            return initial_custom_property_value(registration, element.document());
        }
    };

    if (resolved_value->is_guaranteed_invalid())
        return invalid_custom_property_fallback_value(move(resolved_value));

    if (!registration.has_value() || registration->syntax->type() == Parser::SyntaxNode::NodeType::Universal)
        return resolved_value;

    auto resolved_value_contains_attr_tainted_values = resolved_value->is_unresolved() && resolved_value->as_unresolved().contains_attr_tainted_values();
    auto parsing_params = Parser::ParsingParams { document };
    parsing_params.value_context.append(PropertyID::Custom);
    auto parsed_value = Parser::parse_with_a_syntax(parsing_params, resolved_value->tokenize(), registration->syntax);
    if (parsed_value->is_guaranteed_invalid())
        return invalid_custom_property_fallback_value(move(parsed_value));

    auto computed_value = [&] {
        // FIXME: At the moment we incorrectly apply ASF replacement at cascade time when we should instead be applying
        //        it at computed-value time. This means we may not yet have a ComputedProperties for us to absolutize
        //        against. For now we just return the parsed value as-is and rely on the consuming property to
        //        absolutize it later.
        if (!computed_style_for_custom_property_resolution)
            return parsed_value;

        return compute_registered_custom_property_value(registration.value(), move(parsed_value), get_computation_context_for_property(PropertyID::Custom, *computed_style_for_custom_property_resolution, element.abstract_element()));
    }();

    if (resolved_value_contains_attr_tainted_values) {
        VERIFY(!computed_value->is_unresolved());
        return UnresolvedStyleValue::create_attr_tainted_with_parsed_value(computed_value->tokenize(), {}, {}, UnresolvedStyleValue::SourceTextMode::Trim, computed_value);
    }

    return computed_value;
}

ComputationContext StyleComputer::fallback_computation_context_for_custom_property(AbstractOrHypotheticalElement const& element) const
{
    auto abstract_element = element.abstract_element();

    auto context_from_computed_values = [&](DOM::AbstractElement const& styled_element) -> ComputationContext {
        auto length_resolution_context = Length::ResolutionContext::for_element(styled_element);
        length_resolution_context.subject_element = &abstract_element.element();
        return {
            .length_resolution_context = move(length_resolution_context),
            .abstract_element = abstract_element,
            .color_scheme = styled_element.computed_values()->color_scheme(),
        };
    };

    if (abstract_element.computed_values())
        return context_from_computed_values(abstract_element);

    if (auto parent = abstract_element.element_to_inherit_style_from(); parent.has_value() && parent->computed_values())
        return context_from_computed_values(*parent);

    auto length_resolution_context = Length::ResolutionContext::for_document(document());
    length_resolution_context.subject_element = &abstract_element.element();
    return {
        .length_resolution_context = length_resolution_context,
        .abstract_element = abstract_element,
    };
}

void StyleComputer::compute_custom_properties(ComputedProperties& computed_style, DOM::AbstractElement abstract_element) const
{
    // https://drafts.csswg.org/css-variables/#propdef-
    // The computed value of a custom property is its specified value with any arbitrary-substitution functions replaced.
    // FIXME: These should probably be part of ComputedProperties.
    auto data = abstract_element.custom_property_data();
    if (!data)
        return;

    // If this element is sharing its parent's data (no own custom properties),
    // the parent has already resolved its values, so there's nothing to do.
    auto inherit_from = abstract_element.element_to_inherit_style_from();
    if (inherit_from.has_value() && inherit_from->custom_property_data().ptr() == data.ptr())
        return;

    if (data->own_values().is_empty())
        return;

    // Resolve var() references and only keep values that differ from parent.
    // This avoids growing the hashmap to full size and then shrinking it,
    // which would leave an oversized bucket array.
    RefPtr<CustomPropertyData const> parent_data;
    if (inherit_from.has_value())
        parent_data = inheritable_custom_property_data(*inherit_from);

    OrderedHashMap<Utf16FlyString, StyleProperty> resolved_own;
    for (auto const& [name, style_property] : data->own_values()) {
        // FIXME: Can we store the resolved value in `data` immediately to avoid recomputing it for any subsequent
        //        properties that depend on it?
        auto resolved_value = compute_value_of_custom_property(&computed_style, abstract_element, name);
        if (parent_data) {
            auto const* parent_property = parent_data->get(name);
            if (parent_property && resolved_value->equals(*parent_property->value))
                continue;
        }
        resolved_own.set(name,
            StyleProperty {
                .important = style_property.important,
                .property_id = style_property.property_id,
                .value = move(resolved_value),
            });
    }

    if (resolved_own.is_empty() && parent_data) {
        abstract_element.set_custom_property_data(parent_data);
        return;
    }

    // FIXME: We should update in place so that non-recomputed children aren't left pointing at stale data
    abstract_element.set_custom_property_data(
        CustomPropertyData::create(move(resolved_own), parent_data ? move(parent_data) : data->parent()));
}

// https://www.w3.org/TR/css-values-4/#snap-a-length-as-a-border-width
static CSSPixels snap_a_length_as_a_border_width(double device_pixels_per_css_pixel, CSSPixels length)
{
    // 1. Assert: len is non-negative.
    VERIFY(length >= 0);

    // 2. If len is an integer number of device pixels, do nothing.
    auto device_pixels = length.to_double() * device_pixels_per_css_pixel;
    if (device_pixels == trunc(device_pixels))
        return length;

    // 3. If len is greater than zero, but less than 1 device pixel, round len up to 1 device pixel.
    if (device_pixels > 0 && device_pixels < 1)
        return CSSPixels::nearest_value_for(1 / device_pixels_per_css_pixel);

    // 4. If len is greater than 1 device pixel, round it down to the nearest integer number of device pixels.
    if (device_pixels > 1)
        return CSSPixels::nearest_value_for(floor(device_pixels) / device_pixels_per_css_pixel);

    return length;
}

static NonnullRefPtr<StyleValue const> compute_style_value_list(NonnullRefPtr<StyleValue const> const& style_value, Function<NonnullRefPtr<StyleValue const>(NonnullRefPtr<StyleValue const> const&)> const& compute_entry)
{
    StyleValueVector computed_entries;

    for (auto const& entry : style_value->as_value_list().values())
        computed_entries.append(compute_entry(entry));

    return StyleValueList::create(move(computed_entries), StyleValueList::Separator::Comma);
}

static NonnullRefPtr<StyleValue const> repeat_style_value_list_to_n_elements(NonnullRefPtr<StyleValue const> const& style_value, size_t n)
{
    auto const& value_list = style_value->as_value_list();

    if (value_list.size() == n)
        return style_value;

    StyleValueVector repeated_values;
    repeated_values.ensure_capacity(n);

    for (size_t i = 0; i < n; ++i)
        repeated_values.unchecked_append(value_list.value_at(i, true));

    return StyleValueList::create(move(repeated_values), value_list.separator());
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_value_of_property(
    PropertyID property_id,
    NonnullRefPtr<StyleValue const> const& specified_value,
    Function<NonnullRefPtr<StyleValue const>(PropertyID)> const& get_property_specified_value,
    ComputationContext const& computation_context,
    double device_pixels_per_css_pixel)
{
    auto const& absolutized_value = specified_value->absolutized(computation_context);

    auto inheritance_parent = [&]() {
        return computation_context.abstract_element
            .map([](auto const& abstract_element) { return abstract_element.element_to_inherit_style_from(); })
            .value_or(OptionalNone {});
    };

    switch (property_id) {
    case PropertyID::AnimationName:
        return compute_animation_name(absolutized_value);
    // NB: The background properties are coordinated at compute time rather than use time, unlike other coordinating list property groups
    case PropertyID::BackgroundAttachment:
    case PropertyID::BackgroundClip:
    case PropertyID::BackgroundOrigin:
    case PropertyID::BackgroundPositionX:
    case PropertyID::BackgroundPositionY:
    case PropertyID::BackgroundRepeat:
    case PropertyID::BackgroundSize:
        return repeat_style_value_list_to_n_elements(absolutized_value, get_property_specified_value(PropertyID::BackgroundImage)->as_value_list().size());
    case PropertyID::BorderBottomWidth:
    case PropertyID::BorderLeftWidth:
    case PropertyID::BorderRightWidth:
    case PropertyID::BorderTopWidth:
    case PropertyID::OutlineWidth:
        return compute_border_or_outline_width(absolutized_value, device_pixels_per_css_pixel);
    case PropertyID::CornerBottomLeftShape:
    case PropertyID::CornerBottomRightShape:
    case PropertyID::CornerTopLeftShape:
    case PropertyID::CornerTopRightShape:
        return compute_corner_shape(absolutized_value);
    case PropertyID::FontSize: {
        auto parent = inheritance_parent();
        if (ComputedValuesFFI::rust_value_depends_on_inherited_info_for_property(absolutized_value->rust_style_value_data(), to_underlying(PropertyID::FontSize)) && parent.has_value()) {
            auto parent_values = parent->computed_values();
            if (parent_values && parent_values->font_metrics_depend_on_viewport_metrics())
                computation_context.length_resolution_context.record_viewport_relative_length_resolution();
        }
        return compute_font_size(absolutized_value, get_property_specified_value(PropertyID::MathDepth)->as_integer().integer(), parent);
    }
    case PropertyID::FontStyle:
        return compute_font_style(absolutized_value);
    case PropertyID::FontWeight:
        return compute_font_weight(absolutized_value, inheritance_parent());
    case PropertyID::FontWidth:
        return compute_font_width(absolutized_value);
    case PropertyID::FontFeatureSettings:
    case PropertyID::FontVariationSettings:
        return compute_font_feature_tag_value_list(absolutized_value);
    case PropertyID::LetterSpacing:
    case PropertyID::WordSpacing: {
        // The normal-keyword-to-zero-length rule lives in the Rust style computation core.
        auto result = ComputedValuesFFI::rust_compute_letter_or_word_spacing(absolutized_value->rust_style_value_data());
        if (result.unchanged)
            return absolutized_value;
        return LengthStyleValue::create(Length::make_px(result.value));
    }
    case PropertyID::LineHeight:
        return compute_line_height(absolutized_value, computation_context.length_resolution_context.font_metrics.font_size);
    case PropertyID::MathDepth:
        return compute_math_depth(absolutized_value, inheritance_parent());
    case PropertyID::PositionArea:
        return compute_position_area(absolutized_value);
    default:
        return absolutized_value;
    }

    VERIFY_NOT_REACHED();
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_animation_name(NonnullRefPtr<StyleValue const> const& absolutized_value)
{
    // https://drafts.csswg.org/css-animations-1/#animation-name
    // list, each item either a case-sensitive css identifier or the keyword none

    return compute_style_value_list(absolutized_value, [](NonnullRefPtr<StyleValue const> const& entry) -> NonnullRefPtr<StyleValue const> {
        // none | <custom-ident>
        if (entry->to_keyword() == Keyword::None || entry->is_custom_ident())
            return entry;

        // <string>
        if (entry->is_string()) {
            auto const& string_value = entry->as_string().string_value();

            // AD-HOC: We shouldn't convert strings that aren't valid <custom-ident>s
            if (!is_valid_custom_ident(string_value, { { "none"sv } }))
                return entry;

            return CustomIdentStyleValue::create(entry->as_string().string_value());
        }

        VERIFY_NOT_REACHED();
    });
}

// https://drafts.csswg.org/css-fonts-4/#font-variation-settings-def
// https://drafts.csswg.org/css-fonts/#font-feature-settings-prop
NonnullRefPtr<StyleValue const> StyleComputer::compute_font_feature_tag_value_list(NonnullRefPtr<StyleValue const> const& absolutized_value)
{
    // NB: The computation logic is the same for both font-feature-settings and font-variation-settings, first we
    //     deduplicate feature tags (with latter taking precedence), then we sort them in ascending order by code unit
    if (absolutized_value->is_keyword())
        return absolutized_value;

    // The deduplication and sorting live in the Rust style computation core; it works over the
    // entry indices and calls back for the interned-fly-string tag comparisons.
    auto values = absolutized_value->as_value_list().values();
    struct TagContext {
        StyleValueVector const& values;
    } context { values };

    Vector<u32> order;
    order.resize(values.size());
    auto count = ComputedValuesFFI::rust_font_feature_settings_computed_order(
        values.size(),
        &context,
        [](void* context, size_t i, size_t j) -> bool {
            auto const& values = static_cast<TagContext*>(context)->values;
            return values[i]->as_open_type_tagged().tag() == values[j]->as_open_type_tagged().tag();
        },
        [](void* context, size_t i, size_t j) -> bool {
            auto const& values = static_cast<TagContext*>(context)->values;
            return values[i]->as_open_type_tagged().tag().operator<=>(values[j]->as_open_type_tagged().tag()) < 0;
        },
        order.data());

    StyleValueVector axis_tags;
    axis_tags.ensure_capacity(count);
    for (size_t i = 0; i < count; ++i)
        axis_tags.unchecked_append(values[order[i]]);

    return StyleValueList::create(move(axis_tags), StyleValueList::Separator::Comma);
}

NonnullRefPtr<StyleValue const> StyleComputer::compute_border_or_outline_width(NonnullRefPtr<StyleValue const> const& absolutized_value, double device_pixels_per_css_pixel)
{
    // The line-width keywords and the border-width snapping live in the Rust style computation core.
    auto result = ComputedValuesFFI::rust_compute_border_or_outline_width(absolutized_value->rust_style_value_data(), device_pixels_per_css_pixel);
    if (result.handled)
        return LengthStyleValue::create(Length::make_px(result.value));

    auto const absolute_length = Length::from_style_value(absolutized_value, {}).absolute_length_to_px();
    return LengthStyleValue::create(Length::make_px(snap_a_length_as_a_border_width(device_pixels_per_css_pixel, absolute_length)));
}

// https://drafts.csswg.org/css-borders-4/#propdef-corner-top-left-shape
NonnullRefPtr<StyleValue const> StyleComputer::compute_corner_shape(NonnullRefPtr<StyleValue const> const& absolutized_value)
{
    // The keyword-to-superellipse mapping lives in the Rust style computation core.
    auto result = ComputedValuesFFI::rust_compute_corner_shape_parameter(absolutized_value->rust_style_value_data());
    VERIFY(result.handled);
    if (result.unchanged)
        return absolutized_value;

    // NB: The round value is cached since it is the initial value of the corner-*-shape properties.
    if (result.value == 1) {
        static auto const& cached_round_value = SuperellipseStyleValue::create(NumberStyleValue::create(1)).leak_ref();
        return cached_round_value;
    }
    return SuperellipseStyleValue::create(NumberStyleValue::create(result.value));
}
NonnullRefPtr<StyleValue const> StyleComputer::compute_font_size(NonnullRefPtr<StyleValue const> const& absolutized_value, int computed_math_depth, Optional<DOM::AbstractElement> const& inheritance_parent, CSSPixels initial_font_size)
{
    auto inherited_font_size = inheritance_parent.has_value()
        ? inheritance_parent->computed_values()->font_size()
        : initial_font_size;

    auto inherited_math_depth = inheritance_parent.has_value()
        ? inheritance_parent->computed_values()->math_depth()
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
    auto inherited_font_weight = inheritance_parent.has_value()
        ? inheritance_parent->computed_values()->font_weight()
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
NonnullRefPtr<StyleValue const> StyleComputer::compute_line_height(NonnullRefPtr<StyleValue const> const& absolutized_value, CSSPixels computed_font_size)
{
    // The line-height rules live in the Rust style computation core, including calc
    // resolution against the computed font size.
    auto result = ComputedValuesFFI::rust_compute_line_height(absolutized_value->rust_style_value_data(), computed_font_size.raw_value());
    if (result.handled) {
        if (result.unchanged)
            return absolutized_value;
        if (result.is_number)
            return NumberStyleValue::create(result.value);
        return LengthStyleValue::create(Length::make_px(result.value));
    }

    // NOTE: We also support calc()'d lengths (percentages resolve to lengths so we don't have to handle them separately)
    if (absolutized_value->is_calculated() && absolutized_value->as_calculated().resolves_to_length_percentage())
        return LengthStyleValue::create(absolutized_value->as_calculated().resolve_length({ .percentage_basis = Length::make_px(computed_font_size) }).value());

    // NOTE: We also support calc()'d numbers
    VERIFY(absolutized_value->is_calculated() && absolutized_value->as_calculated().resolves_to_number());
    return NumberStyleValue::create(absolutized_value->as_calculated().resolve_number({ .percentage_basis = Length::make_px(computed_font_size) }).value());
}

// https://drafts.csswg.org/css-anchor-position/#position-area-computed
NonnullRefPtr<StyleValue const> StyleComputer::compute_position_area(NonnullRefPtr<StyleValue const> const& absolutized_value)
{
    // The computed value of a <position-area> value is the two keywords indicating the selected tracks in each axis,
    // with the long (block-start) and short (start) logical keywords treated as equivalent. It serializes in the order
    // given in the grammar (above), with the logical keywords serialized in their short forms (e.g. start start
    // instead of block-start inline-start).
    if (absolutized_value->is_keyword())
        return absolutized_value;

    auto to_short_keyword = [](NonnullRefPtr<KeywordStyleValue const> const& keyword_value) -> NonnullRefPtr<KeywordStyleValue const> {
        // The short-form mapping lives in the Rust style computation core.
        auto short_keyword = static_cast<Keyword>(ComputedValuesFFI::rust_position_area_short_keyword(to_underlying(keyword_value->keyword())));
        if (short_keyword == keyword_value->keyword())
            return keyword_value;
        return KeywordStyleValue::create(short_keyword);
    };

    auto const& value_list = absolutized_value->as_value_list();
    VERIFY(value_list.size() == 2);

    auto values = value_list.values();
    auto const& block_value = values.at(0);
    auto const& inline_value = values.at(1);

    // When one axis is span-all, the value computes to a single logical keyword from the
    // other axis. The remapping decision lives in the Rust style computation core.
    auto span_all_remap = ComputedValuesFFI::rust_position_area_span_all_remap(
        to_underlying(block_value->as_keyword().keyword()), to_underlying(inline_value->as_keyword().keyword()));
    if (block_value->as_keyword().keyword() == Keyword::SpanAll || inline_value->as_keyword().keyword() == Keyword::SpanAll) {
        if (span_all_remap.remapped)
            return KeywordStyleValue::create(static_cast<Keyword>(span_all_remap.keyword));
        return absolutized_value;
    }

    auto short_block_value = to_short_keyword(block_value->as_keyword());
    auto short_inline_value = to_short_keyword(inline_value->as_keyword());
    if (*block_value != short_block_value || *inline_value != short_inline_value)
        return StyleValueList::create({ short_block_value, short_inline_value }, StyleValueList::Separator::Space);

    return absolutized_value;
}

// https://w3c.github.io/mathml-core/#propdef-math-depth
NonnullRefPtr<StyleValue const> StyleComputer::compute_math_depth(NonnullRefPtr<StyleValue const> const& absolutized_value, Optional<DOM::AbstractElement> const& inheritance_parent)
{
    auto inherited_math_depth = inheritance_parent.has_value()
        ? inheritance_parent->computed_values()->math_depth()
        : InitialValues::math_depth();

    auto inherited_math_style = inheritance_parent.has_value()
        ? inheritance_parent->computed_values()->math_style()
        : InitialValues::math_style();

    // The math-depth rules live in the Rust style computation core.
    auto result = ComputedValuesFFI::rust_compute_math_depth(absolutized_value->rust_style_value_data(), inherited_math_depth, inherited_math_style == MathStyle::Compact);
    if (result.handled)
        return IntegerStyleValue::create(static_cast<i64>(result.value));

    // - If the specified value of math-depth is of the form add(<integer>) then the computed value of
    //   math-depth of the element is its inherited value plus the specified integer.
    if (absolutized_value->is_function())
        return IntegerStyleValue::create(AK::saturating_add(inherited_math_depth, int_from_style_value(absolutized_value->as_function().value())));

    VERIFY(absolutized_value->is_calculated());
    return IntegerStyleValue::create(int_from_style_value(absolutized_value));
}

void StyleComputer::reset_ancestor_filter()
{
    m_ancestor_filter->clear();
}

void StyleComputer::reset_has_result_cache()
{
    if (!m_has_result_cache)
        m_has_result_cache = make<SelectorMatching::HasResultCache>();
    else
        m_has_result_cache->clear();

    if (!m_has_fast_reject_filter_cache)
        m_has_fast_reject_filter_cache = make<SelectorMatching::HasFastRejectFilterCache>();
    else
        m_has_fast_reject_filter_cache->clear();
}

void StyleComputer::push_ancestor(DOM::Element const& element)
{
    for_each_element_hash(element, [&](u32 hash) {
        m_ancestor_filter->increment(hash);
    });
}

void StyleComputer::pop_ancestor(DOM::Element const& element)
{
    for_each_element_hash(element, [&](u32 hash) {
        m_ancestor_filter->decrement(hash);
    });
}

template<typename RuleBuckets>
static void add_rule_to_simplified_selector_bucket(RuleBuckets& rule_buckets, MatchingRule const& matching_rule, SimplifiedSelectorForBucketing const& bucket)
{
    if (bucket.type == Selector::SimpleSelector::Type::Id) {
        rule_buckets.rules_by_id.ensure(bucket.name).append(matching_rule);
        return;
    }
    if (bucket.type == Selector::SimpleSelector::Type::Class) {
        rule_buckets.rules_by_class.ensure(bucket.name).append(matching_rule);
        return;
    }
    if (bucket.type == Selector::SimpleSelector::Type::TagName) {
        rule_buckets.rules_by_tag_name.ensure(bucket.name).append(matching_rule);
        return;
    }
    if (bucket.type == Selector::SimpleSelector::Type::Attribute) {
        rule_buckets.rules_by_attribute_name.ensure(bucket.name).append(matching_rule);
        return;
    }
    VERIFY_NOT_REACHED();
}

template<typename RuleBuckets>
static bool add_rule_to_multiple_is_or_where_buckets(RuleBuckets& rule_buckets, MatchingRule const& matching_rule, Selector::CompoundSelector const& bucket_compound_selector, u32& next_multi_bucket_rule_index)
{
    // Pseudo-element style discovery walks originating-element buckets while
    // computing the originating element's style. If a pseudo selector would
    // otherwise fall into `other`, split `:is()`/`:where()` alternatives across
    // their cheap subject buckets so we avoid trying the rule for every element.
    Optional<Vector<SimplifiedSelectorForBucketing>> buckets;
    for (auto const& simple_selector : bucket_compound_selector.simple_selectors.in_reverse()) {
        auto candidate_buckets = buckets_for_is_or_where_selector(simple_selector);
        if (!candidate_buckets.has_value())
            continue;
        buckets = candidate_buckets.release_value();
        break;
    }

    if (!buckets.has_value())
        return false;

    VERIFY(next_multi_bucket_rule_index < NumericLimits<u32>::max());
    auto multi_bucket_matching_rule = matching_rule;
    multi_bucket_matching_rule.multi_bucket_rule_index = ++next_multi_bucket_rule_index;
    for (auto const& bucket : *buckets)
        add_rule_to_simplified_selector_bucket(rule_buckets, multi_bucket_matching_rule, bucket);
    return true;
}

template<typename RuleBuckets>
static void add_rule_to_rule_buckets(RuleBuckets& rule_buckets, MatchingRule const& matching_rule, Selector::CompoundSelector const& bucket_compound_selector, bool contains_root_pseudo_class, SubjectPseudoClassBuckets subject_pseudo_class_buckets, AncestorHashBuckets ancestor_hash_buckets)
{
    // NOTE: We traverse the simple selectors in reverse order to make sure that class/ID buckets are preferred over tag buckets
    //       in the common case of div.foo or div#foo selectors.
    auto add_to_id_bucket = [&](Utf16FlyString const& name) {
        rule_buckets.rules_by_id.ensure(name).append(matching_rule);
    };

    auto add_to_class_bucket = [&](Utf16FlyString const& name) {
        rule_buckets.rules_by_class.ensure(name).append(matching_rule);
    };

    auto add_to_tag_name_bucket = [&](Utf16FlyString const& name) {
        rule_buckets.rules_by_tag_name.ensure(name).append(matching_rule);
    };

    auto add_to_attribute_bucket = [&](Utf16FlyString const& name) {
        rule_buckets.rules_by_attribute_name.ensure(name).append(matching_rule);
    };

    auto add_to_subject_pseudo_class_bucket = [&](PseudoClass pseudo_class) {
        rule_buckets.rules_by_subject_pseudo_class[to_underlying(pseudo_class)].append(matching_rule);
    };

    Optional<PseudoClass> subject_pseudo_class_bucket;
    auto consider_subject_pseudo_class_bucket = [&](PseudoClass pseudo_class) {
        if (!subject_pseudo_class_bucket.has_value()
            || subject_pseudo_class_bucket_priority(pseudo_class) > subject_pseudo_class_bucket_priority(subject_pseudo_class_bucket.value())) {
            subject_pseudo_class_bucket = pseudo_class;
        }
    };

    for (auto const& simple_selector : bucket_compound_selector.simple_selectors.in_reverse()) {
        if (simple_selector.type == Selector::SimpleSelector::Type::Id) {
            add_to_id_bucket(simple_selector.id_name());
            return;
        }
        if (simple_selector.type == Selector::SimpleSelector::Type::Class) {
            add_to_class_bucket(simple_selector.class_name());
            return;
        }
        if (simple_selector.type == Selector::SimpleSelector::Type::TagName) {
            add_to_tag_name_bucket(simple_selector.qualified_name().name.lowercase_name);
            return;
        }
        // NOTE: Single-argument :is()/:where() selectors can be bucketed by a mandatory
        //       id, class, tag, or attribute in their rightmost compound selector.
        if (auto simplified = bucket_for_is_or_where_selector(simple_selector); simplified.has_value()) {
            if (simplified->type == Selector::SimpleSelector::Type::TagName) {
                add_to_tag_name_bucket(simplified->name);
                return;
            }
            if (simplified->type == Selector::SimpleSelector::Type::Class) {
                add_to_class_bucket(simplified->name);
                return;
            }
            if (simplified->type == Selector::SimpleSelector::Type::Id) {
                add_to_id_bucket(simplified->name);
                return;
            }
            if (simplified->type == Selector::SimpleSelector::Type::Attribute) {
                add_to_attribute_bucket(simplified->name);
                return;
            }
        }

        if (simple_selector.type == Selector::SimpleSelector::Type::PseudoClass) {
            auto const pseudo_class = simple_selector.pseudo_class().type;
            if (subject_pseudo_class_is_bucketable(pseudo_class)) {
                consider_subject_pseudo_class_bucket(pseudo_class);
                continue;
            }
            if (auto bucket = subject_pseudo_class_bucket_for_is_or_where_selector(simple_selector); bucket.has_value())
                consider_subject_pseudo_class_bucket(bucket.value());
        }
    }

    if (contains_root_pseudo_class) {
        rule_buckets.root_rules.append(matching_rule);
    } else {
        for (auto const& simple_selector : bucket_compound_selector.simple_selectors) {
            if (simple_selector.type == Selector::SimpleSelector::Type::Attribute) {
                add_to_attribute_bucket(simple_selector.attribute().qualified_name.name.lowercase_name);
                return;
            }
        }
        if (subject_pseudo_class_buckets == SubjectPseudoClassBuckets::Yes && subject_pseudo_class_bucket.has_value()) {
            add_to_subject_pseudo_class_bucket(subject_pseudo_class_bucket.value());
            return;
        }
        if (ancestor_hash_buckets == AncestorHashBuckets::Yes) {
            if (auto ancestor_hash = ancestor_hash_bucket_for_selector(matching_rule.selector); ancestor_hash.has_value()) {
                rule_buckets.rules_by_ancestor_hash.ensure(ancestor_hash.value()).append(matching_rule);
                return;
            }
        }
        rule_buckets.other_rules.append(matching_rule);
    }
}

template<typename RuleBuckets>
static IterationDecision for_each_matching_rule_bucket(DOM::AbstractElement abstract_element, RuleBuckets const& rule_buckets, Function<bool(u32)> const& may_contain_ancestor_hash, Function<IterationDecision(Vector<MatchingRule> const&)> const& callback)
{
    for (auto const& class_name : abstract_element.element().class_names()) {
        if (auto it = rule_buckets.rules_by_class.find(class_name); it != rule_buckets.rules_by_class.end()) {
            if (callback(it->value) == IterationDecision::Break)
                return IterationDecision::Break;
        }
    }
    if (auto id = abstract_element.element().id(); id.has_value()) {
        if (auto it = rule_buckets.rules_by_id.find(id.value()); it != rule_buckets.rules_by_id.end()) {
            if (callback(it->value) == IterationDecision::Break)
                return IterationDecision::Break;
        }
    }
    auto lowercased_local_name = abstract_element.element().lowercased_local_name();
    if (auto it = rule_buckets.rules_by_tag_name.find(lowercased_local_name); it != rule_buckets.rules_by_tag_name.end()) {
        if (callback(it->value) == IterationDecision::Break)
            return IterationDecision::Break;
    }

    if (abstract_element.element().is_document_element()) {
        if (callback(rule_buckets.root_rules) == IterationDecision::Break)
            return IterationDecision::Break;
    }

    IterationDecision decision = IterationDecision::Continue;
    abstract_element.element().for_each_attribute([&](Utf16FlyString const& name, auto const&) {
        if (auto it = rule_buckets.rules_by_attribute_name.find(name); it != rule_buckets.rules_by_attribute_name.end()) {
            decision = callback(it->value);
        }
    });
    if (decision == IterationDecision::Break)
        return IterationDecision::Break;

    static constexpr Array<PseudoClass, 18> subject_pseudo_classes {
        PseudoClass::Host,
        PseudoClass::FocusVisible,
        PseudoClass::Focus,
        PseudoClass::Active,
        PseudoClass::Fullscreen,
        PseudoClass::Target,
        PseudoClass::Heading,
        PseudoClass::Checked,
        PseudoClass::Disabled,
        PseudoClass::LocalLink,
        PseudoClass::PlaceholderShown,
        PseudoClass::Visited,
        PseudoClass::AnyLink,
        PseudoClass::Link,
        PseudoClass::Hover,
        PseudoClass::FocusWithin,
        PseudoClass::Enabled,
        PseudoClass::Unchecked,
    };
    for (auto pseudo_class : subject_pseudo_classes) {
        auto const& rules = rule_buckets.rules_by_subject_pseudo_class[to_underlying(pseudo_class)];
        if (rules.is_empty())
            continue;
        if (!matches_subject_pseudo_class_bucket(pseudo_class, abstract_element.element()))
            continue;
        if (callback(rules) == IterationDecision::Break)
            return IterationDecision::Break;
    }

    for (auto const& [hash, rules] : rule_buckets.rules_by_ancestor_hash) {
        if (!may_contain_ancestor_hash(hash))
            continue;
        if (callback(rules) == IterationDecision::Break)
            return IterationDecision::Break;
    }

    return callback(rule_buckets.other_rules);
}

void RuleCache::add_rule(MatchingRule const& matching_rule, Optional<PseudoElement> pseudo_element, bool contains_root_pseudo_class, SubjectPseudoClassBuckets subject_pseudo_class_buckets, AncestorHashBuckets ancestor_hash_buckets)
{
    if (matching_rule.slotted) {
        slotted_rules.append(matching_rule);
        return;
    }
    if (matching_rule.contains_part_pseudo_element) {
        part_rules.append(matching_rule);
        return;
    }

    if (matching_rule.contains_pseudo_element && pseudo_element.has_value()) {
        if (Selector::PseudoElementSelector::is_known_pseudo_element_type(pseudo_element.value())) {
            auto& pseudo_element_rules = rules_by_pseudo_element[to_underlying(pseudo_element.value())];
            pseudo_element_rules_mask |= pseudo_element_style_bit(pseudo_element.value());

            // Normalized pseudo-element selectors end with a pseudo-element compound; bucket them
            // by the originating element compound so `.foo::before` keeps using the `.foo` bucket.
            auto const& bucket_compound_selector = [&]() -> Selector::CompoundSelector const& {
                for (auto const& compound_selector : matching_rule.selector.compound_selectors().in_reverse()) {
                    if (compound_selector.combinator != Selector::Combinator::PseudoElement)
                        return compound_selector;
                }
                return matching_rule.selector.compound_selectors().last();
            }();
            if (!contains_root_pseudo_class
                && !bucket_for_compound_selector(bucket_compound_selector).has_value()
                && (subject_pseudo_class_buckets == SubjectPseudoClassBuckets::No || !subject_pseudo_class_bucket_for_compound_selector(bucket_compound_selector).has_value())
                && add_rule_to_multiple_is_or_where_buckets(pseudo_element_rules, matching_rule, bucket_compound_selector, next_multi_bucket_rule_index)) {
                return;
            }
            add_rule_to_rule_buckets(pseudo_element_rules, matching_rule, bucket_compound_selector, contains_root_pseudo_class, subject_pseudo_class_buckets, ancestor_hash_buckets);
        }
        return;
    }

    add_rule_to_rule_buckets(*this, matching_rule, matching_rule.selector.compound_selectors().last(), contains_root_pseudo_class, subject_pseudo_class_buckets, ancestor_hash_buckets);
}

void RuleCache::for_each_matching_rules(DOM::AbstractElement abstract_element, Function<bool(u32)> const& may_contain_ancestor_hash, Function<IterationDecision(Vector<MatchingRule> const&)> callback) const
{
    if (abstract_element.pseudo_element().has_value()) {
        if (Selector::PseudoElementSelector::is_known_pseudo_element_type(abstract_element.pseudo_element().value())) {
            (void)for_each_matching_rule_bucket(abstract_element, rules_by_pseudo_element.at(to_underlying(abstract_element.pseudo_element().value())), may_contain_ancestor_hash, callback);
        } else {
            // NOTE: We don't cache rules for unknown pseudo-elements. They can't match anything anyway.
        }
        return;
    }

    (void)for_each_matching_rule_bucket(abstract_element, *this, may_contain_ancestor_hash, callback);
}

void RuleCache::for_each_matching_pseudo_element_rules(DOM::AbstractElement abstract_element, Function<bool(u32)> const& may_contain_ancestor_hash, Function<IterationDecision(Vector<MatchingRule> const&)> callback) const
{
    VERIFY(!abstract_element.pseudo_element().has_value());

    auto remaining_pseudo_element_rules = pseudo_element_rules_mask;
    while (remaining_pseudo_element_rules != 0) {
        auto pseudo_element_index = count_trailing_zeroes(remaining_pseudo_element_rules);
        remaining_pseudo_element_rules &= remaining_pseudo_element_rules - 1;

        auto const& pseudo_element_rules = rules_by_pseudo_element.at(pseudo_element_index);
        if (for_each_matching_rule_bucket(abstract_element, pseudo_element_rules, may_contain_ancestor_hash, callback) == IterationDecision::Break)
            return;
    }
}

void StyleComputer::ScopedMatchingRule::visit_edges(GC::Cell::Visitor& visitor)
{
    if (rule)
        rule->visit_edges(visitor);
    visitor.visit(shadow_root);
    visitor.visit(scope_root);
}

}
