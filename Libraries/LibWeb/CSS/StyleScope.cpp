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
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Page/Page.h>

namespace Web::CSS {

void RuleCaches::visit_edges(GC::Cell::Visitor& visitor)
{
    main.visit_edges(visitor);
    for (auto& it : by_layer) {
        it.value->visit_edges(visitor);
    }
}

void StyleScope::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_node);
    visitor.visit(m_user_style_sheet);
    for (auto& cache : m_pseudo_class_rule_cache) {
        if (cache)
            cache->visit_edges(visitor);
    }
    if (m_author_rule_cache)
        m_author_rule_cache->visit_edges(visitor);
    if (m_user_rule_cache)
        m_user_rule_cache->visit_edges(visitor);
    if (m_user_agent_rule_cache)
        m_user_agent_rule_cache->visit_edges(visitor);
}

void MatchingRule::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(shadow_root);
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
    m_qualified_layer_names_in_order.append({});
}

void StyleScope::build_rule_cache()
{
    m_author_rule_cache = make<RuleCaches>();
    m_user_rule_cache = make<RuleCaches>();
    m_user_agent_rule_cache = make<RuleCaches>();

    m_selector_insights = make<SelectorInsights>();
    m_style_invalidation_data = make<StyleInvalidationData>();

    if (auto user_style_source = document().page().user_style(); user_style_source.has_value()) {
        m_user_style_sheet = GC::make_root(parse_css_stylesheet(CSS::Parser::ParsingParams(document()), user_style_source.value()));
    }

    build_qualified_layer_names_cache();

    m_pseudo_class_rule_cache[to_underlying(PseudoClass::Hover)] = make<RuleCache>();
    m_pseudo_class_rule_cache[to_underlying(PseudoClass::Active)] = make<RuleCache>();
    m_pseudo_class_rule_cache[to_underlying(PseudoClass::Focus)] = make<RuleCache>();
    m_pseudo_class_rule_cache[to_underlying(PseudoClass::FocusWithin)] = make<RuleCache>();
    m_pseudo_class_rule_cache[to_underlying(PseudoClass::FocusVisible)] = make<RuleCache>();
    m_pseudo_class_rule_cache[to_underlying(PseudoClass::Target)] = make<RuleCache>();

    make_rule_cache_for_cascade_origin(CascadeOrigin::Author, *m_selector_insights);
    make_rule_cache_for_cascade_origin(CascadeOrigin::User, *m_selector_insights);
    make_rule_cache_for_cascade_origin(CascadeOrigin::UserAgent, *m_selector_insights);
}

void StyleScope::invalidate_rule_cache()
{
    m_author_rule_cache = nullptr;

    // NOTE: We could be smarter about keeping the user rule cache, and style sheet.
    //       Currently we are re-parsing the user style sheet every time we build the caches,
    //       as it may have changed.
    m_user_rule_cache = nullptr;
    m_user_style_sheet = nullptr;

    // NOTE: It might not be necessary to throw away the UA rule cache.
    //       If we are sure that it's safe, we could keep it as an optimization.
    m_user_agent_rule_cache = nullptr;

    m_pseudo_class_rule_cache = {};
    m_style_invalidation_data = nullptr;
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
        sheet = GC::make_root(parse_css_stylesheet(CSS::Parser::ParsingParams(internal_css_realm()), default_stylesheet_source));
    }
    return *sheet;
}

static CSSStyleSheet& quirks_mode_stylesheet()
{
    static GC::Root<CSSStyleSheet> sheet;
    if (!sheet.cell()) {
        extern String quirks_mode_stylesheet_source;
        sheet = GC::make_root(parse_css_stylesheet(CSS::Parser::ParsingParams(internal_css_realm()), quirks_mode_stylesheet_source));
    }
    return *sheet;
}

static CSSStyleSheet& mathml_stylesheet()
{
    static GC::Root<CSSStyleSheet> sheet;
    if (!sheet.cell()) {
        extern String mathml_stylesheet_source;
        sheet = GC::make_root(parse_css_stylesheet(CSS::Parser::ParsingParams(internal_css_realm()), mathml_stylesheet_source));
    }
    return *sheet;
}

static CSSStyleSheet& svg_stylesheet()
{
    static GC::Root<CSSStyleSheet> sheet;
    if (!sheet.cell()) {
        extern String svg_stylesheet_source;
        sheet = GC::make_root(parse_css_stylesheet(CSS::Parser::ParsingParams(internal_css_realm()), svg_stylesheet_source));
    }
    return *sheet;
}

template<typename Callback>
void StyleScope::for_each_stylesheet(CascadeOrigin cascade_origin, Callback callback) const
{
    if (cascade_origin == CascadeOrigin::UserAgent) {
        callback(default_stylesheet());
        if (document().in_quirks_mode())
            callback(quirks_mode_stylesheet());
        callback(mathml_stylesheet());
        callback(svg_stylesheet());
    }
    if (cascade_origin == CascadeOrigin::User) {
        if (m_user_style_sheet)
            callback(*m_user_style_sheet);
    }
    if (cascade_origin == CascadeOrigin::Author) {
        for_each_active_css_style_sheet(move(callback));
    }
}

