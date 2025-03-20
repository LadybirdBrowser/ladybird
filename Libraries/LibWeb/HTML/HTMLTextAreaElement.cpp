/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibWeb/Bindings/HTMLTextAreaElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLTextAreaElement);

HTMLTextAreaElement::HTMLTextAreaElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
    , m_input_event_timer(Core::Timer::create_single_shot(0, [weak_this = make_weak_ptr()]() {
        if (!weak_this)
            return;
        static_cast<HTMLTextAreaElement*>(weak_this.ptr())->queue_firing_input_event();
    }))
{
}

HTMLTextAreaElement::~HTMLTextAreaElement() = default;

void HTMLTextAreaElement::adjust_computed_style(CSS::ComputedProperties& style)
{
    // https://drafts.csswg.org/css-display-3/#unbox
    if (style.display().is_contents())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));

    // AD-HOC: We rewrite `display: inline` to `display: inline-block`.
    //         This is required for the internal shadow tree to work correctly in layout.
    if (style.display().is_inline_outside() && style.display().is_flow_inside())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::InlineBlock)));

    if (style.property(CSS::PropertyID::Width).has_auto())
        style.set_property(CSS::PropertyID::Width, CSS::LengthStyleValue::create(CSS::Length(cols(), CSS::Length::Type::Ch)));
    if (style.property(CSS::PropertyID::Height).has_auto())
        style.set_property(CSS::PropertyID::Height, CSS::LengthStyleValue::create(CSS::Length(rows(), CSS::Length::Type::Lh)));
}

void HTMLTextAreaElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLTextAreaElement);
}

void HTMLTextAreaElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_placeholder_element);
    visitor.visit(m_placeholder_text_node);
    visitor.visit(m_inner_text_element);
    visitor.visit(m_text_node);
}

void HTMLTextAreaElement::did_receive_focus()
{
    if (!m_text_node)
        return;
    m_text_node->invalidate_style(DOM::StyleInvalidationReason::DidReceiveFocus);

    if (m_placeholder_text_node)
        m_placeholder_text_node->invalidate_style(DOM::StyleInvalidationReason::DidReceiveFocus);
}

