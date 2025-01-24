/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleInvalidationData.h>

namespace Web::CSS {

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
        for (auto const& simple_selector : compound_selector.simple_selectors) {
            simple_selectors.append(simple_selector);
        }
        combinator = compound_selector.combinator;

        --compound_selector_index;
    }
    if (!simple_selectors.is_empty()) {
        callback(simple_selectors, combinator, is_rightmost);
    }
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
        case PseudoClass::PlaceholderShown:
        case PseudoClass::Checked:
            style_invalidation_data.pseudo_classes_used_in_has_selectors.set(pseudo_class.type);
            break;
        default:
            break;
        }
        for (auto const& child_selector : pseudo_class.argument_selector_list) {
            for (auto const& compound_selector : child_selector->compound_selectors()) {
                for (auto const& simple_selector : compound_selector.simple_selectors) {
                    collect_properties_used_in_has(simple_selector, style_invalidation_data, in_has || pseudo_class.type == PseudoClass::Has);
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

enum class ExcludePropertiesNestedInNotPseudoClass : bool {
    No,
    Yes,
};

enum class InsideNthChildPseudoClass {
    No,
    Yes
};

static InvalidationSet build_invalidation_sets_for_selector_impl(StyleInvalidationData& style_invalidation_data, Selector const& selector, InsideNthChildPseudoClass inside_nth_child_pseudo_class);

static void build_invalidation_sets_for_simple_selector(Selector::SimpleSelector const& selector, InvalidationSet& invalidation_set, ExcludePropertiesNestedInNotPseudoClass exclude_properties_nested_in_not_pseudo_class, StyleInvalidationData& style_invalidation_data, InsideNthChildPseudoClass inside_nth_child_selector)
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
        case PseudoClass::Disabled:
        case PseudoClass::PlaceholderShown:
        case PseudoClass::Checked:
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
        if (AK::first_is_one_of(pseudo_class.type, PseudoClass::NthChild, PseudoClass::NthLastChild, PseudoClass::NthOfType, PseudoClass::NthLastOfType)) {
            inside_nth_child_pseudo_class_for_nested = InsideNthChildPseudoClass::Yes;
        }
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

static InvalidationSet build_invalidation_sets_for_selector_impl(StyleInvalidationData& style_invalidation_data, Selector const& selector, InsideNthChildPseudoClass inside_nth_child_pseudo_class)
{
    auto const& compound_selectors = selector.compound_selectors();
    int compound_selector_index = compound_selectors.size() - 1;
    VERIFY(compound_selector_index >= 0);

    InvalidationSet invalidation_set_for_rightmost_selector;
    Selector::Combinator previous_compound_combinator = Selector::Combinator::None;
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

        if (is_rightmost) {
            // The rightmost selector is handled twice:
            //  1) Include properties nested in :not()
            //  2) Exclude properties nested in :not()
            //
            // This ensures we handle cases like:
            //   :not(.foo) => produce invalidation set .foo { $ } ($ = invalidate self)
            //   .bar :not(.foo) => produce invalidation sets .foo { $ } and .bar { * } (* = invalidate subtree)
            //                      which means invalidation_set_for_rightmost_selector should be empty
            for (auto const& simple_selector : simple_selectors) {
                InvalidationSet s;
                build_invalidation_sets_for_simple_selector(simple_selector, s, ExcludePropertiesNestedInNotPseudoClass::No, style_invalidation_data, inside_nth_child_pseudo_class);
                s.for_each_property([&](auto const& invalidation_property) {
                    auto& descendant_invalidation_set = style_invalidation_data.descendant_invalidation_sets.ensure(invalidation_property, [] { return InvalidationSet {}; });
                    descendant_invalidation_set.set_needs_invalidate_self();
                    if (inside_nth_child_pseudo_class == InsideNthChildPseudoClass::Yes) {
                        // When invalidation property is nested in nth-child selector like p:nth-child(even of #t1, #t2, #t3)
                        // we need to make all siblings are invalidated.
                        descendant_invalidation_set.set_needs_invalidate_whole_subtree();
                    }
                });
            }

            for (auto const& simple_selector : simple_selectors) {
                build_invalidation_sets_for_simple_selector(simple_selector, invalidation_set_for_rightmost_selector, ExcludePropertiesNestedInNotPseudoClass::Yes, style_invalidation_data, inside_nth_child_pseudo_class);
            }
        } else {
            VERIFY(previous_compound_combinator != Selector::Combinator::None);
            for (auto const& simple_selector : simple_selectors) {
                InvalidationSet s;
                build_invalidation_sets_for_simple_selector(simple_selector, s, ExcludePropertiesNestedInNotPseudoClass::No, style_invalidation_data, inside_nth_child_pseudo_class);
                s.for_each_property([&](auto const& invalidation_property) {
                    auto& descendant_invalidation_set = style_invalidation_data.descendant_invalidation_sets.ensure(invalidation_property, [] {
                        return InvalidationSet {};
                    });
                    // If the rightmost selector's invalidation set is empty, it means there's no
                    // specific property-based invalidation, so we fall back to invalidating the whole subtree.
                    // If combinator to the right of current compound selector is NextSibling or SubsequentSibling,
                    // we also need to invalidate the whole subtree, because we don't support sibling invalidation sets.
                    if (AK::first_is_one_of(previous_compound_combinator, Selector::Combinator::NextSibling, Selector::Combinator::SubsequentSibling)) {
                        descendant_invalidation_set.set_needs_invalidate_whole_subtree();
                    } else if (invalidation_set_for_rightmost_selector.is_empty()) {
                        descendant_invalidation_set.set_needs_invalidate_whole_subtree();
                    } else {
                        descendant_invalidation_set.include_all_from(invalidation_set_for_rightmost_selector);
                    }
                });
            }
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
