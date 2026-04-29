/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ReportTime.h>
#include <LibWeb/CSS/CSSImportRule.h>
#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSLayerBlockRule.h>
#include <LibWeb/CSS/CSSLayerStatementRule.h>
#include <LibWeb/CSS/CSSNestedDeclarations.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/CounterStyle.h>
#include <LibWeb/CSS/CounterStyleDefinition.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Page/Page.h>

namespace Web::CSS {

void RuleCaches::visit_edges(GC::Cell::Visitor& visitor)
{
    main.visit_edges(visitor);
    for (auto& it : by_layer) {
        it.value->visit_edges(visitor);
    }
}

NonnullRefPtr<StyleCache> StyleCache::create()
{
    auto style_cache = adopt_ref(*new StyleCache);
    style_cache->qualified_layer_names_in_order.append({});
    return style_cache;
}

NonnullRefPtr<StyleCache> StyleCache::create_for_style_scope(StyleScope& style_scope)
{
    auto style_cache = StyleCache::create();
    style_scope.populate_rule_cache(*style_cache);
    return style_cache;
}

void StyleCache::visit_edges(GC::Cell::Visitor& visitor)
{
    for (auto& cache : pseudo_class_rule_cache) {
        if (cache)
            cache->visit_edges(visitor);
    }
    author_rule_cache.visit_edges(visitor);
    user_rule_cache.visit_edges(visitor);
    user_agent_rule_cache.visit_edges(visitor);
}

void StyleScope::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_node);
    visitor.visit(m_user_style_sheet);
    if (m_rule_cache)
        m_rule_cache->visit_edges(visitor);
    visitor.visit(m_pending_has_invalidations);
}

void MatchingRule::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(rule);
    visitor.visit(sheet);
}

void RuleCache::visit_edges(GC::Cell::Visitor& visitor)
{
    auto visit_vector = [&](auto& vector) {
        for (auto& rule : vector)
            rule.visit_edges(visitor);
    };
    auto visit_map = [&](auto& map) {
        for (auto& [_, rules] : map) {
            visit_vector(rules);
        }
    };

    visit_map(rules_by_id);
    visit_map(rules_by_class);
    visit_map(rules_by_tag_name);
    visit_map(rules_by_attribute_name);
    for (auto& rules : rules_by_pseudo_element) {
        visit_vector(rules);
    }
    visit_vector(root_rules);
    visit_vector(slotted_rules);
    visit_vector(part_rules);
    visit_vector(other_rules);
}

StyleScope::StyleScope(GC::Ref<DOM::Node> node)
    : m_node(node)
{
}

void StyleScope::build_rule_cache()
{
    if (auto* shadow_root = as_if<DOM::ShadowRoot>(*m_node)) {
        GC::Ptr<CSSStyleSheet> constructed_style_sheet;
        bool saw_more_than_one_style_sheet = false;
        shadow_root->for_each_active_css_style_sheet([&](CSSStyleSheet& style_sheet) {
            if (constructed_style_sheet) {
                saw_more_than_one_style_sheet = true;
                return;
            }
            constructed_style_sheet = style_sheet;
        });

        if (constructed_style_sheet && !saw_more_than_one_style_sheet && constructed_style_sheet->constructed() && !document().page().user_style().has_value()) {
            m_rule_cache = constructed_style_sheet->shared_single_constructed_sheet_style_cache(*this);
            return;
        }
    }

    m_rule_cache = StyleCache::create();
    populate_rule_cache(*m_rule_cache);
}

void StyleScope::populate_rule_cache(StyleCache& style_cache)
{
    build_user_style_sheet_if_needed();

    build_qualified_layer_names_cache(style_cache);

    style_cache.pseudo_class_rule_cache[to_underlying(PseudoClass::Hover)] = make<RuleCache>();
    style_cache.pseudo_class_rule_cache[to_underlying(PseudoClass::Active)] = make<RuleCache>();
    style_cache.pseudo_class_rule_cache[to_underlying(PseudoClass::Focus)] = make<RuleCache>();
    style_cache.pseudo_class_rule_cache[to_underlying(PseudoClass::FocusWithin)] = make<RuleCache>();
    style_cache.pseudo_class_rule_cache[to_underlying(PseudoClass::FocusVisible)] = make<RuleCache>();
    style_cache.pseudo_class_rule_cache[to_underlying(PseudoClass::Has)] = make<RuleCache>();
    style_cache.pseudo_class_rule_cache[to_underlying(PseudoClass::Target)] = make<RuleCache>();

    make_rule_cache_for_cascade_origin(CascadeOrigin::Author, style_cache);
    make_rule_cache_for_cascade_origin(CascadeOrigin::User, style_cache);
    make_rule_cache_for_cascade_origin(CascadeOrigin::UserAgent, style_cache);
}

void StyleScope::invalidate_rule_cache()
{
    invalidate_counter_style_cache();
    m_rule_cache = nullptr;

    // NOTE: We could be smarter about keeping the user rule cache, and style sheet.
    //       Currently we are re-parsing the user style sheet every time we build the caches,
    //       as it may have changed.
    m_user_style_sheet = nullptr;
}