void StyleScope::make_rule_cache_for_cascade_origin(CascadeOrigin cascade_origin, SelectorInsights& insights)
{
    GC::Ptr<DOM::ShadowRoot const> scope_shadow_root;
    if (m_node->is_shadow_root())
        scope_shadow_root = as<DOM::ShadowRoot>(*m_node);

    Vector<MatchingRule> matching_rules;
    size_t style_sheet_index = 0;
    for_each_stylesheet(cascade_origin, [&](auto& sheet) {
        auto& rule_caches = [&] -> RuleCaches& {
            switch (cascade_origin) {
            case CascadeOrigin::Author:
                return *m_author_rule_cache;
            case CascadeOrigin::User:
                return *m_user_rule_cache;
            case CascadeOrigin::UserAgent:
                return *m_user_agent_rule_cache;
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
                m_style_invalidation_data->build_invalidation_sets_for_selector(selector);
            }

            for (CSS::Selector const& selector : absolutized_selectors) {
                MatchingRule matching_rule {
                    scope_shadow_root,
                    &rule,
                    sheet,
                    sheet.default_namespace(),
                    selector,
                    style_sheet_index,
                    rule_index,
                    selector.specificity(),
                    cascade_origin,
                    false,
                };

                auto const& qualified_layer_name = matching_rule.qualified_layer_name();
                auto& rule_cache = qualified_layer_name.is_empty() ? rule_caches.main : *rule_caches.by_layer.ensure(qualified_layer_name, [] { return make<RuleCache>(); });

                bool contains_root_pseudo_class = false;
                Optional<CSS::PseudoElement> pseudo_element;

                collect_selector_insights(selector, insights);

                for (auto const& simple_selector : selector.compound_selectors().last().simple_selectors) {
                    if (!matching_rule.contains_pseudo_element) {
                        if (simple_selector.type == CSS::Selector::SimpleSelector::Type::PseudoElement) {
                            matching_rule.contains_pseudo_element = true;
                            // FIXME: This wrongly assumes there is only one pseudo-element per selector.
                            pseudo_element = simple_selector.pseudo_element().type();
                            matching_rule.slotted = pseudo_element == PseudoElement::Slotted;
                            matching_rule.contains_part_pseudo_element = pseudo_element == PseudoElement::Part;
                        }
                    }
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
                    if (!m_pseudo_class_rule_cache[i])
                        continue;
                    if (selector.contains_pseudo_class(pseudo_class)) {
                        // For pseudo class rule caches we intentionally pass no pseudo-element, because we don't want to bucket pseudo class rules by pseudo-element type.
                        m_pseudo_class_rule_cache[i]->add_rule(matching_rule, {}, contains_root_pseudo_class);
                    }
                }

                rule_cache.add_rule(matching_rule, pseudo_element, contains_root_pseudo_class);
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

                keyframe_set->keyframes_by_key.insert(key, resolved_keyframe);
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
                }
                for (auto const& argument_selector : simple_selector.pseudo_class().argument_selector_list) {
                    collect_selector_insights(*argument_selector, insights);
                }
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

void StyleScope::build_qualified_layer_names_cache()
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
            case CSSRule::Type::CounterStyle:
            case CSSRule::Type::FontFace:
            case CSSRule::Type::FontFeatureValues:
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
    m_qualified_layer_names_in_order.clear();
    flatten_layer_names_tree(m_qualified_layer_names_in_order, ""sv, {}, root);
}

bool StyleScope::may_have_has_selectors() const
{
    if (!has_valid_rule_cache())
        return true;

    build_rule_cache_if_needed();
    return m_selector_insights->has_has_selectors;
}

bool StyleScope::have_has_selectors() const
{
    build_rule_cache_if_needed();
    return m_selector_insights->has_has_selectors;
}

DOM::Document& StyleScope::document() const
{
    return m_node->document();
}

RuleCache const& StyleScope::get_pseudo_class_rule_cache(PseudoClass pseudo_class) const
{
    build_rule_cache_if_needed();
    return *m_pseudo_class_rule_cache[to_underlying(pseudo_class)];
}

void StyleScope::for_each_active_css_style_sheet(Function<void(CSS::CSSStyleSheet&)> const& callback) const
{
    if (auto* shadow_root = as_if<DOM::ShadowRoot>(*m_node)) {
        shadow_root->for_each_active_css_style_sheet(callback);
    } else {
        m_node->document().for_each_active_css_style_sheet(callback);
    }
}

void StyleScope::schedule_ancestors_style_invalidation_due_to_presence_of_has(DOM::Node& node)
{
    m_pending_nodes_for_style_invalidation_due_to_presence_of_has.set(node);
    document().set_needs_invalidation_of_elements_affected_by_has();
}

void StyleScope::invalidate_style_of_elements_affected_by_has()
{
    if (m_pending_nodes_for_style_invalidation_due_to_presence_of_has.is_empty()) {
        return;
    }

    ScopeGuard clear_pending_nodes_guard = [&] {
        m_pending_nodes_for_style_invalidation_due_to_presence_of_has.clear();
    };

    // It's ok to call have_has_selectors() instead of may_have_has_selectors() here and force
    // rule cache build, because it's going to be built soon anyway, since we could get here
    // only from update_style().
    if (!have_has_selectors()) {
        return;
    }

    auto nodes = move(m_pending_nodes_for_style_invalidation_due_to_presence_of_has);
    for (auto const& node : nodes) {
        if (!node)
            continue;
        for (auto ancestor = node.ptr(); ancestor; ancestor = ancestor->parent_or_shadow_host()) {
            if (!ancestor->is_element())
                continue;
            auto& element = static_cast<DOM::Element&>(*ancestor);
            element.invalidate_style_if_affected_by_has();

            auto* parent = ancestor->parent_or_shadow_host();
            if (!parent)
                return;

            // If any ancestor's sibling was tested against selectors like ".a:has(+ .b)" or ".a:has(~ .b)"
            // its style might be affected by the change in descendant node.
            parent->for_each_child_of_type<DOM::Element>([&](auto& ancestor_sibling_element) {
                if (ancestor_sibling_element.affected_by_has_pseudo_class_with_relative_selector_that_has_sibling_combinator())
                    ancestor_sibling_element.invalidate_style_if_affected_by_has();
                return IterationDecision::Continue;
            });
        }
    }
}

}
