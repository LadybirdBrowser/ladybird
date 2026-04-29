/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Invalidation/AttributeInvalidator.h>
#include <LibWeb/CSS/InvalidationSet.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleSheetInvalidation.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/StyleInvalidationReason.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/Infra/CharacterTypes.h>

namespace Web::CSS::Invalidation {

void invalidate_style_after_attribute_change(
    DOM::Element& element,
    FlyString const& attribute_name,
    Optional<String> const& old_value,
    Optional<String> const& new_value)
{
    Vector<InvalidationSet::Property, 1> changed_properties;
    DOM::StyleInvalidationOptions style_invalidation_options;
    if (element.is_presentational_hint(attribute_name) || element.style_uses_attr_css_function())
        style_invalidation_options.invalidate_self = true;

    if (attribute_name == HTML::AttributeNames::style) {
        style_invalidation_options.invalidate_self = true;
    } else if (attribute_name == HTML::AttributeNames::class_) {
        Vector<StringView> old_classes;
        Vector<StringView> new_classes;
        if (old_value.has_value())
            old_classes = old_value->bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);
        if (new_value.has_value())
            new_classes = new_value->bytes_as_string_view().split_view_if(Infra::is_ascii_whitespace);
        for (auto& old_class : old_classes) {
            if (!new_classes.contains_slow(old_class))
                changed_properties.append({ .type = InvalidationSet::Property::Type::Class, .value = FlyString::from_utf8_without_validation(old_class.bytes()) });
        }
        for (auto& new_class : new_classes) {
            if (!old_classes.contains_slow(new_class))
                changed_properties.append({ .type = InvalidationSet::Property::Type::Class, .value = FlyString::from_utf8_without_validation(new_class.bytes()) });
        }
    } else if (attribute_name == HTML::AttributeNames::id) {
        if (old_value.has_value())
            changed_properties.append({ .type = InvalidationSet::Property::Type::Id, .value = FlyString(old_value.value()) });
        if (new_value.has_value())
            changed_properties.append({ .type = InvalidationSet::Property::Type::Id, .value = FlyString(new_value.value()) });
    } else if (attribute_name == HTML::AttributeNames::disabled) {
        changed_properties.append({ .type = InvalidationSet::Property::Type::PseudoClass, .value = PseudoClass::Disabled });
        changed_properties.append({ .type = InvalidationSet::Property::Type::PseudoClass, .value = PseudoClass::Enabled });
    } else if (attribute_name == HTML::AttributeNames::placeholder) {
        changed_properties.append({ .type = InvalidationSet::Property::Type::PseudoClass, .value = PseudoClass::PlaceholderShown });
    } else if (attribute_name == HTML::AttributeNames::value) {
        changed_properties.append({ .type = InvalidationSet::Property::Type::PseudoClass, .value = PseudoClass::Checked });
    } else if (attribute_name == HTML::AttributeNames::required) {
        changed_properties.append({ .type = InvalidationSet::Property::Type::PseudoClass, .value = PseudoClass::Required });
        changed_properties.append({ .type = InvalidationSet::Property::Type::PseudoClass, .value = PseudoClass::Optional });
    }

    if (!new_value.has_value())
        element.remember_removed_attribute_for_style_invalidation(attribute_name);

    changed_properties.append({ .type = InvalidationSet::Property::Type::Attribute, .value = attribute_name });
    element.invalidate_style(DOM::StyleInvalidationReason::ElementAttributeChange, changed_properties, style_invalidation_options);

    // If this element hosts a shadow root whose stylesheets have :host()-matching rules, the shadow tree's computed
    // styles can depend on this host's attributes. Mark the shadow subtree dirty so those rules re-evaluate.
    if (auto shadow_root = element.shadow_root()) {
        if (determine_shadow_root_stylesheet_effects(*shadow_root).may_match_shadow_host) {
            shadow_root->set_entire_subtree_needs_style_update(true);
            shadow_root->set_needs_style_update(true);
        }
    }
}

}
