/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@gmta.nl>
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLButtonElement.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLLegendElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>

namespace Web::HTML {

static SelectionDirection string_to_selection_direction(Optional<String> value)
{
    if (!value.has_value())
        return SelectionDirection::None;
    if (value.value() == "forward"sv)
        return SelectionDirection::Forward;
    if (value.value() == "backward"sv)
        return SelectionDirection::Backward;
    return SelectionDirection::None;
}

void FormAssociatedElement::set_form(HTMLFormElement* form)
{
    if (m_form)
        m_form->remove_associated_element({}, form_associated_element_to_html_element());
    m_form = form;
    if (m_form)
        m_form->add_associated_element({}, form_associated_element_to_html_element());
}

bool FormAssociatedElement::enabled() const
{
    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-fe-disabled
    auto const& html_element = form_associated_element_to_html_element();

    // A form control is disabled if any of the following conditions are met:
    // 1. The element is a button, input, select, textarea, or form-associated custom element, and the disabled attribute is specified on this element (regardless of its value).
    // FIXME: This doesn't check for form-associated custom elements.
    if ((is<HTMLButtonElement>(html_element) || is<HTMLInputElement>(html_element) || is<HTMLSelectElement>(html_element) || is<HTMLTextAreaElement>(html_element)) && html_element.has_attribute(HTML::AttributeNames::disabled))
        return false;

    // 2. The element is a descendant of a fieldset element whose disabled attribute is specified, and is not a descendant of that fieldset element's first legend element child, if any.
    for (auto* fieldset_ancestor = html_element.first_ancestor_of_type<HTMLFieldSetElement>(); fieldset_ancestor; fieldset_ancestor = fieldset_ancestor->first_ancestor_of_type<HTMLFieldSetElement>()) {
        if (fieldset_ancestor->has_attribute(HTML::AttributeNames::disabled)) {
            auto* first_legend_element_child = fieldset_ancestor->first_child_of_type<HTMLLegendElement>();
            if (!first_legend_element_child || !html_element.is_descendant_of(*first_legend_element_child))
                return false;
        }
    }

    return true;
}

void FormAssociatedElement::set_parser_inserted(Badge<HTMLParser>)
{
    m_parser_inserted = true;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#association-of-controls-and-forms:nodes-are-inserted
void FormAssociatedElement::form_node_was_inserted()
{
    // 1. If the form-associated element's parser inserted flag is set, then return.
    if (m_parser_inserted)
        return;

    // 2. Reset the form owner of the form-associated element.
    reset_form_owner();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#association-of-controls-and-forms:nodes-are-removed
void FormAssociatedElement::form_node_was_removed()
{
    // 1. If the form-associated element has a form owner and the form-associated element and its form owner are no longer in the same tree, then reset the form owner of the form-associated element.
    if (m_form && &form_associated_element_to_html_element().root() != &m_form->root())
        reset_form_owner();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#association-of-controls-and-forms:category-listed-3
void FormAssociatedElement::form_node_attribute_changed(FlyString const& name, Optional<String> const& value)
{
    // When a listed form-associated element's form attribute is set, changed, or removed, then the user agent must
    // reset the form owner of that element.
    if (name == HTML::AttributeNames::form) {
        auto& html_element = form_associated_element_to_html_element();

        if (value.has_value())
            html_element.document().add_form_associated_element_with_form_attribute(*this);
        else
            html_element.document().remove_form_associated_element_with_form_attribute(*this);

        reset_form_owner();
    }
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#association-of-controls-and-forms:category-listed-4
void FormAssociatedElement::element_id_changed(Badge<DOM::Document>)
{
    // When a listed form-associated element has a form attribute and the ID of any of the elements in the tree changes,
    // then the user agent must reset the form owner of that form-associated element.
    VERIFY(form_associated_element_to_html_element().has_attribute(HTML::AttributeNames::form));
    reset_form_owner();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#association-of-controls-and-forms:category-listed-5
void FormAssociatedElement::element_with_id_was_added_or_removed(Badge<DOM::Document>)
{
    // When a listed form-associated element has a form attribute and an element with an ID is inserted into or removed
    // from the Document, then the user agent must reset the form owner of that form-associated element.
    VERIFY(form_associated_element_to_html_element().has_attribute(HTML::AttributeNames::form));
    reset_form_owner();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#reset-the-form-owner
void FormAssociatedElement::reset_form_owner()
{
    auto& html_element = form_associated_element_to_html_element();

    // 1. Unset element's parser inserted flag.
    m_parser_inserted = false;

    // 2. If all of the following conditions are true
    //    - element's form owner is not null
    //    - element is not listed or its form content attribute is not present
    //    - element's form owner is its nearest form element ancestor after the change to the ancestor chain
    //    then do nothing, and return.
    if (m_form
        && (!is_listed() || !html_element.has_attribute(HTML::AttributeNames::form))
        && html_element.first_ancestor_of_type<HTMLFormElement>() == m_form.ptr()) {
        return;
    }

    // 3. Set element's form owner to null.
    set_form(nullptr);

    // 4. If element is listed, has a form content attribute, and is connected, then:
    if (is_listed() && html_element.has_attribute(HTML::AttributeNames::form) && html_element.is_connected()) {
        // 1. If the first element in element's tree, in tree order, to have an ID that is identical to element's form content attribute's value, is a form element, then associate the element with that form element.
        auto form_value = html_element.attribute(HTML::AttributeNames::form);
        html_element.root().for_each_in_inclusive_subtree_of_type<HTMLFormElement>([this, &form_value](HTMLFormElement& form_element) {
            if (form_element.id() == form_value) {
                set_form(&form_element);
                return TraversalDecision::Break;
            }

            return TraversalDecision::Continue;
        });
    }

    // 5. Otherwise, if element has an ancestor form element, then associate element with the nearest such ancestor form element.
    else {
        auto* form_ancestor = html_element.first_ancestor_of_type<HTMLFormElement>();
        if (form_ancestor)
            set_form(form_ancestor);
    }
}

// https://w3c.github.io/webdriver/#dfn-clear-algorithm
void FormAssociatedElement::clear_algorithm()
{
    // When the clear algorithm is invoked for an element that does not define its own clear algorithm, its reset
    // algorithm must be invoked instead.
    reset_algorithm();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-fs-formaction
String FormAssociatedElement::form_action() const
{
    // The formAction IDL attribute must reflect the formaction content attribute, except that on getting, when the content attribute is missing or its value is the empty string,
    // the element's node document's URL must be returned instead.
    auto& html_element = form_associated_element_to_html_element();
    auto form_action_attribute = html_element.attribute(HTML::AttributeNames::formaction);
    if (!form_action_attribute.has_value() || form_action_attribute.value().is_empty()) {
        return html_element.document().url_string();
    }

    auto document_base_url = html_element.document().base_url();
    return MUST(document_base_url.complete_url(form_action_attribute.value()).to_string());
}

WebIDL::ExceptionOr<void> FormAssociatedElement::set_form_action(String const& value)
{
    auto& html_element = form_associated_element_to_html_element();
    return html_element.set_attribute(HTML::AttributeNames::formaction, value);
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-textarea/input-relevant-value
void FormAssociatedTextControlElement::relevant_value_was_changed(JS::GCPtr<DOM::Text> text_node)
{
    auto the_relevant_value = relevant_value();
    auto relevant_value_length = the_relevant_value.code_points().length();

    // 1. If the element has a selection:
    if (m_selection_start < m_selection_end) {
        // 1. If the start of the selection is now past the end of the relevant value, set it to
        //    the end of the relevant value.
        if (m_selection_start > relevant_value_length)
            m_selection_start = relevant_value_length;

        // 2. If the end of the selection is now past the end of the relevant value, set it to the
        //    end of the relevant value.
        if (m_selection_end > relevant_value_length)
            m_selection_end = relevant_value_length;

        // 3. If the user agent does not support empty selection, and both the start and end of the
        //    selection are now pointing to the end of the relevant value, then instead set the
        //    element's text entry cursor position to the end of the relevant value, removing any
        //    selection.
        // NOTE: We support empty selections.
        return;
    }

    // 2. Otherwise, the element must have a text entry cursor position position. If it is now past
    //    the end of the relevant value, set it to the end of the relevant value.
    auto& document = form_associated_element_to_html_element().document();
    auto const current_cursor_position = document.cursor_position();
    if (current_cursor_position && text_node
        && current_cursor_position->node() == text_node
        && current_cursor_position->offset() > relevant_value_length) {
        document.set_cursor_position(DOM::Position::create(document.realm(), *text_node, relevant_value_length));
    }
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-select
WebIDL::ExceptionOr<void> FormAssociatedTextControlElement::select()
{
    // 1. If this element is an input element, and either select() does not apply to this element
    //    or the corresponding control has no selectable text, return.
    auto& html_element = form_associated_element_to_html_element();
    if (is<HTMLInputElement>(html_element)) {
        auto& input_element = static_cast<HTMLInputElement&>(html_element);
        if (!input_element.select_applies() || !input_element.has_selectable_text())
            return {};
    }

    // 2. Set the selection range with 0 and infinity.
    set_the_selection_range(0, NumericLimits<WebIDL::UnsignedLong>::max());
    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-selectionstart
Optional<WebIDL::UnsignedLong> FormAssociatedTextControlElement::selection_start() const
{
    // 1. If this element is an input element, and selectionStart does not apply to this element, return null.
    auto const& html_element = form_associated_element_to_html_element();
    if (is<HTMLInputElement>(html_element)) {
        auto const& input_element = static_cast<HTMLInputElement const&>(html_element);
        if (!input_element.selection_or_range_applies())
            return {};
    }

    // 2. If there is no selection, return the code unit offset within the relevant value to the character that
    //    immediately follows the text entry cursor.
    if (m_selection_start == m_selection_end) {
        if (auto cursor = form_associated_element_to_html_element().document().cursor_position())
            return cursor->offset();
    }

    // 3. Return the code unit offset within the relevant value to the character that immediately follows the start of
    //    the selection.
    return m_selection_start;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#textFieldSelection:dom-textarea/input-selectionstart-2
WebIDL::ExceptionOr<void> FormAssociatedTextControlElement::set_selection_start(Optional<WebIDL::UnsignedLong> const& value)
{
    // 1. If this element is an input element, and selectionStart does not apply to this element,
    //    throw an "InvalidStateError" DOMException.
    auto& html_element = form_associated_element_to_html_element();
    if (is<HTMLInputElement>(html_element)) {
        auto& input_element = static_cast<HTMLInputElement&>(html_element);
        if (!input_element.selection_or_range_applies())
            return WebIDL::InvalidStateError::create(html_element.realm(), "setSelectionStart does not apply to this input type"_fly_string);
    }

    // 2. Let end be the value of this element's selectionEnd attribute.
    auto end = m_selection_end;

    // 3. If end is less than the given value, set end to the given value.
    if (value.has_value() && end < value.value())
        end = value.value();

    // 4. Set the selection range with the given value, end, and the value of this element's
    //    selectionDirection attribute.
    set_the_selection_range(value, end, selection_direction_state());
    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-selectionend
Optional<WebIDL::UnsignedLong> FormAssociatedTextControlElement::selection_end() const
{
    // 1. If this element is an input element, and selectionEnd does not apply to this element, return
    //    null.
    auto const& html_element = form_associated_element_to_html_element();
    if (is<HTMLInputElement>(html_element)) {
        auto const& input_element = static_cast<HTMLInputElement const&>(html_element);
        if (!input_element.selection_or_range_applies())
            return {};
    }

    // 2. If there is no selection, return the code unit offset within the relevant value to the
    //    character that immediately follows the text entry cursor.
    if (m_selection_start == m_selection_end) {
        if (auto cursor = form_associated_element_to_html_element().document().cursor_position())
            return cursor->offset();
    }

    // 3. Return the code unit offset within the relevant value to the character that immediately
    //    follows the end of the selection.
    return m_selection_end;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#textFieldSelection:dom-textarea/input-selectionend-3
WebIDL::ExceptionOr<void> FormAssociatedTextControlElement::set_selection_end(Optional<WebIDL::UnsignedLong> const& value)
{
    // 1. If this element is an input element, and selectionEnd does not apply to this element,
    //    throw an "InvalidStateError" DOMException.
    auto& html_element = form_associated_element_to_html_element();
    if (is<HTMLInputElement>(html_element)) {
        auto& input_element = static_cast<HTMLInputElement&>(html_element);
        if (!input_element.selection_or_range_applies())
            return WebIDL::InvalidStateError::create(html_element.realm(), "setSelectionEnd does not apply to this input type"_fly_string);
    }

    // 2. Set the selection range with the value of this element's selectionStart attribute, the
    //    given value, and the value of this element's selectionDirection attribute.
    set_the_selection_range(m_selection_start, value, selection_direction_state());
    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#selection-direction
Optional<String> FormAssociatedTextControlElement::selection_direction() const
{
    // 1. If this element is an input element, and selectionDirection does not apply to this
    //    element, return null.
    auto const& html_element = form_associated_element_to_html_element();
    if (is<HTMLInputElement>(html_element)) {
        auto const& input_element = static_cast<HTMLInputElement const&>(html_element);
        if (!input_element.selection_or_range_applies())
            return {};
    }

    // 2. Return this element's selection direction.
    switch (m_selection_direction) {
    case SelectionDirection::Forward:
        return "forward"_string;
    case SelectionDirection::Backward:
        return "backward"_string;
    case SelectionDirection::None:
        return "none"_string;
    default:
        VERIFY_NOT_REACHED();
    }
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#set-the-selection-direction
void FormAssociatedTextControlElement::set_selection_direction(Optional<String> direction)
{
    // To set the selection direction of an element to a given direction, update the element's
    // selection direction to the given direction, unless the direction is "none" and the
    // platform does not support that direction; in that case, update the element's selection
    // direction to "forward".
    m_selection_direction = string_to_selection_direction(direction);
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-selectiondirection
WebIDL::ExceptionOr<void> FormAssociatedTextControlElement::set_selection_direction_binding(Optional<String> direction)
{
    // 1. If this element is an input element, and selectionDirection does not apply to this element,
    //    throw an "InvalidStateError" DOMException.
    auto const& html_element = form_associated_element_to_html_element();
    if (is<HTMLInputElement>(html_element)) {
        auto const& input_element = static_cast<HTMLInputElement const&>(html_element);
        if (!input_element.selection_direction_applies())
            return WebIDL::InvalidStateError::create(input_element.realm(), "selectionDirection does not apply to element"_fly_string);
    }

    set_the_selection_range(m_selection_start, m_selection_end, string_to_selection_direction(direction));
    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-setrangetext
WebIDL::ExceptionOr<void> FormAssociatedTextControlElement::set_range_text(String const& replacement)
{
    return set_range_text(replacement, m_selection_start, m_selection_end);
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-setrangetext
WebIDL::ExceptionOr<void> FormAssociatedTextControlElement::set_range_text(String const& replacement, WebIDL::UnsignedLong start, WebIDL::UnsignedLong end, Bindings::SelectionMode selection_mode)
{
    // 1. If this element is an input element, and setRangeText() does not apply to this element,
    //    throw an "InvalidStateError" DOMException.
    auto& html_element = form_associated_element_to_html_element();
    if (is<HTMLInputElement>(html_element) && !static_cast<HTMLInputElement&>(html_element).selection_or_range_applies())
        return WebIDL::InvalidStateError::create(html_element.realm(), "setRangeText does not apply to this input type"_fly_string);

    // 2. Set this element's dirty value flag to true.
    set_dirty_value_flag(true);

    // 3. If the method has only one argument, then let start and end have the values of the selectionStart attribute and the selectionEnd attribute respectively.
    //    Otherwise, let start, end have the values of the second and third arguments respectively.
    // NOTE: This is handled by the caller.

    // 4. If start is greater than end, then throw an "IndexSizeError" DOMException.
    if (start > end)
        return WebIDL::IndexSizeError::create(html_element.realm(), "The start argument must be less than or equal to the end argument"_fly_string);

    // 5. If start is greater than the length of the relevant value of the text control, then set it to the length of the relevant value of the text control.
    auto the_relevant_value = relevant_value();
    auto relevant_value_length = the_relevant_value.code_points().length();
    if (start > relevant_value_length)
        start = relevant_value_length;

    // 6. If end is greater than the length of the relevant value of the text control, then set it to the length of the relevant value of the text control.
    if (end > relevant_value_length)
        end = relevant_value_length;

    // 7. Let selection start be the current value of the selectionStart attribute.
    auto selection_start = m_selection_start;

    // 8. Let selection end be the current value of the selectionEnd attribute.
    auto selection_end = m_selection_end;

    // 9. If start is less than end, delete the sequence of code units within the element's relevant value starting with
    //    the code unit at the startth position and ending with the code unit at the (end-1)th position.
    if (start < end) {
        StringBuilder builder;
        auto before_removal_point_view = the_relevant_value.code_points().unicode_substring_view(0, start);
        builder.append(before_removal_point_view.as_string());
        auto after_removal_point_view = the_relevant_value.code_points().unicode_substring_view(end);
        builder.append(after_removal_point_view.as_string());
        the_relevant_value = MUST(builder.to_string());
    }

    // 10. Insert the value of the first argument into the text of the relevant value of the text control, immediately before the startth code unit.
    StringBuilder builder;
    auto before_insertion_point_view = the_relevant_value.code_points().unicode_substring_view(0, start);
    builder.append(before_insertion_point_view.as_string());
    builder.append(replacement);
    auto after_insertion_point_view = the_relevant_value.code_points().unicode_substring_view(start);
    builder.append(after_insertion_point_view.as_string());
    the_relevant_value = MUST(builder.to_string());
    TRY(set_relevant_value(the_relevant_value));

    // 11. Let new length be the length of the value of the first argument.
    i64 new_length = replacement.code_points().length();

    // 12. Let new end be the sum of start and new length.
    auto new_end = start + new_length;

    // 13. Run the appropriate set of substeps from the following list:
    switch (selection_mode) {
    // If the fourth argument's value is "select"
    case Bindings::SelectionMode::Select:
        // Let selection start be start.
        selection_start = start;

        // Let selection end be new end.
        selection_end = new_end;
        break;

    // If the fourth argument's value is "start"
    case Bindings::SelectionMode::Start:
        // Let selection start and selection end be start.
        selection_start = start;
        selection_end = start;
        break;

    // If the fourth argument's value is "end"
    case Bindings::SelectionMode::End:
        selection_start = new_end;
        selection_end = new_end;
        break;

    // If the fourth argument's value is "preserve"
    case Bindings::SelectionMode::Preserve:
        // 1. Let old length be end minus start.
        auto old_length = end - start;

        // 2. Let delta be new length minus old length.
        auto delta = new_length - old_length;

        // 3. If selection start is greater than end, then increment it by delta.
        //    (If delta is negative, i.e. the new text is shorter than the old text, then this will decrease the value of selection start.)
        //    Otherwise: if selection start is greater than start, then set it to start.
        //    (This snaps the start of the selection to the start of the new text if it was in the middle of the text that it replaced.)
        if (selection_start > end)
            selection_start += delta;
        else if (selection_start > start)
            selection_start = start;

        // 4. If selection end is greater than end, then increment it by delta in the same way.
        //    Otherwise: if selection end is greater than start, then set it to new end.
        //    (This snaps the end of the selection to the end of the new text if it was in the middle of the text that it replaced.)
        if (selection_end > end)
            selection_end += delta;
        else if (selection_end > start)
            selection_end = new_end;
        break;
    }

    // 14. Set the selection range with selection start and selection end.
    set_the_selection_range(selection_start, selection_end);

    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-setselectionrange
WebIDL::ExceptionOr<void> FormAssociatedTextControlElement::set_selection_range(Optional<WebIDL::UnsignedLong> start, Optional<WebIDL::UnsignedLong> end, Optional<String> direction)
{
    // 1. If this element is an input element, and setSelectionRange() does not apply to this
    //    element, throw an "InvalidStateError" DOMException.
    auto& html_element = form_associated_element_to_html_element();
    if (is<HTMLInputElement>(html_element) && !static_cast<HTMLInputElement&>(html_element).selection_or_range_applies())
        return WebIDL::InvalidStateError::create(html_element.realm(), "setSelectionRange does not apply to this input type"_fly_string);

    // 2. Set the selection range with start, end, and direction.
    set_the_selection_range(start, end, string_to_selection_direction(direction));
    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#set-the-selection-range
void FormAssociatedTextControlElement::set_the_selection_range(Optional<WebIDL::UnsignedLong> start, Optional<WebIDL::UnsignedLong> end, SelectionDirection direction, SelectionSource source)
{
    // 1. If start is null, let start be zero.
    start = start.value_or(0);

    // 2. If end is null, let end be zero.
    end = end.value_or(0);

    // 3. Set the selection of the text control to the sequence of code units within the relevant
    //    value starting with the code unit at the startth position (in logical order) and ending
    //    with the code unit at the (end-1)th position. Arguments greater than the length of the
    //    relevant value of the text control (including the special value infinity) must be treated
    //    as pointing at the end of the text control.
    auto the_relevant_value = relevant_value();
    auto relevant_value_length = the_relevant_value.code_points().length();
    auto new_selection_start = AK::min(start.value(), relevant_value_length);
    auto new_selection_end = AK::min(end.value(), relevant_value_length);

    //    If end is less than or equal to start then the start of the selection and the end of the
    //    selection must both be placed immediately before the character with offset end. In UAs
    //    where there is no concept of an empty selection, this must set the cursor to be just
    //    before the character with offset end.
    new_selection_start = AK::min(new_selection_start, new_selection_end);

    bool was_modified = m_selection_start != new_selection_start || m_selection_end != new_selection_end;
    m_selection_start = new_selection_start;
    m_selection_end = new_selection_end;

    // 4. If direction is not identical to either "backward" or "forward", or if the direction
    //    argument was not given, set direction to "none".
    // NOTE: This is handled by the argument's default value and ::string_to_selection_direction().

    // 5. Set the selection direction of the text control to direction.
    was_modified |= m_selection_direction != direction;
    m_selection_direction = direction;

    // 6. If the previous steps caused the selection of the text control to be modified (in either
    //    extent or direction), then queue an element task on the user interaction task source
    //    given the element to fire an event named select at the element, with the bubbles attribute
    //    initialized to true.
    if (was_modified) {
        auto& html_element = form_associated_element_to_html_element();

        // AD-HOC: We don't fire the event if the user moves the cursor without selecting any text.
        //         This is not in the spec but matches how other browsers behave.
        if (source == SelectionSource::DOM || m_selection_start != m_selection_end) {
            html_element.queue_an_element_task(Task::Source::UserInteraction, [&html_element] {
                auto select_event = DOM::Event::create(html_element.realm(), EventNames::select, { .bubbles = true });
                static_cast<DOM::EventTarget*>(&html_element)->dispatch_event(select_event);
            });
        }

        // AD-HOC: Notify the element that the selection was changed, so it can perform
        //         element-specific updates.
        selection_was_changed(m_selection_start, m_selection_end);
    }
}

}
