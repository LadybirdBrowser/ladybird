/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
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

template<typename Callback>
static void for_each_ascii_whitespace_separated_token(Utf16View input, Callback callback)
{
    size_t start = 0;
    for (size_t i = 0; i <= input.length_in_code_units(); ++i) {
        if (i != input.length_in_code_units() && !Infra::is_ascii_whitespace(input.code_unit_at(i)))
            continue;

        if (i > start)
            callback(input.substring_view(start, i - start));
        start = i + 1;
    }
}

void invalidate_style_after_attribute_change(
    DOM::Element& element,
    Utf16FlyString const& attribute_name,
    Optional<Utf16String> const& old_value,
    Optional<Utf16String> const& new_value)
{
    Vector<InvalidationSet::Property, 1> changed_properties;
    DOM::StyleInvalidationOptions style_invalidation_options;
    if (element.is_presentational_hint(attribute_name) || element.style_uses_attr_css_function())
        style_invalidation_options.invalidate_self = true;

    if (attribute_name == HTML::AttributeNames::style) {
        style_invalidation_options.invalidate_self = true;
    } else if (attribute_name == HTML::AttributeNames::class_) {
        Vector<Utf16View> old_classes;
        Vector<Utf16View> new_classes;
        if (old_value.has_value())
            for_each_ascii_whitespace_separated_token(old_value->utf16_view(), [&](auto class_) { old_classes.append(class_); });
        if (new_value.has_value())
            for_each_ascii_whitespace_separated_token(new_value->utf16_view(), [&](auto class_) { new_classes.append(class_); });
        for (auto& old_class : old_classes) {
            if (!new_classes.contains_slow(old_class))
                changed_properties.append({ .type = InvalidationSet::Property::Type::Class, .value = Utf16FlyString::from_utf16(old_class) });
        }
        for (auto& new_class : new_classes) {
            if (!old_classes.contains_slow(new_class))
                changed_properties.append({ .type = InvalidationSet::Property::Type::Class, .value = Utf16FlyString::from_utf16(new_class) });
        }
    } else if (attribute_name == HTML::AttributeNames::id) {
        if (old_value.has_value())
            changed_properties.append({ .type = InvalidationSet::Property::Type::Id, .value = Utf16FlyString::from_utf16(old_value->utf16_view()) });
        if (new_value.has_value())
            changed_properties.append({ .type = InvalidationSet::Property::Type::Id, .value = Utf16FlyString::from_utf16(new_value->utf16_view()) });
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

    auto attribute_name_for_invalidation = attribute_name;
    if (!new_value.has_value())
        element.remember_removed_attribute_for_style_invalidation(attribute_name_for_invalidation);

    changed_properties.append({ .type = InvalidationSet::Property::Type::Attribute, .value = move(attribute_name_for_invalidation) });
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
