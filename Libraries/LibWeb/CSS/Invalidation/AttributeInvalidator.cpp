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
#include <LibWeb/CSS/Invalidation/ElementStateInvalidator.h>
#include <LibWeb/CSS/Invalidation/FormControlInvalidator.h>
#include <LibWeb/CSS/Invalidation/LanguageInvalidator.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLProgressElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
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

// An attribute is not only something selectors test by name: some of them decide a pseudo-class,
// and the engine that owns state invalidation has to hear that as a state fact rather than infer it
// from an attribute name. Each fact is published as the truth the element now has.
//
// `disabled` is not element-local. A disabled fieldset or optgroup disables the controls under it,
// so the fact has to be published for the subtree and not only for the element the attribute was
// written on.
static void record_pseudo_classes_implied_by_attribute(DOM::Element& element, Utf16FlyString const& attribute_name, Optional<Utf16String> const& old_value)
{
    if (attribute_name == HTML::AttributeNames::disabled) {
        element.for_each_in_inclusive_subtree_of_type<DOM::Element>([](DOM::Element& descendant) {
            auto matches_disabled = descendant.matches_disabled_pseudo_class();
            auto matches_enabled = descendant.matches_enabled_pseudo_class();
            if (!matches_disabled && !matches_enabled)
                return TraversalDecision::Continue;
            record_element_state_changed(descendant, PseudoClass::Disabled, matches_disabled);
            record_element_state_changed(descendant, PseudoClass::Enabled, matches_enabled);
            auto can_be_read_write = (is<HTML::HTMLInputElement>(descendant) && as<HTML::HTMLInputElement>(descendant).is_allowed_to_be_readonly())
                || is<HTML::HTMLTextAreaElement>(descendant);
            if (can_be_read_write && !descendant.has_attribute(HTML::AttributeNames::readonly)) {
                auto is_read_write = SelectorMatching::element_matches_state(descendant, PseudoClass::ReadWrite);
                invalidate_style_after_read_write_state_change(descendant, !is_read_write);
            }
            invalidate_style_after_validity_change(descendant);
            return TraversalDecision::Continue;
        });
    } else if (attribute_name == HTML::AttributeNames::placeholder) {
        record_element_state_changed(element, PseudoClass::PlaceholderShown, element.matches_placeholder_shown_pseudo_class());
    } else if (attribute_name == HTML::AttributeNames::checked) {
        record_element_state_changed(element, PseudoClass::Checked, element.matches_checked_pseudo_class());
        record_element_state_changed(element, PseudoClass::Unchecked, element.matches_unchecked_pseudo_class());
        if (auto* input = as_if<HTML::HTMLInputElement>(element); input && input->checked_applies())
            invalidate_style_after_default_state_change(element, old_value.has_value());
    } else if (attribute_name == HTML::AttributeNames::required || attribute_name == HTML::AttributeNames::type) {
        auto required_state = SelectorMatching::element_required_state(element);
        record_element_state_changed(element, PseudoClass::Required, required_state == SelectorMatching::RequiredState::Required);
        record_element_state_changed(element, PseudoClass::Optional, required_state == SelectorMatching::RequiredState::Optional);
        // Which states a control can hold is decided by what kind of control it is, so a type
        // change moves them without any of them being written: a checked radio that becomes a text
        // field is not checked, and nothing says so but the type.
        if (attribute_name == HTML::AttributeNames::type) {
            record_element_state_changed(element, PseudoClass::Checked, element.matches_checked_pseudo_class());
            record_element_state_changed(element, PseudoClass::Unchecked, element.matches_unchecked_pseudo_class());
            record_element_state_changed(element, PseudoClass::PlaceholderShown, element.matches_placeholder_shown_pseudo_class());
            record_element_state_changed(element, PseudoClass::Indeterminate, SelectorMatching::element_matches_state(element, PseudoClass::Indeterminate));
            if (is<HTML::HTMLInputElement>(element))
                invalidate_style_after_directionality_change(element);
        }
    } else if (attribute_name == HTML::AttributeNames::readonly) {
        auto can_be_read_write = (is<HTML::HTMLInputElement>(element) && as<HTML::HTMLInputElement>(element).is_allowed_to_be_readonly())
            || is<HTML::HTMLTextAreaElement>(element);
        if (can_be_read_write && SelectorMatching::element_matches_state(element, PseudoClass::Enabled)) {
            auto is_read_write = SelectorMatching::element_matches_state(element, PseudoClass::ReadWrite);
            invalidate_style_after_read_write_state_change(element, !is_read_write);
        }
        if (can_be_read_write || (is<HTML::HTMLElement>(element) && as<HTML::HTMLElement>(element).is_form_associated_custom_element()))
            invalidate_style_after_validity_change(element);
    } else if (attribute_name == HTML::AttributeNames::switch_) {
        record_element_state_changed(element, PseudoClass::Indeterminate, SelectorMatching::element_matches_state(element, PseudoClass::Indeterminate));
    } else if (attribute_name == HTML::AttributeNames::value && is<HTML::HTMLProgressElement>(element)) {
        record_element_state_changed(element, PseudoClass::Indeterminate, SelectorMatching::element_matches_state(element, PseudoClass::Indeterminate));
    } else if (attribute_name == HTML::AttributeNames::selected && is<HTML::HTMLOptionElement>(element)) {
        invalidate_style_after_default_state_change(element, old_value.has_value());
    }
}

void invalidate_style_after_attribute_change(
    DOM::Element& element,
    Utf16FlyString const& attribute_name,
    Optional<Utf16String> const& old_value,
    Optional<Utf16String> const& new_value)
{
    // An attribute that sources declarations changes what wins on this element. That is a
    // declaration input, not a selector one, and StyleEngine reaches the element from it directly.
    if (attribute_name == HTML::AttributeNames::style) {
        record_element_declarations_changed(element, ElementDeclarationKind::InlineStyle, old_value.has_value(), new_value.has_value());
    } else if (element.is_presentational_hint(attribute_name) || element.style_uses_attr_css_function()
        || (element.supports_dimension_attributes() && attribute_name.is_one_of(HTML::AttributeNames::width, HTML::AttributeNames::height))) {
        // The width and height attributes of an element that supports them map to hints the way
        // the presentational hint attributes do.
        record_element_declarations_changed(element, ElementDeclarationKind::PresentationalHint, true, true);
    }

    // The pseudo-classes an attribute implies are separate facts about the element, and each is its
    // own input: `disabled` makes an element `:disabled`, `required` makes it `:required`.
    record_pseudo_classes_implied_by_attribute(element, attribute_name, old_value);
}

}