void StyleScope::build_user_style_sheet_if_needed()
{
    if (m_user_style_sheet)
        return;

    if (auto user_style_source = document().page().user_style(); user_style_source.has_value())
        m_user_style_sheet = GC::make_root(parse_css_stylesheet(CSS::Parser::ParsingParams(document()), user_style_source.value()));
}

void StyleScope::build_rule_cache_if_needed() const
{
    if (has_valid_rule_cache())
        return;
    const_cast<StyleScope&>(*this).build_rule_cache();
}

static CSSStyleSheet& default_stylesheet()
{
    static GC::Root<CSSStyleSheet> sheet;
    if (!sheet.cell()) {
        extern String default_stylesheet_source;
        sheet = GC::make_root(parse_css_stylesheet(CSS::Parser::ParsingParams(internal_css_realm(), Parser::IsUAStyleSheet::Yes), default_stylesheet_source));
    }
    return *sheet;
}

static CSSStyleSheet& quirks_mode_stylesheet()
{
    static GC::Root<CSSStyleSheet> sheet;
    if (!sheet.cell()) {
        extern String quirks_mode_stylesheet_source;
        sheet = GC::make_root(parse_css_stylesheet(CSS::Parser::ParsingParams(internal_css_realm(), Parser::IsUAStyleSheet::Yes), quirks_mode_stylesheet_source));
    }
    return *sheet;
}

static CSSStyleSheet& mathml_stylesheet()
{
    static GC::Root<CSSStyleSheet> sheet;
    if (!sheet.cell()) {
        extern String mathml_stylesheet_source;
        sheet = GC::make_root(parse_css_stylesheet(CSS::Parser::ParsingParams(internal_css_realm(), Parser::IsUAStyleSheet::Yes), mathml_stylesheet_source));
    }
    return *sheet;
}

static CSSStyleSheet& svg_stylesheet()
{
    static GC::Root<CSSStyleSheet> sheet;
    if (!sheet.cell()) {
        extern String svg_stylesheet_source;
        sheet = GC::make_root(parse_css_stylesheet(CSS::Parser::ParsingParams(internal_css_realm(), Parser::IsUAStyleSheet::Yes), svg_stylesheet_source));
    }
    return *sheet;
}

void StyleScope::for_each_stylesheet(CascadeOrigin cascade_origin, Function<void(CSS::CSSStyleSheet&)> const& callback) const
{
    if (cascade_origin == CascadeOrigin::UserAgent) {
        callback(default_stylesheet());
        if (document().in_quirks_mode())
            callback(quirks_mode_stylesheet());
        callback(mathml_stylesheet());
        callback(svg_stylesheet());
    }
    if (cascade_origin == CascadeOrigin::User) {
        auto& style_scope = const_cast<StyleScope&>(*this);
        style_scope.build_user_style_sheet_if_needed();
        if (style_scope.m_user_style_sheet)
            callback(*style_scope.m_user_style_sheet);
    }
    if (cascade_origin == CascadeOrigin::Author) {
        for_each_active_css_style_sheet(move(callback));
    }
}

