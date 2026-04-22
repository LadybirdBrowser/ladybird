/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSKeyframesRule.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/SelectorEngine.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/HTMLTableCellElement.h>
#include <LibWeb/HTML/HTMLTableElement.h>

namespace Web::CSS {

bool selector_may_match_light_dom_under_shadow_host(Selector const& selector)
{
    if (selector.is_slotted())
        return true;

    if (!selector.contains_pseudo_class(PseudoClass::Host))
        return false;

    // A bare :host selector only targets the host itself, but once a shadow rule keeps walking to another compound it
    // can match light-DOM nodes in the host tree instead of staying confined to the shadow subtree.
    return selector.compound_selectors().size() > 1;
}

bool selector_may_match_light_dom_under_shadow_host(StringView selector_text)
{
    CSS::Parser::ParsingParams parsing_params;
    auto selectors = parse_selector(parsing_params, selector_text);
    if (!selectors.has_value() || selectors->size() != 1)
        return false;
    return selector_may_match_light_dom_under_shadow_host(selectors->first());
}

static bool is_universal_only_compound(Selector::CompoundSelector const& compound_selector)
{
    if (compound_selector.simple_selectors.is_empty())
        return false;
    for (auto const& simple_selector : compound_selector.simple_selectors) {
        if (simple_selector.type != Selector::SimpleSelector::Type::Universal)
            return false;
    }
    return true;
}

static bool is_pseudo_element_only_compound(Selector::CompoundSelector const& compound_selector)
{
    if (compound_selector.simple_selectors.is_empty())
        return false;
    for (auto const& simple_selector : compound_selector.simple_selectors) {
        if (simple_selector.type != Selector::SimpleSelector::Type::PseudoElement)
            return false;
    }
    return true;
}

struct AnchorInvalidationRule {
    InvalidationSet anchor_set;
    RefPtr<Selector> anchor_selector;
    GC::Ptr<CSSStyleSheet const> style_sheet_for_rule;
};

// If the leading compounds (everything except the rightmost) of `compound_selectors` produce a usable invalidation
// set, return an anchor rule built from them.
static Optional<AnchorInvalidationRule> try_build_anchor_for_leading_compounds(Vector<Selector::CompoundSelector> const& compound_selectors, StyleInvalidationData& throwaway_data, GC::Ptr<CSSStyleSheet const> style_sheet_for_rule)
{
    if (compound_selectors.size() < 2)
        return {};

    Vector<Selector::CompoundSelector> anchor_compound_selectors;
    anchor_compound_selectors.ensure_capacity(compound_selectors.size() - 1);
    for (size_t i = 0; i < compound_selectors.size() - 1; ++i)
        anchor_compound_selectors.append(compound_selectors[i]);

    auto const& rightmost_anchor = anchor_compound_selectors.last();
    InvalidationSet anchor_set;
    for (auto const& simple : rightmost_anchor.simple_selectors)
        build_invalidation_sets_for_simple_selector(simple, anchor_set, ExcludePropertiesNestedInNotPseudoClass::No, throwaway_data, InsideNthChildPseudoClass::No);
    if (!anchor_set.has_properties())
        return {};

    RefPtr<Selector> anchor_selector;
    if (anchor_compound_selectors.size() > 1)
        anchor_selector = Selector::create(move(anchor_compound_selectors));

    return AnchorInvalidationRule { move(anchor_set), move(anchor_selector), style_sheet_for_rule };
}

void extend_style_sheet_invalidation_set_with_style_rule(StyleSheetInvalidationSet& result, CSSStyleRule const& style_rule)
{
    auto* style_sheet_for_rule = const_cast<CSSStyleRule&>(style_rule).parent_style_sheet();

    StyleInvalidationData throwaway_data;

    for (auto const& selector : style_rule.absolutized_selectors()) {
        result.may_match_light_dom_under_shadow_host |= selector_may_match_light_dom_under_shadow_host(*selector);
        result.may_match_shadow_host |= selector->contains_pseudo_class(PseudoClass::Host);

        auto const& compound_selectors = selector->compound_selectors();
        if (compound_selectors.is_empty())
            continue;

        auto const& rightmost = compound_selectors.last();

        InvalidationSet rightmost_set;
        for (auto const& simple : rightmost.simple_selectors)
            build_invalidation_sets_for_simple_selector(simple, rightmost_set, ExcludePropertiesNestedInNotPseudoClass::No, throwaway_data, InsideNthChildPseudoClass::No);

        if (rightmost_set.has_properties()) {
            result.invalidation_set.include_all_from(rightmost_set);
            continue;
        }

        // The rightmost compound has no targetable properties on its own, but we can still avoid a whole-subtree
        // invalidation if the leading compounds carry an anchor invalidation set that we can match against.
        if (is_pseudo_element_only_compound(rightmost)) {
            if (auto anchor = try_build_anchor_for_leading_compounds(compound_selectors, throwaway_data, style_sheet_for_rule); anchor.has_value()) {
                result.pseudo_element_rules.append({
                    .anchor_set = move(anchor->anchor_set),
                    .anchor_selector = move(anchor->anchor_selector),
                    .style_sheet_for_rule = anchor->style_sheet_for_rule,
                });
                continue;
            }
        }

        if (is_universal_only_compound(rightmost)) {
            if (auto anchor = try_build_anchor_for_leading_compounds(compound_selectors, throwaway_data, style_sheet_for_rule); anchor.has_value()) {
                result.trailing_universal_rules.append({
                    .anchor_set = move(anchor->anchor_set),
                    .anchor_selector = move(anchor->anchor_selector),
                    .combinator = rightmost.combinator,
                    .style_sheet_for_rule = anchor->style_sheet_for_rule,
                });
                continue;
            }
        }

        result.invalidation_set.set_needs_invalidate_whole_subtree();
        return;
    }
}

static GC::Ptr<DOM::Element const> shadow_host_for_targeted_shadow_root_invalidation(DOM::Node const& root)
{
    if (auto const* shadow_root = as_if<DOM::ShadowRoot>(root))
        return shadow_root->host();
    return nullptr;
}

template<typename Rule>
static bool element_matches_anchor_rule(DOM::Element const& element, Rule const& rule, GC::Ptr<DOM::Element const> shadow_host, GC::Ptr<DOM::ShadowRoot const> rule_shadow_root)
{
    if (!element.includes_properties_from_invalidation_set(rule.anchor_set))
        return false;
    if (rule.anchor_selector) {
        SelectorEngine::MatchContext context {
            .style_sheet_for_rule = rule.style_sheet_for_rule,
            .rule_shadow_root = rule_shadow_root,
        };
        if (!SelectorEngine::matches(*rule.anchor_selector, element, shadow_host, context))
            return false;
    }
    return true;
}

static void apply_trailing_universal_combinator(Selector::Combinator combinator, DOM::Element& anchor, DOM::Node& target_root)
{
    switch (combinator) {
    case Selector::Combinator::ImmediateChild:
        target_root.for_each_child_of_type<DOM::Element>([](DOM::Element& child) {
            child.set_needs_style_update(true);
            return IterationDecision::Continue;
        });
        break;
    case Selector::Combinator::Descendant:
        target_root.for_each_in_subtree_of_type<DOM::Element>([](DOM::Element& descendant) {
            descendant.set_needs_style_update(true);
            return TraversalDecision::Continue;
        });
        break;
    case Selector::Combinator::NextSibling:
        if (auto* sibling = anchor.next_element_sibling())
            sibling->set_needs_style_update(true);
        break;
    case Selector::Combinator::SubsequentSibling:
        for (auto* sibling = anchor.next_element_sibling(); sibling; sibling = sibling->next_element_sibling())
            sibling->set_needs_style_update(true);
        break;
    case Selector::Combinator::Column:
        // The column combinator relates a <col>/<colgroup> anchor to the table cells in the represented column. We do
        // not yet have a more precise invalidation primitive for column membership, so keep the work bounded to the
        // current table instead of dropping the invalidation.
        for (auto* ancestor = &anchor; ancestor; ancestor = ancestor->parent_element()) {
            if (auto* table = as_if<HTML::HTMLTableElement>(*ancestor)) {
                table->for_each_in_subtree_of_type<HTML::HTMLTableCellElement>([](HTML::HTMLTableCellElement& cell) {
                    cell.set_needs_style_update(true);
                    return TraversalDecision::Continue;
                });
                break;
            }
        }
        break;
    case Selector::Combinator::None:
        break;
    }
}

// Walk `root` once and, for each element, check the primary invalidation set plus every anchor rule in one pass. Doing
// this as a single traversal instead of one walk per rule kind keeps the dirtying cost proportional to the tree size
// rather than to tree_size * rule_count.
static void invalidate_elements_matching_invalidation_set_and_anchor_rules(
    DOM::Node& root,
    StyleSheetInvalidationSet const& result,
    GC::Ptr<DOM::Element const> shadow_host,
    GC::Ptr<DOM::ShadowRoot const> rule_shadow_root)
{
    auto const& invalidation_set = result.invalidation_set;
    bool const has_primary_set = invalidation_set.has_properties();
    bool const has_anchor_rules = !result.pseudo_element_rules.is_empty() || !result.trailing_universal_rules.is_empty();
    if (!has_primary_set && !has_anchor_rules)
        return;

    root.for_each_in_inclusive_subtree_of_type<DOM::Element>([&](DOM::Element& element) {
        if (has_primary_set && !element.needs_style_update()
            && element.includes_properties_from_invalidation_set(invalidation_set)) {
            element.set_needs_style_update(true);
        }

        if (!element.needs_style_update()) {
            for (auto const& rule : result.pseudo_element_rules) {
                if (element_matches_anchor_rule(element, rule, shadow_host, rule_shadow_root)) {
                    element.set_needs_style_update(true);
                    break;
                }
            }
        }

        // Trailing-universal rules dirty *other* elements (siblings or children of the anchor), so we still have to
        // check them even if `element` is already marked dirty.
        for (auto const& rule : result.trailing_universal_rules) {
            if (element_matches_anchor_rule(element, rule, shadow_host, rule_shadow_root))
                apply_trailing_universal_combinator(rule.combinator, element, element);
        }

        return TraversalDecision::Continue;
    });
}

static bool slot_or_ancestor_needs_style_update(HTML::HTMLSlotElement const& slot, DOM::ShadowRoot const& shadow_root)
{
    for (auto const* node = static_cast<DOM::Node const*>(&slot); node && node != &shadow_root; node = node->parent_node()) {
        if (node->needs_style_update())
            return true;
    }
    return false;
}

// Slotted light-DOM nodes inherit from their assigned <slot>. If a targeted shadow-root invalidation dirties the slot
// or one of its ancestors, we must also dirty the flattened assignees outside the shadow subtree.
void invalidate_assigned_elements_for_dirty_slots(DOM::ShadowRoot& shadow_root)
{
    shadow_root.for_each_in_inclusive_subtree_of_type<HTML::HTMLSlotElement>([&shadow_root](HTML::HTMLSlotElement& slot) {
        if (!slot_or_ancestor_needs_style_update(slot, shadow_root))
            return TraversalDecision::Continue;
        for (auto const& assigned_element : slot.assigned_elements({ .flatten = true }))
            assigned_element->set_needs_style_update(true);
        return TraversalDecision::Continue;
    });
}

void invalidate_root_for_style_sheet_change(DOM::Node& root, StyleSheetInvalidationSet const& result, DOM::StyleInvalidationReason reason, bool force_broad_invalidation)
{
    auto const& invalidation_set = result.invalidation_set;
    if (force_broad_invalidation || invalidation_set.needs_invalidate_whole_subtree()) {
        root.invalidate_style(reason);
        if (auto* shadow_root = as_if<DOM::ShadowRoot>(root)) {
            shadow_root->for_each_in_inclusive_subtree_of_type<HTML::HTMLSlotElement>([](HTML::HTMLSlotElement& slot) {
                slot.set_needs_style_update(true);
                return TraversalDecision::Continue;
            });
            invalidate_assigned_elements_for_dirty_slots(*shadow_root);

            if (auto* host = shadow_root->host()) {
                // Broad shadow-root mutations are only allowed to escape the shadow tree when the stylesheet can
                // actually reach host-side nodes. A layer-order-only change, for example, still needs a full restyle
                // inside the shadow tree, but it should not turn into a document-wide invalidation for unrelated
                // light-DOM.
                if (result.may_match_light_dom_under_shadow_host && !result.may_match_shadow_host) {
                    host->invalidate_style(reason);
                } else if (result.may_match_light_dom_under_shadow_host) {
                    host->root().invalidate_style(reason);
                } else if (result.may_match_shadow_host) {
                    host->invalidate_style(reason);
                    shadow_root->set_needs_style_update(true);
                }
            }
        }
        return;
    }

    auto* rule_shadow_root = as_if<DOM::ShadowRoot>(root);
    invalidate_elements_matching_invalidation_set_and_anchor_rules(
        root, result, shadow_host_for_targeted_shadow_root_invalidation(root), rule_shadow_root);

    if (auto* shadow_root = as_if<DOM::ShadowRoot>(root)) {
        invalidate_assigned_elements_for_dirty_slots(*shadow_root);

        if (auto* host = shadow_root->host()) {
            // Slotted selectors never match the host itself, so a targeted ::slotted(...) invalidation only needs to
            // walk the current host's light-DOM subtree. We can identify that case because it reaches host-side nodes
            // without ever setting may_match_shadow_host.
            //
            // :host combinators are different: they can escape to siblings or other nodes rooted alongside the host,
            // so they still need the broader host-root walk below.
            if (result.may_match_light_dom_under_shadow_host && !result.may_match_shadow_host) {
                invalidate_elements_matching_invalidation_set_and_anchor_rules(*host, result, host, shadow_root);
            } else if (result.may_match_light_dom_under_shadow_host) {
                invalidate_elements_matching_invalidation_set_and_anchor_rules(host->root(), result, host, shadow_root);
            } else if (result.may_match_shadow_host) {
                bool host_or_shadow_tree_needs_style_update = false;
                if (host->includes_properties_from_invalidation_set(invalidation_set))
                    host_or_shadow_tree_needs_style_update = true;
                for (auto const& rule : result.pseudo_element_rules) {
                    if (element_matches_anchor_rule(*host, rule, host, shadow_root)) {
                        host_or_shadow_tree_needs_style_update = true;
                        break;
                    }
                }
                if (host_or_shadow_tree_needs_style_update) {
                    host->set_needs_style_update(true);
                    host->invalidate_style(reason);
                    // A targeted :host rule can change inherited values seen by both shadow descendants and the
                    // host's light-DOM subtree even when no selector directly matches there. Mark both subtrees dirty
                    // so recursive style update descends into those inherited-value dependents too.
                    shadow_root->set_needs_style_update(true);
                }
                for (auto const& rule : result.trailing_universal_rules) {
                    // Host-anchored trailing-universal selectors such as :host > * and :host * target the host's
                    // light-DOM tree, not descendants inside the shadow root. Keep the host as the selector anchor
                    // and dirty host-side nodes from there.
                    if (element_matches_anchor_rule(*host, rule, host, shadow_root))
                        apply_trailing_universal_combinator(rule.combinator, *host, *host);
                }
            }
        }
    }
}

void invalidate_owners_for_inserted_style_rule(CSSStyleSheet const& style_sheet, CSSStyleRule const& style_rule, DOM::StyleInvalidationReason reason)
{
    StyleSheetInvalidationSet invalidation_set;
    extend_style_sheet_invalidation_set_with_style_rule(invalidation_set, style_rule);

    for (auto& document_or_shadow_root : style_sheet.owning_documents_or_shadow_roots()) {
        auto& style_scope = document_or_shadow_root->is_shadow_root()
            ? as<DOM::ShadowRoot>(*document_or_shadow_root).style_scope()
            : document_or_shadow_root->document().style_scope();
        style_scope.invalidate_rule_cache();

        // A dirty shadow subtree can still need follow-up invalidation on the host side for :host(...) and
        // ::slotted(...) matches, so we don't skip shadow roots even when their entire subtree is already marked.
        invalidate_root_for_style_sheet_change(*document_or_shadow_root, invalidation_set, reason);
    }
}

static void for_each_tree_affected_by_shadow_root_stylesheet_change(
    DOM::Node& root,
    bool include_host,
    bool include_light_dom_under_shadow_host,
    Function<void(DOM::Node&)> const& callback)
{
    callback(root);

    if (auto* shadow_root = as_if<DOM::ShadowRoot>(root)) {
        auto* host = shadow_root->host();
        if (!host)
            return;

        // Slotted selectors only reach the current host subtree, but :host combinators can also reach siblings rooted
        // alongside the host. A bare :host selector, however, only needs the host itself.
        if (include_host && include_light_dom_under_shadow_host)
            callback(host->root());
        else if (include_host || include_light_dom_under_shadow_host)
            callback(*host);
    }
}

static bool style_value_references_animation_name(StyleValue const& value, FlyString const& animation_name)
{
    if (value.is_custom_ident())
        return value.as_custom_ident().custom_ident() == animation_name;
    if (value.is_string())
        return value.as_string().string_value() == animation_name;

    if (!value.is_value_list())
        return false;

    for (auto const& item : value.as_value_list().values()) {
        if (item->is_custom_ident() && item->as_custom_ident().custom_ident() == animation_name)
            return true;
        if (item->is_string() && item->as_string().string_value() == animation_name)
            return true;
    }

    return false;
}

static bool element_or_pseudo_references_animation_name(DOM::Element const& element, FlyString const& animation_name)
{
    auto references_animation_name_in_properties = [&](CSS::ComputedProperties const& computed_properties) {
        return style_value_references_animation_name(computed_properties.property(PropertyID::AnimationName), animation_name);
    };

    if (auto computed_properties = element.computed_properties(); computed_properties && references_animation_name_in_properties(*computed_properties))
        return true;

    for (u8 i = 0; i < to_underlying(CSS::PseudoElement::KnownPseudoElementCount); ++i) {
        auto pseudo_element = static_cast<CSS::PseudoElement>(i);
        if (auto computed_properties = element.computed_properties(pseudo_element); computed_properties && references_animation_name_in_properties(*computed_properties))
            return true;
    }

    return false;
}

static void invalidate_elements_affected_by_inserted_keyframes_rule(DOM::Node& root, FlyString const& animation_name)
{
    auto invalidate_matching_element = [&](DOM::Element& element) {
        // A new @keyframes rule only matters for elements or pseudo-elements that were already referencing the
        // inserted animation-name.
        if (element_or_pseudo_references_animation_name(element, animation_name))
            element.set_needs_style_update(true);
        return TraversalDecision::Continue;
    };

    if (root.is_document()) {
        // Document styles are inherited by existing shadow trees too, so document-scoped @keyframes insertions must
        // walk the shadow-including tree instead of stopping at shadow hosts.
        root.for_each_shadow_including_inclusive_descendant([&](DOM::Node& node) {
            if (auto* element = as_if<DOM::Element>(node))
                return invalidate_matching_element(*element);
            return TraversalDecision::Continue;
        });
        return;
    }

    root.for_each_in_inclusive_subtree_of_type<DOM::Element>(invalidate_matching_element);
}

static ShadowRootStylesheetEffects determine_shadow_root_stylesheet_effects_for_sheet(CSSStyleSheet const& style_sheet, DOM::ShadowRoot const& shadow_root)
{
    ShadowRootStylesheetEffects effects;

    Vector<GC::Ptr<HTML::HTMLSlotElement const>> slots;
    shadow_root.for_each_in_inclusive_subtree_of_type<HTML::HTMLSlotElement>([&](HTML::HTMLSlotElement const& slot) {
        slots.append(slot);
        return TraversalDecision::Continue;
    });

    auto selector_may_affect_assigned_nodes_via_slot_inheritance = [&](Selector const& selector) {
        for (auto const& slot : slots) {
            for (auto const* node = static_cast<DOM::Node const*>(slot.ptr()); node && node != &shadow_root; node = node->parent_node()) {
                auto const* element = as_if<DOM::Element>(node);
                if (!element)
                    continue;
                SelectorEngine::MatchContext context {
                    .style_sheet_for_rule = style_sheet,
                    .subject = *element,
                    .rule_shadow_root = &shadow_root,
                };
                if (SelectorEngine::matches(selector, *element, shadow_root.host(), context))
                    return true;
            }
        }
        return false;
    };

    style_sheet.for_each_effective_style_producing_rule([&](CSSRule const& rule) {
        if (effects.may_match_shadow_host && effects.may_match_light_dom_under_shadow_host && effects.may_affect_assigned_nodes_via_slots)
            return;

        if (!is<CSSStyleRule>(rule))
            return;

        auto const& style_rule = as<CSSStyleRule>(rule);
        for (auto const& selector : style_rule.absolutized_selectors()) {
            effects.may_match_shadow_host |= selector->contains_pseudo_class(PseudoClass::Host);
            effects.may_match_light_dom_under_shadow_host |= selector_may_match_light_dom_under_shadow_host(*selector);

            if (!effects.may_affect_assigned_nodes_via_slots && !slots.is_empty())
                effects.may_affect_assigned_nodes_via_slots = selector_may_affect_assigned_nodes_via_slot_inheritance(*selector);

            if (effects.may_match_shadow_host && effects.may_match_light_dom_under_shadow_host && effects.may_affect_assigned_nodes_via_slots)
                return;
        }
    });

    return effects;
}

ShadowRootStylesheetEffects determine_shadow_root_stylesheet_effects(DOM::ShadowRoot const& shadow_root)
{
    ShadowRootStylesheetEffects effects;

    shadow_root.for_each_active_css_style_sheet([&](CSSStyleSheet& style_sheet) {
        auto sheet_effects = determine_shadow_root_stylesheet_effects_for_sheet(style_sheet, shadow_root);
        effects.may_match_shadow_host |= sheet_effects.may_match_shadow_host;
        effects.may_match_light_dom_under_shadow_host |= sheet_effects.may_match_light_dom_under_shadow_host;
        effects.may_affect_assigned_nodes_via_slots |= sheet_effects.may_affect_assigned_nodes_via_slots;
    });

    return effects;
}

ShadowRootStylesheetEffects determine_shadow_root_stylesheet_effects(CSSStyleSheet const& style_sheet)
{
    ShadowRootStylesheetEffects effects;

    for (auto& document_or_shadow_root : style_sheet.owning_documents_or_shadow_roots()) {
        auto* shadow_root = as_if<DOM::ShadowRoot>(*document_or_shadow_root);
        if (!shadow_root)
            continue;

        auto sheet_effects = determine_shadow_root_stylesheet_effects_for_sheet(style_sheet, *shadow_root);
        effects.may_match_shadow_host |= sheet_effects.may_match_shadow_host;
        effects.may_match_light_dom_under_shadow_host |= sheet_effects.may_match_light_dom_under_shadow_host;
        effects.may_affect_assigned_nodes_via_slots |= sheet_effects.may_affect_assigned_nodes_via_slots;

        if (effects.may_match_shadow_host && effects.may_match_light_dom_under_shadow_host && effects.may_affect_assigned_nodes_via_slots)
            break;
    }

    return effects;
}

void invalidate_owners_for_inserted_keyframes_rule(CSSStyleSheet const& style_sheet, CSSKeyframesRule const& keyframes_rule)
{
    for (auto& document_or_shadow_root : style_sheet.owning_documents_or_shadow_roots()) {
        auto& style_scope = document_or_shadow_root->is_shadow_root()
            ? as<DOM::ShadowRoot>(*document_or_shadow_root).style_scope()
            : document_or_shadow_root->document().style_scope();
        style_scope.invalidate_rule_cache();

        if (!document_or_shadow_root->is_shadow_root() && document_or_shadow_root->entire_subtree_needs_style_update())
            continue;

        bool include_host = false;
        bool include_light_dom_under_shadow_host = false;
        if (auto* shadow_root = as_if<DOM::ShadowRoot>(*document_or_shadow_root)) {
            auto effects = determine_shadow_root_stylesheet_effects(*shadow_root);
            include_host = effects.may_match_shadow_host;
            include_light_dom_under_shadow_host = effects.may_match_light_dom_under_shadow_host;
        }

        for_each_tree_affected_by_shadow_root_stylesheet_change(
            *document_or_shadow_root,
            include_host,
            include_light_dom_under_shadow_host,
            [&](DOM::Node& affected_root) {
                invalidate_elements_affected_by_inserted_keyframes_rule(affected_root, keyframes_rule.name());
            });
    }
}

}
