/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/Weak.h>
#include <LibWeb/Bindings/ElementInternals.h>
#include <LibWeb/Bindings/HTMLFormElement.h>
#include <LibWeb/DOM/InputEventsTarget.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/Types.h>
#include <LibWeb/XHR/FormDataEntry.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#selection-direction
enum class SelectionDirection {
    Forward,
    Backward,
    None,
};

class WEB_API FormAssociatedElement {
public:
    // NB: FACE stands for form-associated custom element.
    using FACESubmissionValue = Variant<GC::Ref<FileAPI::File>, Utf16String, GC::ConservativeVector<XHR::FormDataEntry>, Empty>;

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
    virtual Optional<Utf16String> optional_value() const { VERIFY_NOT_REACHED(); }

    virtual HTMLElement& form_associated_element_to_html_element() = 0;
    HTMLElement const& form_associated_element_to_html_element() const { return const_cast<FormAssociatedElement&>(*this).form_associated_element_to_html_element(); }

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-form-reset-control
    virtual void reset_algorithm();

    virtual void clear_algorithm();

    Utf16String form_action() const;
    void set_form_action(Utf16View);

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
    void set_custom_validity(Utf16String& error);

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#mutability
    virtual bool is_mutable() const { return true; }

    void reset_form_owner();

    void update_face_disabled_state();

    Bindings::ValidityStateFlags const& face_validity_flags() const { return m_face_validity_flags; }
    void set_face_validity_flags(Badge<ElementInternals>, Bindings::ValidityStateFlags const& value);

    Utf16String const& face_validation_message() const { return m_face_validation_message; }
    void set_face_validation_message(Badge<ElementInternals>, Utf16View value);

    void set_face_validation_anchor(Badge<ElementInternals>, GC::Ptr<HTMLElement> value);

    FACESubmissionValue const& face_submission_value() const { return m_face_submission_value; }
    void set_face_submission_value(Badge<ElementInternals>, FACESubmissionValue const& value);

    FACESubmissionValue const& face_state() const { return m_face_state; }
    void set_face_state(Badge<ElementInternals>, FACESubmissionValue const& value);

    void set_custom_validity_error_message(Badge<ElementInternals>, Utf16View value) { m_custom_validity_error_message = Utf16String::from_utf16(value); }

protected:
    FormAssociatedElement() = default;
    virtual ~FormAssociatedElement() = default;

    virtual void form_associated_element_was_inserted();
    virtual void form_associated_element_was_removed(DOM::Node*);
    virtual void form_associated_element_was_moved(GC::Ptr<DOM::Node>);
    virtual void form_associated_element_attribute_changed(Utf16FlyString const&, Optional<Utf16String> const&, Optional<Utf16String> const&, Optional<Utf16FlyString> const&);

    void form_node_was_inserted();
    void form_node_was_removed();
    void form_node_was_moved();
    void form_node_attribute_changed(Utf16FlyString const&, Optional<Utf16String> const&);

    void visit_edges(JS::Cell::Visitor&);

private:
    GC::Weak<HTMLFormElement> m_form;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#parser-inserted-flag
    bool m_parser_inserted { false };

    Bindings::ValidityStateFlags m_face_validity_flags {};