void StyleScope::make_rule_cache_for_cascade_origin(CascadeOrigin cascade_origin, StyleCache& style_cache)
{
    Vector<MatchingRule> matching_rules;
    size_t style_sheet_index = 0;
    for_each_stylesheet(cascade_origin, [&](auto& sheet) {
        auto& rule_caches = [&] -> RuleCaches& {
            switch (cascade_origin) {
            case CascadeOrigin::Author:
                return style_cache.author_rule_cache;
            case CascadeOrigin::User:
                return style_cache.user_rule_cache;
            case CascadeOrigin::UserAgent:
                return style_cache.user_agent_rule_cache;
            default:
                VERIFY_NOT_REACHED();
            }
        }();

        size_t rule_index = 0;
        sheet.for_each_effective_style_producing_rule([&](auto const& rule) {
            SelectorList const& absolutized_selectors = [&]() {
                if (rule.type() == CSSRule::Type::Style)
                    return static_cast<CSSStyleRule const&>(rule).absolutized_selectors();
                if (rule.type() == CSSRule::Type::NestedDeclarations)
                    return static_cast<CSSNestedDeclarations const&>(rule).parent_style_rule().absolutized_selectors();
                VERIFY_NOT_REACHED();
            }();

            for (auto const& selector : absolutized_selectors) {
                style_cache.style_invalidation_data.build_invalidation_sets_for_selector(selector);
            }

            for (CSS::Selector const& selector : absolutized_selectors) {
                MatchingRule matching_rule {
                    .rule = &rule,
                    .sheet = sheet,
                    .default_namespace = sheet.default_namespace(),
                    .selector = selector,
                    .style_sheet_index = style_sheet_index,
                    .rule_index = rule_index,
                    .specificity = selector.specificity(),
                    .cascade_origin = cascade_origin,
                    .contains_pseudo_element = selector.target_pseudo_element().has_value(),
                    .slotted = selector.is_slotted(),
                    .contains_part_pseudo_element = selector.has_part_pseudo_element(),
                };

                auto const& qualified_layer_name = matching_rule.qualified_layer_name();
                auto& rule_cache = qualified_layer_name.is_empty() ? rule_caches.main : *rule_caches.by_layer.ensure(qualified_layer_name, [] { return make<RuleCache>(); });

                collect_selector_insights(selector, style_cache.selector_insights);

                bool contains_root_pseudo_class = false;
                for (auto const& simple_selector : selector.compound_selectors().last().simple_selectors) {
                    if (!contains_root_pseudo_class) {
                        if (simple_selector.type == CSS::Selector::SimpleSelector::Type::PseudoClass
                            && simple_selector.pseudo_class().type == CSS::PseudoClass::Root) {
                            contains_root_pseudo_class = true;
                        }
                    }
                }

                for (size_t i = 0; i < to_underlying(PseudoClass::__Count); ++i) {
                    auto pseudo_class = static_cast<PseudoClass>(i);
                    // If we're not building a rule cache for this pseudo class, just ignore it.
                    if (!style_cache.pseudo_class_rule_cache[i])
                        continue;
                    if (selector.contains_pseudo_class(pseudo_class)) {
                        // For pseudo class rule caches we intentionally pass no pseudo-element, because we don't want to bucket pseudo class rules by pseudo-element type.
                        style_cache.pseudo_class_rule_cache[i]->add_rule(matching_rule, {}, contains_root_pseudo_class);
                    }
                }

                rule_cache.add_rule(matching_rule, selector.target_pseudo_element().map([](auto& it) { return it.type(); }), contains_root_pseudo_class);
            }
            ++rule_index;
        });

        // Loosely based on https://drafts.csswg.org/css-animations-2/#keyframe-processing
        sheet.for_each_effective_keyframes_at_rule([&](CSSKeyframesRule const& rule) {
            auto keyframe_set = adopt_ref(*new Animations::KeyframeEffect::KeyFrameSet);
            HashTable<PropertyID> animated_properties;

            // Forwards pass, resolve all the user-specified keyframe properties.
            for (auto const& keyframe_rule : *rule.css_rules()) {
                auto const& keyframe = as<CSSKeyframeRule>(*keyframe_rule);
                Animations::KeyframeEffect::KeyFrameSet::ResolvedKeyFrame resolved_keyframe;

                auto key = static_cast<u64>(keyframe.key().value() * Animations::KeyframeEffect::AnimationKeyFrameKeyScaleFactor);
                auto const& keyframe_style = *keyframe.style();
                for (auto const& it : keyframe_style.properties()) {
                    if (it.property_id == PropertyID::AnimationTimingFunction) {
                        // animation-timing-function is a list property, but inside @keyframes only
                        // a single value is meaningful.
                        NonnullRefPtr<StyleValue const> easing_value = it.value;
                        if (easing_value->is_value_list()) {
                            auto const& list = easing_value->as_value_list();
                            if (list.size() > 0)
                                easing_value = list.value_at(0, false);
                            else
                                continue;
                        }
                        if (easing_value->is_easing() || easing_value->is_keyword())
                            resolved_keyframe.easing = EasingFunction::from_style_value(*easing_value);
                        else
                            resolved_keyframe.easing = easing_value;
                        continue;
                    }
                    if (it.property_id == PropertyID::AnimationComposition) {
                        auto composition_str = it.value->to_string(SerializationMode::Normal);
                        AnimationComposition composition = AnimationComposition::Replace;
                        if (composition_str == "add"sv)
                            composition = AnimationComposition::Add;
                        else if (composition_str == "accumulate"sv)
                            composition = AnimationComposition::Accumulate;
                        resolved_keyframe.composite = Animations::css_animation_composition_to_bindings_composite_operation_or_auto(composition);
                        continue;
                    }
                    if (!is_animatable_property(it.property_id))
                        continue;

                    // Unresolved properties will be resolved in collect_animation_into()
                    StyleComputer::for_each_property_expanding_shorthands(it.property_id, it.value, [&](PropertyID shorthand_id, StyleValue const& shorthand_value) {
                        animated_properties.set(shorthand_id);
                        resolved_keyframe.properties.set(shorthand_id, NonnullRefPtr<StyleValue const> { shorthand_value });
                    });
                }

                if (auto* existing_keyframe = keyframe_set->keyframes_by_key.find(key)) {
                    for (auto& [property_id, value] : resolved_keyframe.properties)
                        existing_keyframe->properties.set(property_id, move(value));
                    if (resolved_keyframe.composite != Bindings::CompositeOperationOrAuto::Auto)
                        existing_keyframe->composite = resolved_keyframe.composite;
                    if (!resolved_keyframe.easing.has<Empty>())
                        existing_keyframe->easing = move(resolved_keyframe.easing);
                } else {
                    keyframe_set->keyframes_by_key.insert(key, resolved_keyframe);
                }
            }

            Animations::KeyframeEffect::generate_initial_and_final_frames(keyframe_set, animated_properties);

            if constexpr (LIBWEB_CSS_DEBUG) {
                dbgln("Resolved keyframe set '{}' into {} keyframes:", rule.name(), keyframe_set->keyframes_by_key.size());
                for (auto it = keyframe_set->keyframes_by_key.begin(); it != keyframe_set->keyframes_by_key.end(); ++it)
                    dbgln("    - keyframe {}: {} properties", it.key(), it->properties.size());
            }

            rule_caches.main.rules_by_animation_keyframes.set(rule.name(), move(keyframe_set));
        });
        ++style_sheet_index;
    });
}

