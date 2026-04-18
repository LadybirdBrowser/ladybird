/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Weak.h>
#include <LibWeb/Bindings/HTMLFormElement.h>
#include <LibWeb/DOM/InputEventsTarget.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Types.h>
#include <LibWeb/XHR/FormDataEntry.h>

namespace Web::HTML {

struct ValidityStateFlags {
    bool value_missing = false;
    bool type_mismatch = false;
    bool pattern_mismatch = false;
    bool too_long = false;
    bool too_short = false;
    bool range_underflow = false;
    bool range_overflow = false;
    bool step_mismatch = false;
    bool bad_input = false;
    bool custom_error = false;

    bool has_one_or_more_true_values() const
    {
        return value_missing || type_mismatch || pattern_mismatch || too_long || too_short || range_underflow || range_overflow || step_mismatch || bad_input || custom_error;
    }
};

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#selection-direction
enum class SelectionDirection {
    Forward,
    Backward,
    None,
};

class WEB_API FormAssociatedElement {
public:
    // NB: FACE stands for form-associated custom element.
    using FACESubmissionValue = Variant<GC::Ref<FileAPI::File>, String, GC::ConservativeVector<XHR::FormDataEntry>, Empty>;

    virtual bool is_form_associated_element() const;

    HTMLFormElement* form() { return m_form; }
    HTMLFormElement const* form() const { return m_form; }

    void set_form(HTMLFormElement*);

    void element_id_changed(Badge<DOM::Document>);
    void element_with_id_was_added_or_removed(Badge<DOM::Document>);

    bool enabled() const;

    void set_parser_inserted(Badge<HTMLParser>);

    // https://html.spec.whatwg.org/multipage/forms.html#category-listed
    virtual bool is_listed() const;

    // https://html.spec.whatwg.org/multipage/forms.html#category-submit
    virtual bool is_submittable() const;

    // https://html.spec.whatwg.org/multipage/forms.html#category-reset
    virtual bool is_resettable() const;

    // https://html.spec.whatwg.org/multipage/forms.html#category-autocapitalize
    virtual bool is_autocapitalize_and_autocorrect_inheriting() const { return false; }

    // https://html.spec.whatwg.org/multipage/forms.html#concept-button
    virtual bool is_button() const { return false; }

    // https://html.spec.whatwg.org/multipage/forms.html#concept-submit-button
    virtual bool is_submit_button() const { return false; }

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#check-validity-steps
    bool check_validity_steps();

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#report-validity-steps
    bool report_validity_steps();

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#candidate-for-constraint-validation
    bool is_candidate_for_constraint_validation() const;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-fv-valid
    bool satisfies_its_constraints() const;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-fs-novalidate
    bool novalidate_state() const;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure/#definitions
    virtual bool suffering_from_being_missing() const;
    virtual bool suffering_from_a_type_mismatch() const;
    virtual bool suffering_from_a_pattern_mismatch() const;
    virtual bool suffering_from_being_too_long() const;
    virtual bool suffering_from_being_too_short() const;
    virtual bool suffering_from_an_underflow() const;
    virtual bool suffering_from_an_overflow() const;
    virtual bool suffering_from_a_step_mismatch() const;
    virtual bool suffering_from_bad_input() const;
    bool suffering_from_a_custom_error() const;

    virtual Utf16String form_value() const { return {}; }
    virtual Optional<String> optional_value() const { VERIFY_NOT_REACHED(); }

    virtual HTMLElement& form_associated_element_to_html_element() = 0;
    HTMLElement const& form_associated_element_to_html_element() const { return const_cast<FormAssociatedElement&>(*this).form_associated_element_to_html_element(); }

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-form-reset-control
    virtual void reset_algorithm();

    virtual void clear_algorithm();

    String form_action() const;
    void set_form_action(String const&);

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-reportvalidity
    bool report_validity();

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-checkvalidity
    bool check_validity();

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-willvalidate
    bool will_validate() const;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-validationmessage
    Utf16String validation_message() const;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-validity
    GC::Ref<ValidityState const> validity() const;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-setcustomvalidity
    void set_custom_validity(String& error);

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#mutability
    virtual bool is_mutable() const { return true; }

