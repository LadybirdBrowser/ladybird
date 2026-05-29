/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/IterationDecision.h>
#include <LibWeb/CSS/Invalidation/InvalidationSetMatcher.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>

namespace Web::CSS::Invalidation {

static bool compound_may_match_element_impl(DOM::Element const& element, Selector::CompoundSelector const& compound_selector, Optional<PseudoClass> ignored_pseudo_class, bool in_selector_list_argument)
{
    for (auto const& simple_selector : compound_selector.simple_selectors) {
        switch (simple_selector.type) {
        case Selector::SimpleSelector::Type::Universal:
            break;
        case Selector::SimpleSelector::Type::Nesting:
        case Selector::SimpleSelector::Type::Invalid:
            return true;
        case Selector::SimpleSelector::Type::PseudoElement:
            if (in_selector_list_argument)
                return false;
            break;
        case Selector::SimpleSelector::Type::TagName:
            if (element.lowercased_local_name() != simple_selector.qualified_name().name.lowercase_name)
                return false;
            break;
        case Selector::SimpleSelector::Type::Id: {
            auto id = element.id();
            if (!id.has_value() || *id != simple_selector.name())
                return false;
            break;
        }
        case Selector::SimpleSelector::Type::Class:
            if (!element.class_names().contains_slow(simple_selector.name()))
                return false;
            break;
        case Selector::SimpleSelector::Type::Attribute:
            if (!element.has_attribute(simple_selector.attribute().qualified_name.name.lowercase_name))
                return false;
            break;
        case Selector::SimpleSelector::Type::PseudoClass: {
            auto const& pseudo_class = simple_selector.pseudo_class();
            if (ignored_pseudo_class.has_value() && pseudo_class.type == *ignored_pseudo_class)
                break;
            switch (pseudo_class.type) {
            case PseudoClass::Is:
            case PseudoClass::Where: {
                bool argument_may_match = false;
                for (auto const& argument_selector : pseudo_class.argument_selector_list) {
                    if (compound_may_match_element_impl(element, argument_selector->compound_selectors().last(), ignored_pseudo_class, true)) {
                        argument_may_match = true;
                        break;
                    }
                }
                if (!argument_may_match)
                    return false;
                break;
            }
            case PseudoClass::Root:
                if (&element != element.document().document_element())
                    return false;
                break;
            default:
                break;
            }
            break;
        }
        }
    }
    return true;
}

bool compound_may_match_element(DOM::Element const& element, Selector::CompoundSelector const& compound_selector, Optional<PseudoClass> ignored_pseudo_class)
{
    return compound_may_match_element_impl(element, compound_selector, ignored_pseudo_class, false);
}

bool element_matches_any_invalidation_set_property(DOM::Element const& element, InvalidationSet const& set)
{
    auto includes_property = [&](InvalidationSet::Property const& property) {
        switch (property.type) {
        case InvalidationSet::Property::Type::Class: {
            auto case_sensitivity = CaseSensitivity::CaseSensitive;
            if (element.document().in_quirks_mode())
                case_sensitivity = CaseSensitivity::CaseInsensitive;
            return element.has_class(property.name(), case_sensitivity);
        }
        case InvalidationSet::Property::Type::Id:
            return element.id() == property.name();
        case InvalidationSet::Property::Type::TagName:
            return element.local_name() == property.name();
        case InvalidationSet::Property::Type::Attribute:
            return element.has_attribute(property.name()) || element.has_removed_attribute_for_style_invalidation(property.name());
        case InvalidationSet::Property::Type::PseudoClass: {
            switch (property.value.get<PseudoClass>()) {
            case PseudoClass::Has:
                return element.affected_by_has_pseudo_class_in_subject_position()
                    || element.affected_by_has_pseudo_class_in_non_subject_position();
            case PseudoClass::Enabled:
                return element.matches_enabled_pseudo_class();
            case PseudoClass::Disabled:
                return element.matches_disabled_pseudo_class();
            case PseudoClass::Defined:
                return element.is_defined();
            case PseudoClass::Checked:
                return element.matches_checked_pseudo_class();
            case PseudoClass::PlaceholderShown:
                return element.matches_placeholder_shown_pseudo_class();
            case PseudoClass::Empty: {
                if (!element.has_children())
                    return true;
                if (element.first_child_of_type<DOM::Element>())
                    return false;
                bool has_nonempty_text_child = false;
                element.for_each_child_of_type<DOM::Text>([&](auto const& text_child) {
                    if (!text_child.data().is_empty()) {
                        has_nonempty_text_child = true;
                        return IterationDecision::Break;
                    }
                    return IterationDecision::Continue;
                });
                return !has_nonempty_text_child;
            }
            case PseudoClass::AnyLink:
            case PseudoClass::Link:
                return element.matches_link_pseudo_class();
            case PseudoClass::LocalLink:
                return element.matches_local_link_pseudo_class();
            case PseudoClass::Root:
                return is<HTML::HTMLHtmlElement>(element);
            case PseudoClass::Host:
                return element.is_shadow_host();
            case PseudoClass::Required:
            case PseudoClass::Optional:
                return is<HTML::HTMLInputElement>(element)
                    || is<HTML::HTMLSelectElement>(element)
                    || is<HTML::HTMLTextAreaElement>(element);
            case PseudoClass::Hover: {
                auto* hovered = element.document().hovered_node();
                if (!hovered)
                    return false;
                if (hovered == &element)
                    return true;
                return element.is_shadow_including_ancestor_of(*hovered);
            }
            case PseudoClass::Focus:
                return element.is_focused();
            case PseudoClass::FocusVisible:
                return element.is_focused() && element.should_indicate_focus();
            case PseudoClass::FocusWithin:
                return element.matches_focus_within_pseudo_class();
            case PseudoClass::Active:
                return element.is_being_activated();
            case PseudoClass::Target:
                return element.document().target_element() == &element;
            case PseudoClass::FirstChild:
                return !element.previous_element_sibling();
            case PseudoClass::LastChild:
                return !element.next_element_sibling();
            case PseudoClass::OnlyChild:
                return !element.previous_element_sibling() && !element.next_element_sibling();
            default:
                VERIFY_NOT_REACHED();
            }
        }
        case InvalidationSet::Property::Type::InvalidateSelf:
            return false;
        case InvalidationSet::Property::Type::InvalidateWholeSubtree:
            return true;
        default:
            VERIFY_NOT_REACHED();
        }
    };

    bool includes_any = false;
    set.for_each_property([&](auto const& property) {
        if (includes_property(property)) {
            includes_any = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return includes_any;
}

}