void StyleScope::collect_selector_insights(Selector const& selector, SelectorInsights& insights)
{
    for (auto const& compound_selector : selector.compound_selectors()) {
        for (auto const& simple_selector : compound_selector.simple_selectors) {
            if (simple_selector.type == Selector::SimpleSelector::Type::PseudoClass) {
                if (simple_selector.pseudo_class().type == PseudoClass::Has) {
                    insights.has_has_selectors = true;
                    for (auto const& argument_selector : simple_selector.pseudo_class().argument_selector_list) {
                        for (auto const& relative_compound_selector : argument_selector->compound_selectors()) {
                            if (relative_compound_selector.combinator == Selector::Combinator::NextSibling
                                || relative_compound_selector.combinator == Selector::Combinator::SubsequentSibling) {
                                insights.has_has_selectors_with_relative_selector_that_has_sibling_combinator = true;
                                break;
                            }
                        }
                    }
                }
                for (auto const& argument_selector : simple_selector.pseudo_class().argument_selector_list) {
                    collect_selector_insights(*argument_selector, insights);
                }
            } else if (simple_selector.type == Selector::SimpleSelector::Type::PseudoElement) {
                // Pseudo-elements like ::slotted(...) carry a compound selector argument whose contents need the
                // same insight collection pass.
                auto const& pseudo_element = simple_selector.pseudo_element();
                if (pseudo_element.type() == PseudoElement::Slotted)
                    collect_selector_insights(pseudo_element.compound_selector(), insights);
            }
        }
    }
}

struct LayerNode {
    OrderedHashMap<FlyString, LayerNode> children {};
};

static void flatten_layer_names_tree(Vector<FlyString>& layer_names, StringView const& parent_qualified_name, FlyString const& name, LayerNode const& node)
{
    FlyString qualified_name = parent_qualified_name.is_empty() ? name : MUST(String::formatted("{}.{}", parent_qualified_name, name));

    for (auto const& item : node.children)
        flatten_layer_names_tree(layer_names, qualified_name, item.key, item.value);

    layer_names.append(qualified_name);
}

void StyleScope::build_qualified_layer_names_cache(StyleCache& style_cache)
{
    LayerNode root;

    auto insert_layer_name = [&](FlyString const& internal_qualified_name) {
        auto* node = &root;
        internal_qualified_name.bytes_as_string_view()
            .for_each_split_view('.', SplitBehavior::Nothing, [&](StringView part) {
                auto local_name = MUST(FlyString::from_utf8(part));
                node = &node->children.ensure(local_name);
            });
    };

    // Walk all style sheets, identifying when we first see a @layer name, and add its qualified name to the list.
    // TODO: Separate the light and shadow-dom layers.
    for_each_stylesheet(CascadeOrigin::Author, [&](auto& sheet) {
        // NOTE: Postorder so that a @layer block is iterated after its children,
        // because we want those children to occur before it in the list.
        sheet.for_each_effective_rule(TraversalOrder::Postorder, [&](auto& rule) {
            switch (rule.type()) {
            case CSSRule::Type::Import: {
                auto& import = as<CSSImportRule>(rule);
                // https://drafts.csswg.org/css-cascade-5/#at-import
                // The layer is added to the layer order even if the import fails to load the stylesheet, but is
                // subject to any import conditions (just as if declared by an @layer rule wrapped in the appropriate
                // conditional group rules).
                if (auto layer_name = import.internal_qualified_layer_name({}); layer_name.has_value() && import.matches())
                    insert_layer_name(layer_name.release_value());
                break;
            }
            case CSSRule::Type::LayerBlock: {
                auto& layer_block = as<CSSLayerBlockRule>(rule);
                insert_layer_name(layer_block.internal_qualified_name({}));
                break;
            }
            case CSSRule::Type::LayerStatement: {
                auto& layer_statement = as<CSSLayerStatementRule>(rule);
                auto qualified_names = layer_statement.internal_qualified_name_list({});
                for (auto& name : qualified_names)
                    insert_layer_name(name);
                break;
            }

                // Ignore everything else
            case CSSRule::Type::Style:
            case CSSRule::Type::Media:
            case CSSRule::Type::Container:
            case CSSRule::Type::CounterStyle:
            case CSSRule::Type::FontFace:
            case CSSRule::Type::FontFeatureValues:
            case CSSRule::Type::Function:
            case CSSRule::Type::FunctionDeclarations:
            case CSSRule::Type::Keyframes:
            case CSSRule::Type::Keyframe:
            case CSSRule::Type::Margin:
            case CSSRule::Type::Namespace:
            case CSSRule::Type::NestedDeclarations:
            case CSSRule::Type::Page:
            case CSSRule::Type::Property:
            case CSSRule::Type::Supports:
                break;
            }
        });
    });

    // Now, produce a flat list of qualified names to use later
    style_cache.qualified_layer_names_in_order.clear();
    flatten_layer_names_tree(style_cache.qualified_layer_names_in_order, ""sv, {}, root);
}