void HTMLTextAreaElement::did_lose_focus()
{
    if (m_text_node)
        m_text_node->invalidate_style(DOM::StyleInvalidationReason::DidLoseFocus);

    if (m_placeholder_text_node)
        m_placeholder_text_node->invalidate_style(DOM::StyleInvalidationReason::DidLoseFocus);

    // The change event fires when the value is committed, if that makes sense for the control,
    // or else when the control loses focus
    queue_an_element_task(HTML::Task::Source::UserInteraction, [this] {
        auto change_event = DOM::Event::create(realm(), HTML::EventNames::change);
        change_event->set_bubbles(true);
        dispatch_event(change_event);
    });
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-tabindex
i32 HTMLTextAreaElement::default_tab_index_value() const
{
    // See the base function for the spec comments.
    return 0;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-textarea-element:concept-form-reset-control
void HTMLTextAreaElement::reset_algorithm()
{
    // The reset algorithm for textarea elements is to set the user validity to false, dirty value flag back to false,
    m_user_validity = false;
    m_dirty_value = false;
    // and set the raw value of element to its child text content.
    set_raw_value(child_text_content());

    if (m_text_node) {
        m_text_node->set_text_content(m_raw_value);
        update_placeholder_visibility();
    }
}

// https://w3c.github.io/webdriver/#dfn-clear-algorithm
void HTMLTextAreaElement::clear_algorithm()
{
    // The clear algorithm for textarea elements is to set the dirty value flag back to false,
    m_dirty_value = false;

    // and set the raw value of element to an empty string.
    set_raw_value(child_text_content());

    // Unlike their associated reset algorithms, changes made to form controls as part of these algorithms do count as
    // changes caused by the user (and thus, e.g. do cause input events to fire).
    queue_firing_input_event();
}

// https://html.spec.whatwg.org/multipage/forms.html#the-textarea-element:concept-node-clone-ext
WebIDL::ExceptionOr<void> HTMLTextAreaElement::cloned(DOM::Node& copy, bool subtree) const
{
    TRY(Base::cloned(copy, subtree));

    // The cloning steps for textarea elements given node, copy, and subtree are to propagate the raw value and dirty value flag from node to copy.
    auto& textarea_copy = as<HTMLTextAreaElement>(copy);
    textarea_copy.m_raw_value = m_raw_value;
    textarea_copy.m_dirty_value = m_dirty_value;

    return {};
}

void HTMLTextAreaElement::form_associated_element_was_inserted()
{
    create_shadow_tree_if_needed();
}

void HTMLTextAreaElement::form_associated_element_was_removed(DOM::Node*)
{
    set_shadow_root(nullptr);
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-textarea-defaultvalue
String HTMLTextAreaElement::default_value() const
{
    // The defaultValue attribute's getter must return the element's child text content.
    return child_text_content();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-textarea-defaultvalue
void HTMLTextAreaElement::set_default_value(String const& default_value)
{
    // The defaultValue attribute's setter must string replace all with the given value within this element.
    string_replace_all(default_value);
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-textarea-value
String HTMLTextAreaElement::value() const
{
    // The value IDL attribute must, on getting, return the element's API value.
    return api_value();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-textarea-value
void HTMLTextAreaElement::set_value(String const& value)
{
    // 1. Let oldAPIValue be this element's API value.
    auto old_api_value = api_value();

    // 2. Set this element's raw value to the new value.
    set_raw_value(value);

    // 3. Set this element's dirty value flag to true.
    m_dirty_value = true;

    // 4. If the new API value is different from oldAPIValue, then move the text entry cursor position to the end of
    //    the text control, unselecting any selected text and resetting the selection direction to "none".
    if (api_value() != old_api_value) {
        if (m_text_node) {
            m_text_node->set_data(m_raw_value);
            update_placeholder_visibility();

            set_the_selection_range(m_text_node->length(), m_text_node->length());
        }
    }
}

void HTMLTextAreaElement::set_raw_value(String value)
{
    auto old_raw_value = move(m_raw_value);
    m_raw_value = move(value);
    m_api_value.clear();

    if (m_raw_value != old_raw_value)
        relevant_value_was_changed();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-textarea-element:concept-fe-api-value-3
String HTMLTextAreaElement::api_value() const
{
    // The algorithm for obtaining the element's API value is to return the element's raw value, with newlines normalized.
    if (!m_api_value.has_value())
        m_api_value = Infra::normalize_newlines(m_raw_value);
    return *m_api_value;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-textarea/input-relevant-value
WebIDL::ExceptionOr<void> HTMLTextAreaElement::set_relevant_value(String const& value)
{
    set_value(value);
    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-textarea-textlength
u32 HTMLTextAreaElement::text_length() const
{
    // The textLength IDL attribute must return the length of the element's API value.
    return AK::utf16_code_unit_length_from_utf8(api_value());
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-willvalidate
bool HTMLTextAreaElement::will_validate()
{
    // The willValidate attribute's getter must return true, if this element is a candidate for constraint validation
    return is_candidate_for_constraint_validation();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-checkvalidity
bool HTMLTextAreaElement::check_validity()
{
    return check_validity_steps();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-reportvalidity
bool HTMLTextAreaElement::report_validity()
{
    dbgln("(STUBBED) HTMLTextAreaElement::report_validity(). Called on: {}", debug_description());
    return true;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-textarea-maxlength
WebIDL::Long HTMLTextAreaElement::max_length() const
{
    // The maxLength IDL attribute must reflect the maxlength content attribute, limited to only non-negative numbers.
    if (auto maxlength_string = get_attribute(HTML::AttributeNames::maxlength); maxlength_string.has_value()) {
        if (auto maxlength = parse_non_negative_integer(*maxlength_string); maxlength.has_value() && *maxlength <= 2147483647)
            return *maxlength;
    }
    return -1;
}

WebIDL::ExceptionOr<void> HTMLTextAreaElement::set_max_length(WebIDL::Long value)
{
    // The maxLength IDL attribute must reflect the maxlength content attribute, limited to only non-negative numbers.
    return set_attribute(HTML::AttributeNames::maxlength, TRY(convert_non_negative_integer_to_string(realm(), value)));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-textarea-minlength
WebIDL::Long HTMLTextAreaElement::min_length() const
{
    // The minLength IDL attribute must reflect the minlength content attribute, limited to only non-negative numbers.
    if (auto minlength_string = get_attribute(HTML::AttributeNames::minlength); minlength_string.has_value()) {
        if (auto minlength = parse_non_negative_integer(*minlength_string); minlength.has_value() && *minlength <= 2147483647)
            return *minlength;
    }
    return -1;
}

WebIDL::ExceptionOr<void> HTMLTextAreaElement::set_min_length(WebIDL::Long value)
{
    // The minLength IDL attribute must reflect the minlength content attribute, limited to only non-negative numbers.
    return set_attribute(HTML::AttributeNames::minlength, TRY(convert_non_negative_integer_to_string(realm(), value)));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-textarea-cols
unsigned HTMLTextAreaElement::cols() const
{
    // The cols and rows attributes are limited to only positive numbers with fallback. The cols IDL attribute's default value is 20.
    if (auto cols_string = get_attribute(HTML::AttributeNames::cols); cols_string.has_value()) {
        if (auto cols = parse_non_negative_integer(*cols_string); cols.has_value() && *cols > 0 && *cols <= 2147483647)
            return *cols;
    }
    return 20;
}

WebIDL::ExceptionOr<void> HTMLTextAreaElement::set_cols(WebIDL::UnsignedLong cols)
{
    if (cols == 0 || cols > 2147483647)
        cols = 20;

    return set_attribute(HTML::AttributeNames::cols, String::number(cols));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-textarea-rows
WebIDL::UnsignedLong HTMLTextAreaElement::rows() const
{
    // The cols and rows attributes are limited to only positive numbers with fallback. The rows IDL attribute's default value is 2.
    if (auto rows_string = get_attribute(HTML::AttributeNames::rows); rows_string.has_value()) {
        if (auto rows = parse_non_negative_integer(*rows_string); rows.has_value() && *rows > 0 && *rows <= 2147483647)
            return *rows;
    }
    return 2;
}

WebIDL::ExceptionOr<void> HTMLTextAreaElement::set_rows(WebIDL::UnsignedLong rows)
{
    if (rows == 0 || rows > 2147483647)
        rows = 2;

    return set_attribute(HTML::AttributeNames::rows, String::number(rows));
}

WebIDL::UnsignedLong HTMLTextAreaElement::selection_start_binding() const
{
    return FormAssociatedTextControlElement::selection_start_binding().value();
}

WebIDL::ExceptionOr<void> HTMLTextAreaElement::set_selection_start_binding(WebIDL::UnsignedLong const& value)
{
    return FormAssociatedTextControlElement::set_selection_start_binding(value);
}

WebIDL::UnsignedLong HTMLTextAreaElement::selection_end_binding() const
{
    return FormAssociatedTextControlElement::selection_end_binding().value();
}

WebIDL::ExceptionOr<void> HTMLTextAreaElement::set_selection_end_binding(WebIDL::UnsignedLong const& value)
{
    return FormAssociatedTextControlElement::set_selection_end_binding(value);
}

String HTMLTextAreaElement::selection_direction_binding() const
{
    return selection_direction().value();
}

void HTMLTextAreaElement::set_selection_direction_binding(String const& direction)
{
    // NOTE: The selectionDirection setter never returns an error for textarea elements.
    MUST(static_cast<FormAssociatedTextControlElement&>(*this).set_selection_direction_binding(direction));
}

void HTMLTextAreaElement::create_shadow_tree_if_needed()
{
    if (shadow_root())
        return;

    auto shadow_root = realm().create<DOM::ShadowRoot>(document(), *this, Bindings::ShadowRootMode::Closed);
    set_shadow_root(shadow_root);

    auto element = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    MUST(shadow_root->append_child(element));

    m_placeholder_element = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    m_placeholder_element->set_use_pseudo_element(CSS::PseudoElement::Placeholder);
    MUST(element->append_child(*m_placeholder_element));

    m_placeholder_text_node = realm().create<DOM::Text>(document(), String {});
    m_placeholder_text_node->set_data(get_attribute_value(HTML::AttributeNames::placeholder));
    MUST(m_placeholder_element->append_child(*m_placeholder_text_node));

    m_inner_text_element = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    MUST(element->append_child(*m_inner_text_element));

    m_text_node = realm().create<DOM::Text>(document(), String {});
    handle_readonly_attribute(attribute(HTML::AttributeNames::readonly));
    // NOTE: If `children_changed()` was called before now, `m_raw_value` will hold the text content.
    //       Otherwise, it will get filled in whenever that does get called.
    m_text_node->set_text_content(m_raw_value);
    handle_maxlength_attribute();
    MUST(m_inner_text_element->append_child(*m_text_node));

    update_placeholder_visibility();
}

// https://html.spec.whatwg.org/multipage/input.html#attr-input-readonly
void HTMLTextAreaElement::handle_readonly_attribute(Optional<String> const& maybe_value)
{
    // The readonly attribute is a boolean attribute that controls whether or not the user can edit the form control. When specified, the element is not mutable.
    set_is_mutable(!maybe_value.has_value());
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-textarea-maxlength
void HTMLTextAreaElement::handle_maxlength_attribute()
{
    if (m_text_node) {
        auto max_length = this->max_length();
        if (max_length >= 0) {
            m_text_node->set_max_length(max_length);
        } else {
            m_text_node->set_max_length({});
        }
    }
}

void HTMLTextAreaElement::update_placeholder_visibility()
{
    if (!m_placeholder_element)
        return;
    if (!m_text_node)
        return;
    auto placeholder_text = get_attribute(AttributeNames::placeholder);
    if (placeholder_text.has_value() && m_text_node->data().is_empty()) {
        MUST(m_placeholder_element->style_for_bindings()->set_property(CSS::PropertyID::Display, "block"sv));
        MUST(m_inner_text_element->style_for_bindings()->set_property(CSS::PropertyID::Display, "none"sv));
    } else {
        MUST(m_placeholder_element->style_for_bindings()->set_property(CSS::PropertyID::Display, "none"sv));
        MUST(m_inner_text_element->style_for_bindings()->set_property(CSS::PropertyID::Display, "block"sv));
    }
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-textarea-element:children-changed-steps
void HTMLTextAreaElement::children_changed(ChildrenChangedMetadata const* metadata)
{
    Base::children_changed(metadata);

    // The children changed steps for textarea elements must, if the element's dirty value flag is false,
    // set the element's raw value to its child text content.
    if (!m_dirty_value) {
        set_raw_value(child_text_content());
        if (m_text_node)
            m_text_node->set_text_content(m_raw_value);
        update_placeholder_visibility();
    }
}

void HTMLTextAreaElement::form_associated_element_attribute_changed(FlyString const& name, Optional<String> const& value, Optional<FlyString> const&)
{
    if (name == HTML::AttributeNames::placeholder) {
        if (m_placeholder_text_node)
            m_placeholder_text_node->set_data(value.value_or(String {}));
    } else if (name == HTML::AttributeNames::readonly) {
        handle_readonly_attribute(value);
    } else if (name == HTML::AttributeNames::maxlength) {
        handle_maxlength_attribute();
    }
}

void HTMLTextAreaElement::did_edit_text_node()
{
    VERIFY(m_text_node);
    set_raw_value(m_text_node->data());

    // Any time the user causes the element's raw value to change, the user agent must queue an element task on the user
    // interaction task source given the textarea element to fire an event named input at the textarea element, with the
    // bubbles and composed attributes initialized to true. User agents may wait for a suitable break in the user's
    // interaction before queuing the task; for example, a user agent could wait for the user to have not hit a key for
    // 100ms, so as to only fire the event when the user pauses, instead of continuously for each keystroke.
    m_input_event_timer->restart(100);

    // A textarea element's dirty value flag must be set to true whenever the user interacts with the control in a way that changes the raw value.
    m_dirty_value = true;

    update_placeholder_visibility();
}

void HTMLTextAreaElement::queue_firing_input_event()
{
    queue_an_element_task(HTML::Task::Source::UserInteraction, [this]() {
        auto change_event = DOM::Event::create(realm(), HTML::EventNames::input, { .bubbles = true, .composed = true });
        dispatch_event(change_event);
    });
}

bool HTMLTextAreaElement::is_focusable() const
{
    return enabled();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-textarea-element%3Asuffering-from-being-missing
bool HTMLTextAreaElement::suffering_from_being_missing() const
{
    // If the element has its required attribute specified, and the element is mutable, and the element's value is the empty string, then the element is suffering from
    // being missing.
    return has_attribute(HTML::AttributeNames::required) && is_mutable() && value().is_empty();
}

}
