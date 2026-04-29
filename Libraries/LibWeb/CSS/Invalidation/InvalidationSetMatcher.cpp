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
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>

namespace Web::CSS::Invalidation {

bool element_matches_any_invalidation_set_property(DOM::Element const& element, InvalidationSet const& set)
{
    auto includes_property = [&](InvalidationSet::Property const& property) {
        switch (property.type) {
        case InvalidationSet::Property::Type::Class:
            return element.class_names().contains_slow(property.name());
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