void StyleScope::invalidate_counter_style_cache()
{
    m_needs_counter_style_cache_update = true;

    // FIXME: We only need to invalidate this style scope and those belonging to descendant shadow roots (since they may
    //        include counter styles which extend the ones defined in this scope), not all style scopes in the document.
    m_node->document().style_scope().m_needs_counter_style_cache_update = true;
    m_node->document().for_each_shadow_root([&](DOM::ShadowRoot& shadow_root) {
        shadow_root.style_scope().m_needs_counter_style_cache_update = true;
    });
}

void StyleScope::build_counter_style_cache()
{
    m_is_doing_counter_style_cache_update = true;

    m_registered_counter_styles.clear_with_capacity();

    HashMap<FlyString, CSS::CounterStyleDefinition> counter_style_definitions;

    auto const define_complex_predefined_counter_styles = [&]() {
        // https://drafts.csswg.org/css-counter-styles-3/#complex-predefined-counters
        // While authors may define their own counter styles using the @counter-style rule or rely on the set of
        // predefined counter styles, a few counter styles are described by rules that are too complex to be captured by
        // the predefined algorithms.

        // FIXME: All of the counter styles defined in this section have a spoken form of numbers

        // https://drafts.csswg.org/css-counter-styles-3/#ethiopic-numeric-counter-style
        // For this system, the name is "ethiopic-numeric", the range is 1 infinite, the suffix is "/ " (U+002F SOLIDUS
        // followed by a U+0020 SPACE), and the rest of the descriptors have their initial value.
        counter_style_definitions.set(
            "ethiopic-numeric"_fly_string,
            CSS::CounterStyleDefinition::create(
                "ethiopic-numeric"_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::EthiopicNumericCounterStyleAlgorithm {} },
                {},
                {},
                "/ "_fly_string,
                Vector<CSS::CounterStyleRangeEntry> { { 1, AK::NumericLimits<i32>::max() } },
                {},
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#extended-range-optional
        // For all of these counter styles, the descriptors are the same as for the limited range variants, except for
        // the range, which is calc(-1 * pow(10, 16) + 1) calc(pow(10, 16) - 1).
        // AD-HOC: Ranges (as with all other CSS <integer>s are limited to i32 range)
        Vector<CSS::CounterStyleRangeEntry> extended_cjk_range { { AK::clamp_to<i32>(-9999999999999999), AK::clamp_to<i32>(9999999999999999) } };

        // https://drafts.csswg.org/css-counter-styles-3/#limited-chinese
        // For all of these counter styles, the suffix is "、" U+3001, the fallback is cjk-decimal, the range is -9999
        // 9999, and the negative value is given in the table of symbols for each style.

        //                  simp-chinese-informal simp-chinese-formal trad-chinese-informal trad-chinese-formal
        // Negative Sign    负 U+8D1F             负 U+8D1F           負 U+8CA0              負 U+8CA0

        // https://drafts.csswg.org/css-counter-styles-3/#simp-chinese-informal
        // simp-chinese-informal
        counter_style_definitions.set(
            "simp-chinese-informal"_fly_string,
            CSS::CounterStyleDefinition::create(
                "simp-chinese-informal"_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::SimpChineseInformal } },
                CSS::CounterStyleNegativeSign { "\U00008D1F"_fly_string, ""_fly_string },
                {},
                "\U00003001"_fly_string,
                extended_cjk_range,
                "cjk-decimal"_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#simp-chinese-formal
        // simp-chinese-formal
        counter_style_definitions.set(
            "simp-chinese-formal"_fly_string,
            CSS::CounterStyleDefinition::create(
                "simp-chinese-formal"_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::SimpChineseFormal } },
                CSS::CounterStyleNegativeSign { "\U00008D1F"_fly_string, ""_fly_string },
                {},
                "\U00003001"_fly_string,
                extended_cjk_range,
                "cjk-decimal"_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#trad-chinese-informal
        // trad-chinese-informal
        counter_style_definitions.set(
            "trad-chinese-informal"_fly_string,
            CSS::CounterStyleDefinition::create(
                "trad-chinese-informal"_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::TradChineseInformal } },
                CSS::CounterStyleNegativeSign { "\U00008CA0"_fly_string, ""_fly_string },
                {},
                "\U00003001"_fly_string,
                extended_cjk_range,
                "cjk-decimal"_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#trad-chinese-formal
        // trad-chinese-formal
        counter_style_definitions.set(
            "trad-chinese-formal"_fly_string,
            CSS::CounterStyleDefinition::create(
                "trad-chinese-formal"_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::TradChineseFormal } },
                CSS::CounterStyleNegativeSign { "\U00008CA0"_fly_string, ""_fly_string },
                {},
                "\U00003001"_fly_string,
                extended_cjk_range,
                "cjk-decimal"_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#cjk-ideographic
        // cjk-ideographic
        // This counter style is identical to trad-chinese-informal. (It exists for legacy reasons.)
        counter_style_definitions.set(
            "cjk-ideographic"_fly_string,
            CSS::CounterStyleDefinition::create(
                "cjk-ideographic"_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::TradChineseInformal } },
                CSS::CounterStyleNegativeSign { "\U00008CA0"_fly_string, ""_fly_string },
                {},
                "\U00003001"_fly_string,
                extended_cjk_range,
                "cjk-decimal"_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#japanese-informal
        // japanese-informal
        counter_style_definitions.set(
            "japanese-informal"_fly_string,
            CSS::CounterStyleDefinition::create(
                "japanese-informal"_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::JapaneseInformal } },
                CSS::CounterStyleNegativeSign { "\U000030DE\U000030A4\U000030CA\U000030B9"_fly_string, ""_fly_string },
                {},
                "\U00003001"_fly_string,
                extended_cjk_range,
                "cjk-decimal"_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#japanese-formal
        // japanese-formal
        counter_style_definitions.set(
            "japanese-formal"_fly_string,
            CSS::CounterStyleDefinition::create(
                "japanese-formal"_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::JapaneseFormal } },
                CSS::CounterStyleNegativeSign { "\U000030DE\U000030A4\U000030CA\U000030B9"_fly_string, ""_fly_string },
                {},
                "\U00003001"_fly_string,
                extended_cjk_range,
                "cjk-decimal"_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#korean-hangul-formal
        // korean-hangul-formal
        counter_style_definitions.set(
            "korean-hangul-formal"_fly_string,
            CSS::CounterStyleDefinition::create(
                "korean-hangul-formal"_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::KoreanHangulFormal } },
                CSS::CounterStyleNegativeSign { "\U0000B9C8\U0000C774\U0000B108\U0000C2A4 "_fly_string, ""_fly_string },
                {},
                ", "_fly_string,
                extended_cjk_range,
                "cjk-decimal"_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#korean-hanja-informal
        // korean-hanja-informal
        counter_style_definitions.set(
            "korean-hanja-informal"_fly_string,
            CSS::CounterStyleDefinition::create(
                "korean-hanja-informal"_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::KoreanHanjaInformal } },
                CSS::CounterStyleNegativeSign { "\U0000B9C8\U0000C774\U0000B108\U0000C2A4 "_fly_string, ""_fly_string },
                {},
                ", "_fly_string,
                extended_cjk_range,
                "cjk-decimal"_fly_string,
                {}));

        // https://drafts.csswg.org/css-counter-styles-3/#korean-hanja-formal
        // korean-hanja-formal
        counter_style_definitions.set(
            "korean-hanja-formal"_fly_string,
            CSS::CounterStyleDefinition::create(
                "korean-hanja-formal"_fly_string,
                CSS::CounterStyleAlgorithmOrExtends { CSS::ExtendedCJKCounterStyleAlgorithm { CSS::ExtendedCJKCounterStyleAlgorithm::Type::KoreanHanjaFormal } },
                CSS::CounterStyleNegativeSign { "\U0000B9C8\U0000C774\U0000B108\U0000C2A4 "_fly_string, ""_fly_string },
                {},
                ", "_fly_string,
                extended_cjk_range,
                "cjk-decimal"_fly_string,
                {}));
    };

    CSS::ComputationContext computation_context {
        .length_resolution_context = CSS::Length::ResolutionContext::for_document(document())
    };

    Function<void(CSS::CSSStyleSheet&)> const collect_counter_style_definitions = [&](CSS::CSSStyleSheet const& style_sheet) {
        style_sheet.for_each_effective_counter_style_at_rule([&](CSS::CSSCounterStyleRule const& counter_style_rule) {
            if (auto const& definition = CSS::CounterStyleDefinition::from_counter_style_rule(counter_style_rule, computation_context); definition.has_value())
                counter_style_definitions.set(definition->name(), *definition);
        });
    };

    // NB: We should only register predefined counter styles in the document's style scope, this ensures overrides are
    //     correctly inherited by shadow roots.
    if (m_node->is_document()) {
        for_each_stylesheet(CSS::CascadeOrigin::UserAgent, collect_counter_style_definitions);
        define_complex_predefined_counter_styles();
        for_each_stylesheet(CSS::CascadeOrigin::User, collect_counter_style_definitions);
    }

    for_each_stylesheet(CSS::CascadeOrigin::Author, collect_counter_style_definitions);

    VERIFY(!m_node->is_document() || counter_style_definitions.contains("decimal"_fly_string));

    auto const is_part_of_extends_cycle = [&](FlyString const& counter_style_name) {
        HashTable<FlyString> visited;
        auto current_counter_style_name = counter_style_name;

        while (true) {
            if (visited.contains(current_counter_style_name))
                return true;

            visited.set(current_counter_style_name);

            auto const& current_definition = counter_style_definitions.get(current_counter_style_name);

            // NB: If we don't have a definition for this counter style it means it's either undefined in this scope
            //     (and will the counter style extending it will instead default to extending "decimal" instead) or it's
            //     defined in an outer style scope (and thus can't extend a counter style in the current scope), neither
            //     of which can lead to a cycle.
            if (!current_definition.has_value())
                return false;

            if (current_definition->algorithm().has<CSS::CounterStyleAlgorithm>())
                return false;

            current_counter_style_name = current_definition->algorithm().get<CSS::CounterStyleSystemStyleValue::Extends>().name;
        }

        VERIFY_NOT_REACHED();
    };

    // NB: We register non-extending counter styles immediately and then extending counter styles after we have
    //     registered their corresponding extended counter style.
    Vector<CSS::CounterStyleDefinition> extending_counter_styles;

    for (auto const& [name, definition] : counter_style_definitions) {
        // NB: We don't need to wait for this counter style's extended counter style to be registered since it doesn't
        //     have one - register it immediately.
        if (definition.algorithm().has<CSS::CounterStyleAlgorithm>()) {
            m_registered_counter_styles.set(name, CSS::CounterStyle::from_counter_style_definition(definition, *this));
            continue;
        }

        auto extends = definition.algorithm().get<CSS::CounterStyleSystemStyleValue::Extends>();

        if (is_part_of_extends_cycle(name)) {
            auto copied = definition;
            copied.set_algorithm(CSS::CounterStyleSystemStyleValue::Extends { "decimal"_fly_string });
            extending_counter_styles.append(copied);
        } else {
            extending_counter_styles.append(definition);
        }
    }

    // FIXME: This is O(n^2) in the worst case but we usually don't see many counter styles so it should be fine in practice.
    while (!extending_counter_styles.is_empty()) {
        for (size_t i = 0; i < extending_counter_styles.size(); ++i) {
            auto const& definition = extending_counter_styles.at(i);
            auto extends = definition.algorithm().get<CSS::CounterStyleSystemStyleValue::Extends>();

            if (!m_registered_counter_styles.contains(extends.name) && counter_style_definitions.contains(extends.name))
                continue;

            m_registered_counter_styles.set(definition.name(), CSS::CounterStyle::from_counter_style_definition(definition, *this));
            extending_counter_styles.remove(i);
            --i;
        }
    }

    m_is_doing_counter_style_cache_update = false;
    m_needs_counter_style_cache_update = false;
}