    void reset_form_owner();

    void update_face_disabled_state();

    ValidityStateFlags const& face_validity_flags() const { return m_face_validity_flags; }
    void set_face_validity_flags(Badge<ElementInternals>, ValidityStateFlags const& value);

    String const& face_validation_message() const { return m_face_validation_message; }
    void set_face_validation_message(Badge<ElementInternals>, String const& value);

    void set_face_validation_anchor(Badge<ElementInternals>, GC::Ptr<HTMLElement> value);

    FACESubmissionValue const& face_submission_value() const { return m_face_submission_value; }
    void set_face_submission_value(Badge<ElementInternals>, FACESubmissionValue const& value);

    FACESubmissionValue const& face_state() const { return m_face_state; }
    void set_face_state(Badge<ElementInternals>, FACESubmissionValue const& value);

    void set_custom_validity_error_message(Badge<ElementInternals>, String const& value) { m_custom_validity_error_message = value; }

protected:
    FormAssociatedElement() = default;
    virtual ~FormAssociatedElement() = default;

    virtual void form_associated_element_was_inserted();
    virtual void form_associated_element_was_removed(DOM::Node*);
    virtual void form_associated_element_was_moved(GC::Ptr<DOM::Node>);
    virtual void form_associated_element_attribute_changed(FlyString const&, Optional<String> const&, Optional<String> const&, Optional<FlyString> const&);

    void form_node_was_inserted();
    void form_node_was_removed();
    void form_node_was_moved();
    void form_node_attribute_changed(FlyString const&, Optional<String> const&);

    void visit_edges(JS::Cell::Visitor&);

private:
    GC::Weak<HTMLFormElement> m_form;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#parser-inserted-flag
    bool m_parser_inserted { false };

    ValidityStateFlags m_face_validity_flags {};

    // https://html.spec.whatwg.org/multipage/custom-elements.html#face-validation-message
    // Each form-associated custom element has a validation message string. It is the empty string initially.
    String m_face_validation_message;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#face-validation-anchor
    // Each form-associated custom element has a validation anchor element. It is null initially.
    GC::Weak<HTMLElement> m_face_validation_anchor;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#face-submission-value
    // Each form-associated custom element has submission value. It is used to provide one or more entries on form submission.
    // The initial value of submission value is null, and submission value can be null, a string, a File, or a list of entries.
    FACESubmissionValue m_face_submission_value;

    // https://html.spec.whatwg.org/multipage/custom-elements.html#face-state
    // Each form-associated custom element has state. It is information with which the user agent can restore a user's input
    // for the element. The initial value of state is null, and state can be null, a string, a File, or a list of entries.
    FACESubmissionValue m_face_state;

