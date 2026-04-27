/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/Optional.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleInvalidationData.h>

namespace Web::CSS {

static void append_or_merge_descendant_rule(Vector<DescendantInvalidationRule>& rules, DescendantInvalidationRule const& rule)
{
    for (auto& existing_rule : rules) {
        if (existing_rule.match_any != rule.match_any)
            continue;
        if (*existing_rule.payload != *rule.payload)
            continue;

        if (existing_rule.match_any)
            return;

        existing_rule.match_set.include_all_from(rule.match_set);
        return;
    }
    rules.append(rule);
}

static void append_or_merge_sibling_rule(Vector<SiblingInvalidationRule>& rules, SiblingInvalidationRule const& rule)
{
    for (auto& existing_rule : rules) {
        if (existing_rule.reach != rule.reach)
            continue;
        if (existing_rule.match_any != rule.match_any)
            continue;
        if (*existing_rule.payload != *rule.payload)
            continue;

        if (existing_rule.match_any)
            return;

        existing_rule.match_set.include_all_from(rule.match_set);
        return;
    }
    rules.append(rule);
}

bool InvalidationPlan::is_empty() const
{
    return !invalidate_self && !invalidate_whole_subtree && descendant_rules.is_empty() && sibling_rules.is_empty();
}

bool DescendantInvalidationRule::operator==(DescendantInvalidationRule const& other) const
{
    return match_set == other.match_set
        && match_any == other.match_any
        && *payload == *other.payload;
}

bool SiblingInvalidationRule::operator==(SiblingInvalidationRule const& other) const
{
    return reach == other.reach
        && match_set == other.match_set
        && match_any == other.match_any
        && *payload == *other.payload;
}

template<typename Rule>
static bool rule_lists_are_equal_ignoring_order(Vector<Rule> const& a, Vector<Rule> const& b)
{
    if (a.size() != b.size())
        return false;

    Vector<bool> matched_rules;
    matched_rules.resize(b.size());

    for (auto const& rule : a) {
        bool found_match = false;
        for (size_t i = 0; i < b.size(); ++i) {
            if (matched_rules[i])
                continue;
            if (!(rule == b[i]))
                continue;
            matched_rules[i] = true;
            found_match = true;
            break;
        }
        if (!found_match)
            return false;
    }

    return true;
}

bool InvalidationPlan::operator==(InvalidationPlan const& other) const
{
    if (invalidate_self != other.invalidate_self)
        return false;
    if (invalidate_whole_subtree != other.invalidate_whole_subtree)
        return false;

    return rule_lists_are_equal_ignoring_order(descendant_rules, other.descendant_rules)
        && rule_lists_are_equal_ignoring_order(sibling_rules, other.sibling_rules);
}

void InvalidationPlan::include_all_from(InvalidationPlan const& other)
{
    invalidate_self |= other.invalidate_self;

    if (invalidate_whole_subtree)
        return;

    if (other.invalidate_whole_subtree) {
        invalidate_whole_subtree = true;
        descendant_rules.clear();
        sibling_rules.clear();
        return;
    }

    for (auto const& descendant_rule : other.descendant_rules)
        append_or_merge_descendant_rule(descendant_rules, descendant_rule);
    for (auto const& sibling_rule : other.sibling_rules)
        append_or_merge_sibling_rule(sibling_rules, sibling_rule);
}

// Iterates over the given selector, grouping consecutive simple selectors that have no combinator (Combinator::None).
// For example, given "div:not(.a) + .b[foo]", the callback is invoked twice:
// once for "div:not(.a)" and once for ".b[foo]".
template<typename Callback>
static void for_each_consecutive_simple_selector_group(Selector const& selector, Callback callback)
{
    auto const& compound_selectors = selector.compound_selectors();
    int compound_selector_index = compound_selectors.size() - 1;
    Vector<Selector::SimpleSelector const&> simple_selectors;
    Selector::Combinator combinator = Selector::Combinator::None;
    bool is_rightmost = true;
    while (compound_selector_index >= 0) {
        if (!simple_selectors.is_empty()) {
            callback(simple_selectors, combinator, is_rightmost);
            simple_selectors.clear();
            is_rightmost = false;
        }

        auto const& compound_selector = compound_selectors[compound_selector_index];
        for (auto const& simple_selector : compound_selector.simple_selectors)
            simple_selectors.append(simple_selector);
        combinator = compound_selector.combinator;

        --compound_selector_index;
    }
    if (!simple_selectors.is_empty())
        callback(simple_selectors, combinator, is_rightmost);
}

static HasArgumentScope classify_has_argument_scope(Selector const& selector)
{
    if (selector.compound_selectors().is_empty())
        return HasArgumentScope::Complex;

    auto leftmost_combinator = selector.compound_selectors().first().combinator;
    switch (leftmost_combinator) {
    case Selector::Combinator::Descendant:
        return HasArgumentScope::AllDescendants;
    case Selector::Combinator::ImmediateChild:
        return selector.compound_selectors().size() == 1 ? HasArgumentScope::ChildrenOnly : HasArgumentScope::Complex;
    case Selector::Combinator::NextSibling:
        return selector.compound_selectors().size() == 1 ? HasArgumentScope::NextSiblingOnly : HasArgumentScope::Complex;
    case Selector::Combinator::SubsequentSibling:
        return selector.compound_selectors().size() == 1 ? HasArgumentScope::AllFollowingSiblings : HasArgumentScope::Complex;
    default:
        return HasArgumentScope::Complex;
    }
}

template<typename Key>
static void append_has_invalidation_metadata(HashMap<Key, Vector<HasInvalidationMetadata>>& map, Key const& key, HasInvalidationMetadata const& metadata)
{
    auto& bucket = map.ensure(key, [] { return Vector<HasInvalidationMetadata> {}; });
    if (!bucket.contains_slow(metadata))
        bucket.append(metadata);
}

static bool selector_contains_featureless_subtree_sensitive_selector(Selector const&);

static bool pseudo_class_can_be_used_as_has_invalidation_feature(PseudoClass pseudo_class)
{
    return first_is_one_of(pseudo_class,
        PseudoClass::Enabled,
        PseudoClass::Disabled,
        PseudoClass::Defined,
        PseudoClass::PlaceholderShown,
        PseudoClass::Checked,
        PseudoClass::Required,
        PseudoClass::Optional,
        PseudoClass::Link,
        PseudoClass::AnyLink,
        PseudoClass::LocalLink);
}

static void collect_properties_used_in_has(Selector::SimpleSelector const& selector, StyleInvalidationData& style_invalidation_data, Optional<HasInvalidationMetadata> metadata)
{
    switch (selector.type) {
    case Selector::SimpleSelector::Type::Id: {
        if (metadata.has_value())
            append_has_invalidation_metadata(style_invalidation_data.ids_used_in_has_selectors, selector.name(), *metadata);
        break;
    }
    case Selector::SimpleSelector::Type::Class: {
        if (metadata.has_value())
            append_has_invalidation_metadata(style_invalidation_data.class_names_used_in_has_selectors, selector.name(), *metadata);
        break;
    }
    case Selector::SimpleSelector::Type::Attribute: {
        if (metadata.has_value())
            append_has_invalidation_metadata(style_invalidation_data.attribute_names_used_in_has_selectors, selector.attribute().qualified_name.name.lowercase_name, *metadata);
        break;
    }
    case Selector::SimpleSelector::Type::TagName: {
        if (metadata.has_value())
            append_has_invalidation_metadata(style_invalidation_data.tag_names_used_in_has_selectors, selector.qualified_name().name.lowercase_name, *metadata);
        break;
    }
    case Selector::SimpleSelector::Type::PseudoClass: {
        auto const& pseudo_class = selector.pseudo_class();
        if (pseudo_class_can_be_used_as_has_invalidation_feature(pseudo_class.type)) {
            if (metadata.has_value())
                append_has_invalidation_metadata(style_invalidation_data.pseudo_classes_used_in_has_selectors, pseudo_class.type, *metadata);
        } else if (metadata.has_value() && !first_is_one_of(pseudo_class.type, PseudoClass::Has, PseudoClass::Is, PseudoClass::Where)) {
            // The structural subtree filter can only compare concrete features and the pseudo-classes listed above.
            // For other pseudo-classes, such as :focus, :default, and :valid, a featureless node can still start or
            // stop matching. Keep the old conservative walk instead of trying to probe them generically.
            style_invalidation_data.has_selectors_sensitive_to_featureless_subtree_changes = true;
        }
        for (auto const& child_selector : pseudo_class.argument_selector_list) {
            Optional<HasInvalidationMetadata> child_metadata = metadata;
            if (pseudo_class.type == PseudoClass::Has) {
                child_metadata = HasInvalidationMetadata {
                    .relative_selector = child_selector.ptr(),
                    .scope = classify_has_argument_scope(*child_selector),
                };
                // These selectors can match because a featureless node is inserted, removed, or moved.
                // Since there is no concrete tag/class/id/attribute/pseudo-class feature to compare against
                // later, structural invalidation must keep walking conservatively for them.
                if (selector_contains_featureless_subtree_sensitive_selector(*child_selector))
                    style_invalidation_data.has_selectors_sensitive_to_featureless_subtree_changes = true;
            }
            for (auto const& compound_selector : child_selector->compound_selectors()) {
                for (auto const& simple_selector : compound_selector.simple_selectors)
                    collect_properties_used_in_has(simple_selector, style_invalidation_data, child_metadata);
            }
        }
        break;
    }
    case Selector::SimpleSelector::Type::PseudoElement: {
        // Pseudo-elements like ::slotted(.x:has(...)) carry a compound selector argument whose contents need the same
        // recursive collection.
        auto const& pseudo_element = selector.pseudo_element();
        if (pseudo_element.type() == PseudoElement::Slotted) {
            for (auto const& compound_selector : pseudo_element.compound_selector().compound_selectors()) {
                for (auto const& simple_selector : compound_selector.simple_selectors)
                    collect_properties_used_in_has(simple_selector, style_invalidation_data, metadata);
            }
        }
        break;
    }
    default:
        break;
    }
}

static bool simple_selector_is_featureless_subtree_sensitive(Selector::SimpleSelector const& selector)
{
    switch (selector.type) {
    case Selector::SimpleSelector::Type::Universal:
        return true;
    case Selector::SimpleSelector::Type::PseudoClass: {
        auto const& pseudo_class = selector.pseudo_class();
        switch (pseudo_class.type) {
        case PseudoClass::Not:
        case PseudoClass::Empty:
        case PseudoClass::FirstChild:
        case PseudoClass::LastChild:
        case PseudoClass::OnlyChild:
        case PseudoClass::FirstOfType:
        case PseudoClass::LastOfType:
        case PseudoClass::OnlyOfType:
        case PseudoClass::NthChild:
        case PseudoClass::NthLastChild:
        case PseudoClass::NthOfType:
        case PseudoClass::NthLastOfType:
            return true;
        case PseudoClass::Is:
        case PseudoClass::Where:
            for (auto const& child_selector : pseudo_class.argument_selector_list) {
                if (selector_contains_featureless_subtree_sensitive_selector(*child_selector))
                    return true;
            }
            return false;
        default:
            return false;
        }
    }
    default:
        return false;
    }
}

static bool selector_contains_featureless_subtree_sensitive_selector(Selector const& selector)
{
    for (auto const& compound_selector : selector.compound_selectors()) {
        for (auto const& simple_selector : compound_selector.simple_selectors) {
            if (simple_selector_is_featureless_subtree_sensitive(simple_selector))
                return true;
        }
    }
    return false;
}

static InvalidationSet build_invalidation_sets_for_selector_impl(StyleInvalidationData& style_invalidation_data, Selector const& selector, InsideNthChildPseudoClass inside_nth_child_pseudo_class);

static void add_invalidation_sets_to_cover_scope_leakage_of_relative_selector_in_has_pseudo_class(Selector const& selector, StyleInvalidationData& style_invalidation_data);

static bool should_register_invalidation_property(InvalidationSet::Property const& property)
{
    return !AK::first_is_one_of(property.type, InvalidationSet::Property::Type::InvalidateSelf, InvalidationSet::Property::Type::InvalidateWholeSubtree);
}

static InvalidationSet build_invalidation_set_for_simple_selectors(Vector<Selector::SimpleSelector const&> const& simple_selectors, ExcludePropertiesNestedInNotPseudoClass exclude_properties_nested_in_not_pseudo_class, StyleInvalidationData& style_invalidation_data, InsideNthChildPseudoClass inside_nth_child_pseudo_class)
{
    InvalidationSet invalidation_set;
    for (auto const& simple_selector : simple_selectors)
        build_invalidation_sets_for_simple_selector(simple_selector, invalidation_set, exclude_properties_nested_in_not_pseudo_class, style_invalidation_data, inside_nth_child_pseudo_class);
    return invalidation_set;
}

static bool simple_selector_group_matches_any(Vector<Selector::SimpleSelector const&> const& simple_selectors)
{
    return simple_selectors.size() == 1 && simple_selectors.first().type == Selector::SimpleSelector::Type::Universal;
}

static NonnullRefPtr<InvalidationPlan> make_invalidate_self_invalidation()
{
    auto invalidation = InvalidationPlan::create();
    invalidation->invalidate_self = true;
    return invalidation;
}

static NonnullRefPtr<InvalidationPlan> make_invalidate_whole_subtree_invalidation()
{
    auto invalidation = InvalidationPlan::create();
    invalidation->invalidate_whole_subtree = true;
    return invalidation;
}

static void add_invalidation_plan_for_properties(StyleInvalidationData& style_invalidation_data, InvalidationSet const& invalidation_properties, InvalidationPlan const& plan)
{
    invalidation_properties.for_each_property([&](auto const& invalidation_property) {
        if (!should_register_invalidation_property(invalidation_property))
            return IterationDecision::Continue;

        auto& stored_invalidation = style_invalidation_data.invalidation_plans.ensure(invalidation_property, [] {
            return InvalidationPlan::create();
        });
        stored_invalidation->include_all_from(plan);
        return IterationDecision::Continue;
    });
}

struct SelectorRighthand {
    InvalidationSet subject_match_set;
    bool subject_matches_any { false };
    NonnullRefPtr<InvalidationPlan> payload;
};

static NonnullRefPtr<InvalidationPlan> build_invalidation_for_combinator(Selector::Combinator combinator, SelectorRighthand const& righthand)
{
    if (righthand.payload->invalidate_whole_subtree || (!righthand.subject_matches_any && righthand.subject_match_set.is_empty()))
        return make_invalidate_whole_subtree_invalidation();

    auto invalidation = InvalidationPlan::create();
    switch (combinator) {
    case Selector::Combinator::ImmediateChild:
    case Selector::Combinator::Descendant:
        append_or_merge_descendant_rule(invalidation->descendant_rules, { righthand.subject_match_set, righthand.subject_matches_any, righthand.payload });
        break;
    case Selector::Combinator::NextSibling:
        append_or_merge_sibling_rule(invalidation->sibling_rules, { SiblingInvalidationReach::Adjacent, righthand.subject_match_set, righthand.subject_matches_any, righthand.payload });
        break;
    case Selector::Combinator::SubsequentSibling:
        append_or_merge_sibling_rule(invalidation->sibling_rules, { SiblingInvalidationReach::Subsequent, righthand.subject_match_set, righthand.subject_matches_any, righthand.payload });
        break;
    default:
        invalidation->invalidate_whole_subtree = true;
        break;
    }
    return invalidation;
}

void build_invalidation_sets_for_simple_selector(Selector::SimpleSelector const& selector, InvalidationSet& invalidation_set, ExcludePropertiesNestedInNotPseudoClass exclude_properties_nested_in_not_pseudo_class, StyleInvalidationData& style_invalidation_data, InsideNthChildPseudoClass inside_nth_child_selector)
{
    switch (selector.type) {
    case Selector::SimpleSelector::Type::Class:
        invalidation_set.set_needs_invalidate_class(selector.name());
        break;
    case Selector::SimpleSelector::Type::Id:
        invalidation_set.set_needs_invalidate_id(selector.name());
        break;
    case Selector::SimpleSelector::Type::TagName:
        invalidation_set.set_needs_invalidate_tag_name(selector.qualified_name().name.lowercase_name);
        break;
    case Selector::SimpleSelector::Type::Attribute:
        invalidation_set.set_needs_invalidate_attribute(selector.attribute().qualified_name.name.lowercase_name);
        break;
    case Selector::SimpleSelector::Type::PseudoClass: {
        auto const& pseudo_class = selector.pseudo_class();
        switch (pseudo_class.type) {
        case PseudoClass::Enabled:
        case PseudoClass::Defined:
        case PseudoClass::Disabled:
        case PseudoClass::Empty:
        case PseudoClass::PlaceholderShown:
        case PseudoClass::Checked:
        case PseudoClass::Has: {
            for (auto const& nested_selector : pseudo_class.argument_selector_list)
                add_invalidation_sets_to_cover_scope_leakage_of_relative_selector_in_has_pseudo_class(*nested_selector, style_invalidation_data);
            [[fallthrough]];
        }
        case PseudoClass::Link:
        case PseudoClass::AnyLink:
        case PseudoClass::LocalLink:
        case PseudoClass::Required:
        case PseudoClass::Optional:
            invalidation_set.set_needs_invalidate_pseudo_class(pseudo_class.type);
            break;
        default:
            break;
        }
        if (pseudo_class.type == PseudoClass::Has)
            break;
        if (exclude_properties_nested_in_not_pseudo_class == ExcludePropertiesNestedInNotPseudoClass::Yes && pseudo_class.type == PseudoClass::Not)
            break;
        InsideNthChildPseudoClass inside_nth_child_pseudo_class_for_nested = inside_nth_child_selector;
        if (AK::first_is_one_of(pseudo_class.type, PseudoClass::NthChild, PseudoClass::NthLastChild, PseudoClass::NthOfType, PseudoClass::NthLastOfType))
            inside_nth_child_pseudo_class_for_nested = InsideNthChildPseudoClass::Yes;
        for (auto const& nested_selector : pseudo_class.argument_selector_list) {
            auto rightmost_invalidation_set_for_selector = build_invalidation_sets_for_selector_impl(style_invalidation_data, *nested_selector, inside_nth_child_pseudo_class_for_nested);
            invalidation_set.include_all_from(rightmost_invalidation_set_for_selector);

            // Propagate :has() from inner selectors where it appears in non-rightmost compounds.
            // The rightmost set only carries properties from the rightmost compound, so :has() in
            // non-rightmost positions (e.g., :is(:has(.x) .y)) is not propagated. We need it in the
            // outer invalidation set so outer compounds register plans for pseudo_class:Has that
            // account for the full selector context.
            // Additionally, when :has() is inside a complex :is()/:where() argument (multiple
            // compounds), the outer invalidation plan can't correctly capture the nested combinator
            // structure — e.g., sibling combinators at the outer level would be applied at the wrong
            // DOM level. Fall back to whole-subtree invalidation for :has() in these cases.
            if (nested_selector->contains_pseudo_class(PseudoClass::Has)) {
                invalidation_set.set_needs_invalidate_pseudo_class(PseudoClass::Has);
                if (nested_selector->compound_selectors().size() > 1) {
                    InvalidationSet has_only;
                    has_only.set_needs_invalidate_pseudo_class(PseudoClass::Has);
                    add_invalidation_plan_for_properties(style_invalidation_data, has_only, *make_invalidate_whole_subtree_invalidation());
                }
            }
        }
        break;
    }
    case Selector::SimpleSelector::Type::PseudoElement: {
        // Pseudo-elements like ::slotted(.x) and ::part(...) carry a compound selector argument whose simple
        // selectors decide which property changes should trigger invalidation against this rule.
        auto const& pseudo_element = selector.pseudo_element();
        if (pseudo_element.type() == PseudoElement::Slotted) {
            for (auto const& compound_selector : pseudo_element.compound_selector().compound_selectors()) {
                for (auto const& nested_simple : compound_selector.simple_selectors)
                    build_invalidation_sets_for_simple_selector(nested_simple, invalidation_set, exclude_properties_nested_in_not_pseudo_class, style_invalidation_data, inside_nth_child_selector);
            }
        }
        break;
    }
    default:
        break;
    }
}

static void add_invalidation_sets_to_cover_scope_leakage_of_relative_selector_in_has_pseudo_class(Selector const& selector, StyleInvalidationData& style_invalidation_data)
{
    // Normally, :has() invalidation scope is limited to ancestors and ancestor siblings, however it could require
    // descendants invalidation when :is() with complex selector is used inside :has() relative selector.
    // For example ".a:has(:is(.b .c))" requires invalidation whenever "b" class is added or removed.
    // To cover this case, we add descendant invalidation set that requires whole subtree invalidation for each
    // property used in non-subject part of complex selector.

    auto invalidate_whole_subtree_for_invalidation_properties_in_non_subject_part_of_complex_selector = [&](Selector const& selector_to_invalidate) {
        for_each_consecutive_simple_selector_group(selector_to_invalidate, [&](Vector<Selector::SimpleSelector const&> const& simple_selectors, Selector::Combinator, bool rightmost) {
            if (rightmost)
                return;

            auto invalidation_set = build_invalidation_set_for_simple_selectors(simple_selectors, ExcludePropertiesNestedInNotPseudoClass::No, style_invalidation_data, InsideNthChildPseudoClass::No);
            add_invalidation_plan_for_properties(style_invalidation_data, invalidation_set, *make_invalidate_whole_subtree_invalidation());
        });
    };

    for_each_consecutive_simple_selector_group(selector, [&](Vector<Selector::SimpleSelector const&> const& simple_selectors, Selector::Combinator, bool) {
        for (auto const& simple_selector : simple_selectors) {
            if (simple_selector.type != Selector::SimpleSelector::Type::PseudoClass)
                continue;
            auto const& pseudo_class = simple_selector.pseudo_class();
            if (pseudo_class.type == PseudoClass::Is || pseudo_class.type == PseudoClass::Where || pseudo_class.type == PseudoClass::Not) {
                for (auto const& nested_selector : pseudo_class.argument_selector_list)
                    invalidate_whole_subtree_for_invalidation_properties_in_non_subject_part_of_complex_selector(*nested_selector);
            }
        }
    });
}

static InvalidationSet build_invalidation_sets_for_selector_impl(StyleInvalidationData& style_invalidation_data, Selector const& selector, InsideNthChildPseudoClass inside_nth_child_pseudo_class)
{
    auto const& compound_selectors = selector.compound_selectors();
    int compound_selector_index = compound_selectors.size() - 1;
    VERIFY(compound_selector_index >= 0);

    InvalidationSet invalidation_set_for_rightmost_selector;
    Selector::Combinator previous_compound_combinator = Selector::Combinator::None;
    Optional<SelectorRighthand> selector_righthand;
    for_each_consecutive_simple_selector_group(selector, [&](Vector<Selector::SimpleSelector const&> const& simple_selectors, Selector::Combinator combinator, bool is_rightmost) {
        // Collect properties used in :has() so we can decide if only specific properties
        // trigger descendant invalidation or if the entire document must be invalidated.
        for (auto const& simple_selector : simple_selectors) {
            collect_properties_used_in_has(simple_selector, style_invalidation_data, {});
        }

        auto invalidation_properties = build_invalidation_set_for_simple_selectors(simple_selectors, ExcludePropertiesNestedInNotPseudoClass::No, style_invalidation_data, inside_nth_child_pseudo_class);
        auto subject_match_set = build_invalidation_set_for_simple_selectors(simple_selectors, ExcludePropertiesNestedInNotPseudoClass::Yes, style_invalidation_data, inside_nth_child_pseudo_class);
        bool subject_matches_any = subject_match_set.is_empty() && simple_selector_group_matches_any(simple_selectors);

        if (is_rightmost) {
            // The rightmost selector is handled twice:
            //  1) Include properties nested in :not()
            //  2) Exclude properties nested in :not()
            //
            // This ensures we handle cases like:
            //   :not(.foo) => produce invalidation set .foo { $ } ($ = invalidate self)
            //   .bar :not(.foo) => produce invalidation sets .foo { $ } and .bar { * } (* = invalidate subtree)
            //                      which means invalidation_set_for_rightmost_selector should be empty
            auto root_plan = make_invalidate_self_invalidation();
            if (inside_nth_child_pseudo_class == InsideNthChildPseudoClass::Yes) {
                // When invalidation property is nested in nth-child selector like p:nth-child(even of #t1, #t2, #t3)
                // we need to make sure all affected siblings are invalidated.
                root_plan->invalidate_whole_subtree = true;
            }
            add_invalidation_plan_for_properties(style_invalidation_data, invalidation_properties, *root_plan);

            invalidation_set_for_rightmost_selector = subject_match_set;
            selector_righthand = SelectorRighthand {
                .subject_match_set = move(subject_match_set),
                .subject_matches_any = subject_matches_any,
                .payload = root_plan,
            };
        } else {
            VERIFY(previous_compound_combinator != Selector::Combinator::None);
            VERIFY(selector_righthand.has_value());

            auto plan = build_invalidation_for_combinator(previous_compound_combinator, *selector_righthand);
            add_invalidation_plan_for_properties(style_invalidation_data, invalidation_properties, *plan);

            selector_righthand = SelectorRighthand {
                .subject_match_set = move(subject_match_set),
                .subject_matches_any = subject_matches_any,
                .payload = move(plan),
            };
        }

        previous_compound_combinator = combinator;
    });

    return invalidation_set_for_rightmost_selector;
}

void StyleInvalidationData::build_invalidation_sets_for_selector(Selector const& selector)
{
    (void)build_invalidation_sets_for_selector_impl(*this, selector, InsideNthChildPseudoClass::No);
}

}