bool StyleScope::may_have_has_selectors() const
{
    if (!has_valid_rule_cache())
        return true;

    build_rule_cache_if_needed();
    return m_rule_cache->selector_insights.has_has_selectors;
}

bool StyleScope::have_has_selectors() const
{
    build_rule_cache_if_needed();
    return m_rule_cache->selector_insights.has_has_selectors;
}

bool StyleScope::may_have_has_selectors_with_relative_selector_that_has_sibling_combinator() const
{
    if (!has_valid_rule_cache())
        return true;

    build_rule_cache_if_needed();
    return m_rule_cache->selector_insights.has_has_selectors_with_relative_selector_that_has_sibling_combinator;
}

bool StyleScope::have_has_selectors_with_relative_selector_that_has_sibling_combinator() const
{
    build_rule_cache_if_needed();
    return m_rule_cache->selector_insights.has_has_selectors_with_relative_selector_that_has_sibling_combinator;
}

DOM::Document& StyleScope::document() const
{
    return m_node->document();
}

RuleCache const& StyleScope::get_pseudo_class_rule_cache(PseudoClass pseudo_class) const
{
    build_rule_cache_if_needed();
    return *m_rule_cache->pseudo_class_rule_cache[to_underlying(pseudo_class)];
}

void StyleScope::for_each_active_css_style_sheet(Function<void(CSS::CSSStyleSheet&)> const& callback) const
{
    if (auto* shadow_root = as_if<DOM::ShadowRoot>(*m_node)) {
        shadow_root->for_each_active_css_style_sheet(callback);
    } else {
        m_node->document().for_each_active_css_style_sheet(callback);
    }
}