    // AD-HOC: Cached disabled state for form-associated custom elements, used to detect changes
    //         and enqueue formDisabledCallback. Only meaningful for FACEs.
    bool m_face_disabled_state { false };

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#custom-validity-error-message
    String m_custom_validity_error_message;
};

enum class SelectionSource {
    UI,
    DOM,
};

class WEB_API FormAssociatedTextControlElement
    : public InputEventsTarget {
public:
    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-textarea/input-relevant-value
    virtual Utf16String relevant_value() const = 0;
    virtual WebIDL::ExceptionOr<void> set_relevant_value(Utf16String const&) = 0;
    virtual Optional<Utf16String> selected_text_for_stringifier() const;

    virtual void set_dirty_value_flag(bool flag) = 0;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-select
    WebIDL::ExceptionOr<void> select();

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-selectionstart
    Optional<WebIDL::UnsignedLong> selection_start_binding() const;
    WebIDL::ExceptionOr<void> set_selection_start_binding(Optional<WebIDL::UnsignedLong> const&);
    WebIDL::UnsignedLong selection_start() const;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-selectionend
    Optional<WebIDL::UnsignedLong> selection_end_binding() const;
    WebIDL::ExceptionOr<void> set_selection_end_binding(Optional<WebIDL::UnsignedLong> const&);
    WebIDL::UnsignedLong selection_end() const;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-selectiondirection
    Optional<String> selection_direction() const;
    void set_selection_direction(Optional<String> direction);
    WebIDL::ExceptionOr<void> set_selection_direction_binding(Optional<String> direction);
    SelectionDirection selection_direction_state() const { return m_selection_direction; }

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-setrangetext
    WebIDL::ExceptionOr<void> set_range_text_binding(Utf16String const& replacement);
    WebIDL::ExceptionOr<void> set_range_text_binding(Utf16String const& replacement, WebIDL::UnsignedLong start, WebIDL::UnsignedLong end, Bindings::SelectionMode = Bindings::SelectionMode::Preserve);
    WebIDL::ExceptionOr<void> set_range_text(Utf16String const& replacement, WebIDL::UnsignedLong start, WebIDL::UnsignedLong end, Bindings::SelectionMode = Bindings::SelectionMode::Preserve);

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-setselectionrange
    void set_the_selection_range(Optional<WebIDL::UnsignedLong> start, Optional<WebIDL::UnsignedLong> end, SelectionDirection direction = SelectionDirection::None, SelectionSource source = SelectionSource::DOM);

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-setselectionrange
    WebIDL::ExceptionOr<void> set_selection_range(Optional<WebIDL::UnsignedLong> start, Optional<WebIDL::UnsignedLong> end, Optional<String> direction);

    // https://w3c.github.io/selection-api/#dfn-has-scheduled-selectionchange-event
    bool has_scheduled_selectionchange_event() const { return m_has_scheduled_selectionchange_event; }
    void set_scheduled_selectionchange_event(bool value) { m_has_scheduled_selectionchange_event = value; }

    virtual HTMLElement& text_control_to_html_element() = 0;
    HTMLElement const& text_control_to_html_element() const { return const_cast<FormAssociatedTextControlElement&>(*this).text_control_to_html_element(); }

    virtual void did_edit_text_node(FlyString const& input_type, Optional<Utf16String> const& data) = 0;

    virtual GC::Ptr<DOM::Text> form_associated_element_to_text_node() = 0;
    virtual GC::Ptr<DOM::Text const> form_associated_element_to_text_node() const { return const_cast<FormAssociatedTextControlElement&>(*this).form_associated_element_to_text_node(); }

    virtual GC::Ptr<DOM::Element> text_control_scroll_container() = 0;

    virtual void handle_insert(FlyString const& input_type, Utf16String const&) override;
    virtual void handle_delete(FlyString const& input_type) override;
    virtual void select_all() override;
    virtual void set_selection_anchor(GC::Ref<DOM::Node>, size_t offset) override;
    virtual void set_selection_focus(GC::Ref<DOM::Node>, size_t offset) override;
    virtual void move_cursor_to_start(CollapseSelection) override;
    virtual void move_cursor_to_end(CollapseSelection) override;
    virtual void increment_cursor_position_offset(CollapseSelection) override;
    virtual void decrement_cursor_position_offset(CollapseSelection) override;
    virtual void increment_cursor_position_to_next_word(CollapseSelection) override;
    virtual void decrement_cursor_position_to_previous_word(CollapseSelection) override;
    virtual void increment_cursor_position_to_next_line(CollapseSelection) override;
    virtual void decrement_cursor_position_to_previous_line(CollapseSelection) override;

    GC::Ptr<DOM::Position> cursor_position() const;

protected:
    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-textarea/input-relevant-value
    void relevant_value_was_changed();

private:
    virtual GC::Ref<JS::Cell> as_cell() override;

    void collapse_selection_to_offset(size_t);
    void scroll_cursor_into_view();
    void selection_was_changed(SelectionSource);

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-textarea/input-selection
    WebIDL::UnsignedLong m_selection_start { 0 };
    WebIDL::UnsignedLong m_selection_end { 0 };
    SelectionDirection m_selection_direction { SelectionDirection::None };

    // https://w3c.github.io/selection-api/#dfn-has-scheduled-selectionchange-event
    bool m_has_scheduled_selectionchange_event { false };
};

}
