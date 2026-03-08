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

bool InvalidationPlan::is_empty() const
{
    return !invalidate_self && !invalidate_whole_subtree && descendant_rules.is_empty() && sibling_rules.is_empty();
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

    descendant_rules.extend(other.descendant_rules);
    sibling_rules.extend(other.sibling_rules);
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

static void collect_properties_used_in_has(Selector::SimpleSelector const& selector, StyleInvalidationData& style_invalidation_data, bool in_has)
{
    switch (selector.type) {
    case Selector::SimpleSelector::Type::Id: {
        if (in_has)
            style_invalidation_data.ids_used_in_has_selectors.set(selector.name());
        break;
    }
    case Selector::SimpleSelector::Type::Class: {
        if (in_has)
            style_invalidation_data.class_names_used_in_has_selectors.set(selector.name());
        break;
    }
    case Selector::SimpleSelector::Type::Attribute: {
        if (in_has)
            style_invalidation_data.attribute_names_used_in_has_selectors.set(selector.attribute().qualified_name.name.lowercase_name);
        break;
    }
    case Selector::SimpleSelector::Type::TagName: {
        if (in_has)
            style_invalidation_data.tag_names_used_in_has_selectors.set(selector.qualified_name().name.lowercase_name);
        break;
    }
    case Selector::SimpleSelector::Type::PseudoClass: {
        auto const& pseudo_class = selector.pseudo_class();
        switch (pseudo_class.type) {
        case PseudoClass::Enabled:
        case PseudoClass::Disabled:
        case PseudoClass::Defined:
        case PseudoClass::PlaceholderShown:
        case PseudoClass::Checked:
        case PseudoClass::Required:
        case PseudoClass::Optional:
        case PseudoClass::Link:
        case PseudoClass::AnyLink:
        case PseudoClass::LocalLink:
        case PseudoClass::Default:
            if (in_has)
                style_invalidation_data.pseudo_classes_used_in_has_selectors.set(pseudo_class.type);
            break;
        default:
            break;
        }
        for (auto const& child_selector : pseudo_class.argument_selector_list) {
            for (auto const& compound_selector : child_selector->compound_selectors()) {
                for (auto const& simple_selector : compound_selector.simple_selectors)
                    collect_properties_used_in_has(simple_selector, style_invalidation_data, in_has || pseudo_class.type == PseudoClass::Has);
            }
        }
        break;
    }
    default:
        break;
    }
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
        invalidation->descendant_rules.append({ righthand.subject_match_set, righthand.subject_matches_any, righthand.payload });
        break;
    case Selector::Combinator::NextSibling:
        invalidation->sibling_rules.append({ SiblingInvalidationReach::Adjacent, righthand.subject_match_set, righthand.subject_matches_any, righthand.payload });
        break;
    case Selector::Combinator::SubsequentSibling:
        invalidation->sibling_rules.append({ SiblingInvalidationReach::Subsequent, righthand.subject_match_set, righthand.subject_matches_any, righthand.payload });
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
            bool in_has = false;
            if (simple_selector.type == Selector::SimpleSelector::Type::PseudoClass) {
                auto const& pseudo_class = simple_selector.pseudo_class();
                if (pseudo_class.type == PseudoClass::Has)
                    in_has = true;
            }
            collect_properties_used_in_has(simple_selector, style_invalidation_data, in_has);
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

            // TODO: Fine-grained :has() invalidation is not yet implemented.
            bool has_pseudo_class_has = false;
            invalidation_properties.for_each_property([&](auto const& property) {
                if (property.type == InvalidationSet::Property::Type::PseudoClass && property.value.template get<PseudoClass>() == PseudoClass::Has)
                    has_pseudo_class_has = true;
                return IterationDecision::Continue;
            });
            if (has_pseudo_class_has) {
                InvalidationSet has_only;
                has_only.set_needs_invalidate_pseudo_class(PseudoClass::Has);
                add_invalidation_plan_for_properties(style_invalidation_data, has_only, *make_invalidate_whole_subtree_invalidation());
            }

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