void StyleScope::schedule_ancestors_style_invalidation_due_to_presence_of_has(GC::Ref<DOM::Node> node)
{
    auto previous_size = m_pending_has_invalidations.size();
    auto& mutation_features = m_pending_has_invalidations.ensure(node);
    if (m_pending_has_invalidations.size() == previous_size)
        return;
    mutation_features.is_conservative = true;
    document().set_needs_invalidation_of_elements_affected_by_has();
}

static void merge_pending_has_invalidation_mutation_features(PendingHasInvalidationMutationFeatures& target, PendingHasInvalidationMutationFeatures const& source)
{
    target.is_conservative |= source.is_conservative;
    target.may_affect_sibling_relationships |= source.may_affect_sibling_relationships;
    target.may_affect_pseudo_classes |= source.may_affect_pseudo_classes;
    for (auto const& tag_name : source.tag_names)
        target.tag_names.set(tag_name);
    for (auto const& id : source.ids)
        target.ids.set(id);
    for (auto const& class_name : source.class_names)
        target.class_names.set(class_name);
    for (auto const& attribute_name : source.attribute_names)
        target.attribute_names.set(attribute_name);
    for (auto const& pseudo_class : source.pseudo_classes)
        target.pseudo_classes.set(pseudo_class);
}

static void collect_pending_has_invalidation_features_from_element(PendingHasInvalidationMutationFeatures& features, DOM::Element const& element)
{
    features.tag_names.set(element.local_name());
    if (element.namespace_uri() != Namespace::HTML)
        features.tag_names.set(element.lowercased_local_name());

    if (auto id = element.id(); id.has_value())
        features.ids.set(*id);

    for (auto const& class_name : element.class_names())
        features.class_names.set(class_name);

    element.for_each_attribute([&](FlyString const& name, String const&) {
        features.attribute_names.set(name);
        if (element.namespace_uri() != Namespace::HTML)
            features.attribute_names.set(name.to_ascii_lowercase());
    });
}