    // https://html.spec.whatwg.org/multipage/custom-elements.html#face-validation-message
    // Each form-associated custom element has a validation message string. It is the empty string initially.
    Utf16String m_face_validation_message;

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
    Utf16String m_custom_validity_error_message;
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
    virtual WebIDL::ExceptionOr<void> set_relevant_value(Utf16View) = 0;
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
    Optional<Utf16FlyString> selection_direction() const;
    void set_selection_direction(Optional<Utf16String> const& direction);
    WebIDL::ExceptionOr<void> set_selection_direction_binding(Utf16View direction);
    WebIDL::ExceptionOr<void> set_selection_direction_binding(Optional<Utf16String> const& direction);
    SelectionDirection selection_direction_state() const { return m_selection_direction; }

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-setrangetext
    WebIDL::ExceptionOr<void> set_range_text_binding(Utf16View replacement);
    WebIDL::ExceptionOr<void> set_range_text_binding(Utf16View replacement, WebIDL::UnsignedLong start, WebIDL::UnsignedLong end, Bindings::SelectionMode = Bindings::SelectionMode::Preserve);
    WebIDL::ExceptionOr<void> set_range_text(Utf16View replacement, WebIDL::UnsignedLong start, WebIDL::UnsignedLong end, Bindings::SelectionMode = Bindings::SelectionMode::Preserve);

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-setselectionrange
    void set_the_selection_range(Optional<WebIDL::UnsignedLong> start, Optional<WebIDL::UnsignedLong> end, SelectionDirection direction = SelectionDirection::None, SelectionSource source = SelectionSource::DOM);

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-setselectionrange
    WebIDL::ExceptionOr<void> set_selection_range(Optional<WebIDL::UnsignedLong> start, Optional<WebIDL::UnsignedLong> end, Optional<Utf16String> const& direction);

    // https://w3c.github.io/selection-api/#dfn-has-scheduled-selectionchange-event
    bool has_scheduled_selectionchange_event() const { return m_has_scheduled_selectionchange_event; }
    void set_scheduled_selectionchange_event(bool value) { m_has_scheduled_selectionchange_event = value; }

    virtual HTMLElement& text_control_to_html_element() = 0;
    HTMLElement const& text_control_to_html_element() const { return const_cast<FormAssociatedTextControlElement&>(*this).text_control_to_html_element(); }

    virtual void did_edit_text_node(Utf16FlyString const& input_type, Optional<Utf16String> const& data) = 0;

    virtual GC::Ptr<DOM::Text> form_associated_element_to_text_node() = 0;
    virtual GC::Ptr<DOM::Text const> form_associated_element_to_text_node() const { return const_cast<FormAssociatedTextControlElement&>(*this).form_associated_element_to_text_node(); }

    virtual GC::Ptr<DOM::Element> text_control_scroll_container() = 0;

    virtual void handle_insert(Utf16FlyString const& input_type, Utf16View) override;
    virtual void handle_delete(Utf16FlyString const& input_type, DispatchInputEvent = DispatchInputEvent::Yes) override;
    virtual GC::Ptr<DOM::Node> mouse_selection_scope() override;
    virtual void select_all() override;
    virtual void set_selection_anchor(GC::Ref<DOM::Node>, size_t offset, TextAffinity = TextAffinity::Downstream) override;
    virtual void set_selection_focus(GC::Ref<DOM::Node>, size_t offset, TextAffinity = TextAffinity::Downstream) override;
    virtual void move_cursor_to_start(CollapseSelection) override;
    virtual void move_cursor_to_end(CollapseSelection) override;
    void move_cursor_to_start_of_current_line(CollapseSelection);
    void move_cursor_to_end_of_current_line(CollapseSelection);
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

    void collapse_selection_to_offset(size_t, TextAffinity = TextAffinity::Downstream);
    void move_selection_end_to(size_t offset, TextAffinity, CollapseSelection);
    void scroll_cursor_into_view();
    void selection_was_changed(SelectionSource);

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-textarea/input-selection
    WebIDL::UnsignedLong m_selection_start { 0 };
    WebIDL::UnsignedLong m_selection_end { 0 };
    SelectionDirection m_selection_direction { SelectionDirection::None };

    // Disambiguates which visual line the selection end renders on when it sits at a soft wrap boundary.
    TextAffinity m_selection_end_affinity { TextAffinity::Downstream };

    // https://w3c.github.io/selection-api/#dfn-has-scheduled-selectionchange-event
    bool m_has_scheduled_selectionchange_event { false };
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::FormAssociatedTextControlElement>() const { return is_html_input_element() || is_html_textarea_element(); }

template<>
WEB_API HTML::FormAssociatedTextControlElement* Node::fast_as<HTML::FormAssociatedTextControlElement>();
template<>
WEB_API HTML::FormAssociatedTextControlElement const* Node::fast_as<HTML::FormAssociatedTextControlElement>() const;

}
