/*
 * Copyright (c) 2018-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/HTMLDetailsElement.h>
#include <LibWeb/HTML/HTMLDialogElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLMeterElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLProgressElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>

namespace Web::SelectorMatching {

static bool matches_hover_pseudo_class(DOM::Element const& element)
{
    auto* hovered_node = element.document().hovered_node();
    if (!hovered_node)
        return false;
    if (&element == hovered_node)
        return true;
    return element.is_shadow_including_ancestor_of(*hovered_node);
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-indeterminate
static inline bool matches_indeterminate_pseudo_class(DOM::Element const& element)
{
    // The :indeterminate pseudo-class must match any element falling into one of the following categories:
    // - input elements whose type attribute is in the Checkbox state and whose indeterminateness is true
    // FIXME: - input elements whose type attribute is in the Radio Button state and whose radio button group contains no input elements whose checkedness state is true.
    if (auto* input_element = as_if<HTML::HTMLInputElement>(element)) {
        switch (input_element->type_state()) {
        case HTML::HTMLInputElement::TypeAttributeState::Checkbox:
            // https://whatpr.org/html-attr-input-switch/9546/semantics-other.html#selector-indeterminate
            // input elements whose type attribute is in the Checkbox state, whose switch attribute is not set
            return input_element->indeterminate() && !element.has_attribute(HTML::AttributeNames::switch_);
        default:
            return false;
        }
    }
    // - progress elements with no value content attribute
    if (is<HTML::HTMLProgressElement>(element)) {
        return !element.has_attribute(HTML::AttributeNames::value);
    }
    return false;
}

static bool matches_read_write_pseudo_class(DOM::Element const& element)
{
    // The :read-write pseudo-class must match any element falling into one of the following categories,
    // which for the purposes of Selectors are thus considered user-alterable: [SELECTORS]
    // - input elements to which the readonly attribute applies, and that are mutable
    //   (i.e. that do not have the readonly attribute specified and that are not disabled)
    if (auto const* input_element = as_if<HTML::HTMLInputElement>(element))
        return input_element->is_allowed_to_be_readonly()
            && !input_element->has_attribute(HTML::AttributeNames::readonly) && input_element->enabled();
    // - textarea elements that do not have a readonly attribute, and that are not disabled
    if (auto const* input_element = as_if<HTML::HTMLTextAreaElement>(element))
        return !input_element->has_attribute(HTML::AttributeNames::readonly) && input_element->enabled();
    // - elements that are editing hosts or editable and are neither input elements nor textarea elements
    return element.is_editable_or_editing_host();
}

// https://drafts.csswg.org/selectors-4/#open-state
static bool matches_open_state_pseudo_class(DOM::Element const& element)
{
    // The :open pseudo-class represents an element that has both “open” and “closed” states,
    // and which is currently in the “open” state.

    // https://html.spec.whatwg.org/multipage/semantics-other.html#selector-open
    // The :open pseudo-class must match any element falling into one of the following categories:
    // - details elements that have an open attribute
    // - dialog elements that have an open attribute
    if (is<HTML::HTMLDetailsElement>(element) || is<HTML::HTMLDialogElement>(element))
        return element.has_attribute(HTML::AttributeNames::open);
    // - select elements that are a drop-down box and whose drop-down boxes are open
    if (auto const* select = as_if<HTML::HTMLSelectElement>(element))
        return select->is_open();
    // - input elements that support a picker and whose pickers are open
    if (auto const* input = as_if<HTML::HTMLInputElement>(element))
        return input->supports_a_picker() && input->is_open();

    return false;
}

static HTML::FormAssociatedElement const* as_form_associated_element(DOM::Element const& element)
{
    auto const* html_element = as_if<HTML::HTMLElement>(element);
    if (!html_element || !html_element->is_form_associated_element())
        return nullptr;
    return html_element;
}

static bool element_is_default(DOM::Element const& element)
{
    auto const* form_associated_element = as_form_associated_element(element);
    if (form_associated_element && form_associated_element->is_submit_button() && form_associated_element->form() && form_associated_element->form()->default_button() == form_associated_element)
        return true;
    if (auto const* input_element = as_if<HTML::HTMLInputElement>(element))
        return input_element->checked_applies() && input_element->has_attribute(HTML::AttributeNames::checked);
    if (auto const* option_element = as_if<HTML::HTMLOptionElement>(element))
        return option_element->has_attribute(HTML::AttributeNames::selected);
    return false;
}

static bool element_is_modal(DOM::Element const& element)
{
    auto const* dialog_element = as_if<HTML::HTMLDialogElement>(element);
    // FIXME: Fullscreen elements are also modal.
    return dialog_element && dialog_element->is_modal();
}

CSS::PseudoClassBitmap element_states(DOM::Element const& element)
{
    CSS::PseudoClassBitmap states;
    auto set = [&](CSS::PseudoClass pseudo_class, bool holds) {
        if (holds)
            states.set(pseudo_class, true);
    };

    // The states every element can be in, whatever it is.
    set(CSS::PseudoClass::Active, element.is_being_activated());
    set(CSS::PseudoClass::Defined, element.is_defined());
    set(CSS::PseudoClass::Focus, element.is_focused());
    set(CSS::PseudoClass::FocusVisible, element.is_focused() && element.should_indicate_focus());
    set(CSS::PseudoClass::FocusWithin, element.matches_focus_within_pseudo_class());
    set(CSS::PseudoClass::Fullscreen, element.is_fullscreen_element());
    set(CSS::PseudoClass::Hover, matches_hover_pseudo_class(element));
    set(CSS::PseudoClass::Target, element.is_target());
    set(CSS::PseudoClass::Open, matches_open_state_pseudo_class(element));
    set(CSS::PseudoClass::Modal, element_is_modal(element));
    set(CSS::PseudoClass::PopoverOpen, element_matches_state(element, CSS::PseudoClass::PopoverOpen));

    // Read-write is what the editing hosts answer, so it is asked of everything too.
    auto read_write = matches_read_write_pseudo_class(element);
    set(CSS::PseudoClass::ReadWrite, read_write);
    set(CSS::PseudoClass::ReadOnly, !read_write);

    // Link state is three answers to one question.
    auto is_link = element.matches_link_pseudo_class();
    auto is_visited = element.matches_visited_pseudo_class();
    set(CSS::PseudoClass::Link, is_link);
    set(CSS::PseudoClass::Visited, is_visited);
    set(CSS::PseudoClass::AnyLink, is_link || is_visited);
    if (is_link || is_visited)
        set(CSS::PseudoClass::LocalLink, element.matches_local_link_pseudo_class());

    // The rest belong to kinds of element, and asking a `<div>` about them costs one type check
    // rather than one call each.
    set(CSS::PseudoClass::Checked, element.matches_checked_pseudo_class());
    set(CSS::PseudoClass::Unchecked, element.matches_unchecked_pseudo_class());
    set(CSS::PseudoClass::Disabled, element.matches_disabled_pseudo_class());
    set(CSS::PseudoClass::Enabled, element.matches_enabled_pseudo_class());
    set(CSS::PseudoClass::PlaceholderShown, element.matches_placeholder_shown_pseudo_class());
    set(CSS::PseudoClass::Indeterminate, matches_indeterminate_pseudo_class(element));

    auto const* form_associated_element = as_form_associated_element(element);
    if (form_associated_element || is<HTML::HTMLOptionElement>(element))
        set(CSS::PseudoClass::Default, element_is_default(element));

    if (form_associated_element) {
        auto required = element_required_state(element);
        set(CSS::PseudoClass::Required, required == RequiredState::Required);
        set(CSS::PseudoClass::Optional, required == RequiredState::Optional);

        auto user_validity = element_user_validity_state(element);
        set(CSS::PseudoClass::UserValid, user_validity == ValidityState::Valid);
        set(CSS::PseudoClass::UserInvalid, user_validity == ValidityState::Invalid);
    }

    if (form_associated_element || is<HTML::HTMLFormElement>(element) || is<HTML::HTMLFieldSetElement>(element)) {
        auto validity = element_validity_state(element);
        set(CSS::PseudoClass::Valid, validity == ValidityState::Valid);
        set(CSS::PseudoClass::Invalid, validity == ValidityState::Invalid);
    }

    if (auto const* media_element = as_if<HTML::HTMLMediaElement>(element)) {
        auto blocked = media_element->blocked();
        auto paused = media_element->paused();
        set(CSS::PseudoClass::Buffering, blocked);
        set(CSS::PseudoClass::Muted, media_element->muted());
        set(CSS::PseudoClass::Paused, paused);
        // :playing tracks the paused attribute alone: it flips synchronously on play() and
        // load(), and does not require a playable media resource.
        set(CSS::PseudoClass::Playing, !paused);
        set(CSS::PseudoClass::Seeking, media_element->seeking());
        set(CSS::PseudoClass::Stalled, media_element->stalled());
    }

    if (auto const* meter = as_if<HTML::HTMLMeterElement>(element)) {
        auto value_state = meter->value_state();
        set(CSS::PseudoClass::EvenLessGoodValue, value_state == HTML::HTMLMeterElement::ValueState::EvenLessGood);
        set(CSS::PseudoClass::SuboptimalValue, value_state == HTML::HTMLMeterElement::ValueState::Suboptimal);
        set(CSS::PseudoClass::OptimalValue, value_state == HTML::HTMLMeterElement::ValueState::Optimal);
        set(CSS::PseudoClass::HighValue, meter->value() > meter->high());
        set(CSS::PseudoClass::LowValue, meter->value() < meter->low());
    }

    return states;
}

bool element_matches_state(DOM::Element const& element, CSS::PseudoClass pseudo_class)
{
    auto const* media_element = as_if<HTML::HTMLMediaElement>(element);
    auto const* meter = as_if<HTML::HTMLMeterElement>(element);
    auto meter_value_state = [&](HTML::HTMLMeterElement::ValueState state) {
        return meter && meter->value_state() == state;
    };

    switch (pseudo_class) {
    case CSS::PseudoClass::Active:
        return element.is_being_activated();
    case CSS::PseudoClass::AnyLink:
        return element.matches_link_pseudo_class() || element.matches_visited_pseudo_class();
    case CSS::PseudoClass::Autofill:
        // FIXME: Nothing autofills yet, so no element is ever in this state.
        return false;
    case CSS::PseudoClass::Buffering:
        return media_element && media_element->blocked();
    case CSS::PseudoClass::Checked:
        return element.matches_checked_pseudo_class();
    case CSS::PseudoClass::Default:
        return element_is_default(element);
    case CSS::PseudoClass::Defined:
        return element.is_defined();
    case CSS::PseudoClass::Disabled:
        return element.matches_disabled_pseudo_class();
    case CSS::PseudoClass::Enabled:
        return element.matches_enabled_pseudo_class();
    case CSS::PseudoClass::EvenLessGoodValue:
        return meter_value_state(HTML::HTMLMeterElement::ValueState::EvenLessGood);
    case CSS::PseudoClass::Focus:
        return element.is_focused();
    case CSS::PseudoClass::FocusVisible:
        return element.is_focused() && element.should_indicate_focus();
    case CSS::PseudoClass::FocusWithin:
        return element.matches_focus_within_pseudo_class();
    case CSS::PseudoClass::Fullscreen:
        return element.is_fullscreen_element();
    case CSS::PseudoClass::HighValue:
        return meter && meter->value() > meter->high();
    case CSS::PseudoClass::Hover:
        return matches_hover_pseudo_class(element);
    case CSS::PseudoClass::Indeterminate:
        return matches_indeterminate_pseudo_class(element);
    case CSS::PseudoClass::Invalid:
        return element_validity_state(element) == ValidityState::Invalid;
    case CSS::PseudoClass::Link:
        return element.matches_link_pseudo_class();
    case CSS::PseudoClass::LocalLink:
        return element.matches_local_link_pseudo_class();
    case CSS::PseudoClass::LowValue:
        return meter && meter->value() < meter->low();
    case CSS::PseudoClass::Modal:
        return element_is_modal(element);
    case CSS::PseudoClass::Muted:
        return media_element && media_element->muted();
    case CSS::PseudoClass::Open:
        return matches_open_state_pseudo_class(element);
    case CSS::PseudoClass::OptimalValue:
        return meter_value_state(HTML::HTMLMeterElement::ValueState::Optimal);
    case CSS::PseudoClass::Optional:
        return element_required_state(element) == RequiredState::Optional;
    case CSS::PseudoClass::Paused:
        return media_element && media_element->paused();
    case CSS::PseudoClass::PlaceholderShown:
        return element.matches_placeholder_shown_pseudo_class();
    case CSS::PseudoClass::Playing:
        return media_element && !media_element->blocked() && !media_element->paused();
    case CSS::PseudoClass::PopoverOpen:
        return element.has_attribute(HTML::AttributeNames::popover)
            && [&] {
                   auto const* html_element = as_if<HTML::HTMLElement>(element);
                   return html_element && html_element->popover_visibility_state() == HTML::HTMLElement::PopoverVisibilityState::Showing;
               }();
    case CSS::PseudoClass::ReadOnly:
        return !matches_read_write_pseudo_class(element);
    case CSS::PseudoClass::ReadWrite:
        return matches_read_write_pseudo_class(element);
    case CSS::PseudoClass::Required:
        return element_required_state(element) == RequiredState::Required;
    case CSS::PseudoClass::Seeking:
        return media_element && media_element->seeking();
    case CSS::PseudoClass::Stalled:
        return media_element && media_element->stalled();
    case CSS::PseudoClass::SuboptimalValue:
        return meter_value_state(HTML::HTMLMeterElement::ValueState::Suboptimal);
    case CSS::PseudoClass::Target:
        return element.is_target();
    case CSS::PseudoClass::Unchecked:
        return element.matches_unchecked_pseudo_class();
    case CSS::PseudoClass::UserInvalid:
        return element_user_validity_state(element) == ValidityState::Invalid;
    case CSS::PseudoClass::UserValid:
        return element_user_validity_state(element) == ValidityState::Valid;
    case CSS::PseudoClass::Valid:
        return element_validity_state(element) == ValidityState::Valid;
    case CSS::PseudoClass::Visited:
        return element.matches_visited_pseudo_class();
    case CSS::PseudoClass::VolumeLocked:
        // FIXME: Volume is never locked, so no element is ever in this state.
        return false;

    // The rest are operators over other selectors, positions, or the tree rather than facts the
    // element carries, so none of them is a state to publish.
    case CSS::PseudoClass::Dir:
    case CSS::PseudoClass::Empty:
    case CSS::PseudoClass::FirstChild:
    case CSS::PseudoClass::FirstOfType:
    case CSS::PseudoClass::Has:
    case CSS::PseudoClass::Heading:
    case CSS::PseudoClass::Host:
    case CSS::PseudoClass::Is:
    case CSS::PseudoClass::Lang:
    case CSS::PseudoClass::LastChild:
    case CSS::PseudoClass::LastOfType:
    case CSS::PseudoClass::Not:
    case CSS::PseudoClass::NthChild:
    case CSS::PseudoClass::NthLastChild:
    case CSS::PseudoClass::NthLastOfType:
    case CSS::PseudoClass::NthOfType:
    case CSS::PseudoClass::OnlyChild:
    case CSS::PseudoClass::OnlyOfType:
    case CSS::PseudoClass::Root:
    case CSS::PseudoClass::Scope:
    case CSS::PseudoClass::State:
    case CSS::PseudoClass::Where:
    case CSS::PseudoClass::__Count:
        return false;
    }
    VERIFY_NOT_REACHED();
}

ValidityState element_validity_state(DOM::Element const& target)
{
    if (auto const* form_associated_element = as_form_associated_element(target)) {
        if (form_associated_element->is_candidate_for_constraint_validation()) {
            return form_associated_element->satisfies_its_constraints() ? ValidityState::Valid : ValidityState::Invalid;
        }
    }

    auto const* form_element = as_if<HTML::HTMLFormElement>(target);
    if (!form_element && !is<HTML::HTMLFieldSetElement>(target))
        return ValidityState::NotApplicable;

    if (form_element)
        return form_element->has_invalid_associated_element() ? ValidityState::Invalid : ValidityState::Valid;

    bool has_invalid_elements = false;
    target.for_each_in_subtree([&](auto& node) {
        auto const* element = as_if<DOM::Element>(node);
        if (!element)
            return TraversalDecision::Continue;
        auto const* form_associated_element = as_form_associated_element(*element);
        if (!form_associated_element)
            return TraversalDecision::Continue;
        if (form_associated_element->is_candidate_for_constraint_validation() && !form_associated_element->satisfies_its_constraints()) {
            has_invalid_elements = true;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    return has_invalid_elements ? ValidityState::Invalid : ValidityState::Valid;
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-user-valid
// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-user-invalid
ValidityState element_user_validity_state(DOM::Element const& target)
{
    bool user_validity = false;
    if (auto const* input_element = as_if<HTML::HTMLInputElement>(target))
        user_validity = input_element->user_validity();
    else if (auto const* select_element = as_if<HTML::HTMLSelectElement>(target))
        user_validity = select_element->user_validity();
    else if (auto const* text_area_element = as_if<HTML::HTMLTextAreaElement>(target))
        user_validity = text_area_element->user_validity();
    else
        return ValidityState::NotApplicable;
    if (!user_validity)
        return ValidityState::NotApplicable;

    auto const& form_associated_element = as<HTML::FormAssociatedElement>(target);
    if (!form_associated_element.is_candidate_for_constraint_validation())
        return ValidityState::NotApplicable;
    return form_associated_element.satisfies_its_constraints() ? ValidityState::Valid : ValidityState::Invalid;
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-optional
// https://html.spec.whatwg.org/multipage/semantics-other.html#selector-required
RequiredState element_required_state(DOM::Element const& target)
{
    if (auto const* input_element = as_if<HTML::HTMLInputElement>(target)) {
        if (input_element->required_applies())
            return input_element->has_attribute(HTML::AttributeNames::required) ? RequiredState::Required : RequiredState::Optional;
        // AD-HOC: Chromium and WebKit also match :optional for hidden inputs.
        return input_element->type_state() == HTML::HTMLInputElement::TypeAttributeState::Hidden
            ? RequiredState::Optional
            : RequiredState::NotApplicable;
    }
    if (is<HTML::HTMLSelectElement>(target) || is<HTML::HTMLTextAreaElement>(target))
        return target.has_attribute(HTML::AttributeNames::required) ? RequiredState::Required : RequiredState::Optional;
    return RequiredState::NotApplicable;
}

static bool child_keeps_element_from_being_empty(DOM::Node const& child)
{
    if (is<DOM::Element>(child))
        return true;
    auto const* text = as_if<DOM::Text>(child);
    return text && !text->data().is_empty();
}

bool element_is_empty_ignoring_child(DOM::Element const& element, DOM::Node const& ignored_child)
{
    for (auto const* child = element.first_child(); child; child = child->next_sibling()) {
        if (child == &ignored_child)
            continue;
        if (child_keeps_element_from_being_empty(*child))
            return false;
    }
    return true;
}

}