static PendingHasInvalidationMutationFeatures collect_pending_has_invalidation_mutation_features(DOM::Node& mutation_root, bool includes_descendants)
{
    PendingHasInvalidationMutationFeatures features;
    features.may_affect_sibling_relationships = includes_descendants;
    features.may_affect_pseudo_classes = true;
    auto collect_node = [&](DOM::Node& node) {
        if (node.is_character_data())
            return;
        if (auto* element = as_if<DOM::Element>(node))
            collect_pending_has_invalidation_features_from_element(features, *element);
    };

    if (!includes_descendants) {
        collect_node(mutation_root);
        return features;
    }

    mutation_root.for_each_in_inclusive_subtree([&](DOM::Node& node) {
        collect_node(node);
        return TraversalDecision::Continue;
    });
    return features;
}

static PendingHasInvalidationMutationFeatures collect_pending_has_invalidation_mutation_features(Vector<CSS::InvalidationSet::Property> const& properties)
{
    PendingHasInvalidationMutationFeatures features;
    for (auto const& property : properties) {
        switch (property.type) {
        case InvalidationSet::Property::Type::Class:
            features.class_names.set(property.name());
            break;
        case InvalidationSet::Property::Type::Id:
            features.ids.set(property.name());
            break;
        case InvalidationSet::Property::Type::TagName:
            features.tag_names.set(property.name());
            break;
        case InvalidationSet::Property::Type::Attribute:
            features.attribute_names.set(property.name());
            break;
        case InvalidationSet::Property::Type::InvalidateSelf:
        case InvalidationSet::Property::Type::InvalidateWholeSubtree:
            features.is_conservative = true;
            break;
        case InvalidationSet::Property::Type::PseudoClass:
            features.pseudo_classes.set(property.value.get<PseudoClass>());
            break;
        }
    }
    return features;
}

void StyleScope::record_pending_has_invalidation_mutation_features(GC::Ref<DOM::Node> scheduled_node, GC::Ref<DOM::Node> mutation_root, bool includes_descendants)
{
    auto features = collect_pending_has_invalidation_mutation_features(*mutation_root, includes_descendants);
    auto previous_size = m_pending_has_invalidations.size();
    auto& existing_features = m_pending_has_invalidations.ensure(scheduled_node);
    if (m_pending_has_invalidations.size() == previous_size) {
        merge_pending_has_invalidation_mutation_features(existing_features, features);
        return;
    }
    existing_features = move(features);
    document().set_needs_invalidation_of_elements_affected_by_has();
}

void StyleScope::record_pending_has_invalidation_mutation_features(GC::Ref<DOM::Node> scheduled_node, Vector<CSS::InvalidationSet::Property> const& properties)
{
    auto features = collect_pending_has_invalidation_mutation_features(properties);
    auto previous_size = m_pending_has_invalidations.size();
    auto& existing_features = m_pending_has_invalidations.ensure(scheduled_node);
    if (m_pending_has_invalidations.size() == previous_size) {
        merge_pending_has_invalidation_mutation_features(existing_features, features);
        return;
    }
    existing_features = move(features);
    document().set_needs_invalidation_of_elements_affected_by_has();
}

RefPtr<CSS::CounterStyle const> StyleScope::get_registered_counter_style(FlyString const& name) const
{
    if (m_needs_counter_style_cache_update && !m_is_doing_counter_style_cache_update)
        const_cast<StyleScope*>(this)->build_counter_style_cache();

    return dereference_global_tree_scoped_reference<CSS::CounterStyle const*>([&](StyleScope const& scope) { return scope.m_registered_counter_styles.get(name); })
        .value_or(nullptr);
}

template<typename T>
Optional<T> StyleScope::dereference_global_tree_scoped_reference(Function<Optional<T>(StyleScope const&)> const& callback) const
{
    // https://drafts.csswg.org/css-shadow-1/#tree-scoped-name-global
    // If a tree-scoped name is global (such as @font-face names), then when a tree-scoped reference is dereferenced to
    // find it, first search only the tree-scoped names associated with the same root as the tree-scoped reference. If
    // no relevant tree-scoped name is found, and the root is a shadow root, then repeat this search in the root’s
    // host’s node tree (recursively). (In other words, global tree-scoped names “inherit” into descendant shadow trees,
    // so long as they don’t define the same name themselves.)
    if (auto result = callback(*this); result.has_value())
        return result;

    if (auto* shadow_root = as_if<DOM::ShadowRoot>(*m_node)) {
        if (auto* host = shadow_root->host()) {
            auto const& root = host->root();

            if (root.is_shadow_root()) {
                auto const& shadow_root = as<DOM::ShadowRoot>(root);
                if (shadow_root.uses_document_style_sheets())
                    return root.document().style_scope().dereference_global_tree_scoped_reference(callback);

                return shadow_root.style_scope().dereference_global_tree_scoped_reference(callback);
            }

            return as<DOM::Document>(root).style_scope().dereference_global_tree_scoped_reference(callback);
        }
    }

    return {};
}

}
