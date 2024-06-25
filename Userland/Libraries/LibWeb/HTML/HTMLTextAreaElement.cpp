/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLTextAreaElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/StyleProperties.h>
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

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLTextAreaElement);

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

void HTMLTextAreaElement::adjust_computed_style(CSS::StyleProperties& style)
{
    // AD-HOC: We rewrite `display: inline` to `display: inline-block`.
    //         This is required for the internal shadow tree to work correctly in layout.
    if (style.display().is_inline_outside() && style.display().is_flow_inside())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::InlineBlock)));

    if (style.property(CSS::PropertyID::Width)->has_auto())
        style.set_property(CSS::PropertyID::Width, CSS::LengthStyleValue::create(CSS::Length(cols(), CSS::Length::Type::Ch)));
    if (style.property(CSS::PropertyID::Height)->has_auto())
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
    auto navigable = document().navigable();
    if (!navigable)
        return;
    if (!m_text_node)
        return;
    navigable->set_cursor_position(DOM::Position::create(realm(), *m_text_node, 0));
}

void HTMLTextAreaElement::did_lose_focus()
{
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
    // The reset algorithm for textarea elements is to set the dirty value flag back to false,
    m_dirty_value = false;
    // and set the raw value of element to its child text content.
    set_raw_value(child_text_content());

    if (m_text_node) {
        m_text_node->set_text_content(m_raw_value);
        update_placeholder_visibility();
    }
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
    auto& realm = this->realm();

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

            if (auto navigable = document().navigable())
                navigable->set_cursor_position(DOM::Position::create(realm, *m_text_node, m_text_node->data().bytes().size()));
        }
    }
}

void HTMLTextAreaElement::set_raw_value(String value)
{
    m_raw_value = move(value);
    m_api_value.clear();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-textarea-element:concept-fe-api-value-3
String HTMLTextAreaElement::api_value() const
{
    // The algorithm for obtaining the element's API value is to return the element's raw value, with newlines normalized.
    if (!m_api_value.has_value())
        m_api_value = Infra::normalize_newlines(m_raw_value);
    return *m_api_value;
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-textarea-textlength
u32 HTMLTextAreaElement::text_length() const
{
    // The textLength IDL attribute must return the length of the element's API value.
    // FIXME: This is inefficient!
    auto utf16_data = MUST(AK::utf8_to_utf16(api_value()));
    return Utf16View { utf16_data }.length_in_code_units();
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-checkvalidity
bool HTMLTextAreaElement::check_validity()
{
    dbgln("(STUBBED) HTMLTextAreaElement::check_validity(). Called on: {}", debug_description());
    return true;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-reportvalidity
bool HTMLTextAreaElement::report_validity()
{
    dbgln("(STUBBED) HTMLTextAreaElement::report_validity(). Called on: {}", debug_description());
    return true;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-cva-setcustomvalidity
void HTMLTextAreaElement::set_custom_validity(String const& error)
{
    dbgln("(STUBBED) HTMLTextAreaElement::set_custom_validity(\"{}\"). Called on: {}", error, debug_description());
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-selectionstart
WebIDL::UnsignedLong HTMLTextAreaElement::selection_start() const
{
    // 1. If this element is an input element, and selectionStart does not apply to this element, return null.

    // 2. If there is no selection, return the code unit offset within the relevant value to the character that
    //    immediately follows the text entry cursor.
    if (auto navigable = document().navigable()) {
        if (auto cursor = navigable->cursor_position())
            return cursor->offset();
    }

    // FIXME: 3. Return the code unit offset within the relevant value to the character that immediately follows the start of
    //           the selection.
    return 0;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#textFieldSelection:dom-textarea/input-selectionstart-2
WebIDL::ExceptionOr<void> HTMLTextAreaElement::set_selection_start(WebIDL::UnsignedLong)
{
    // 1. If this element is an input element, and selectionStart does not apply to this element, throw an
    //    "InvalidStateError" DOMException.

    // FIXME: 2. Let end be the value of this element's selectionEnd attribute.
    // FIXME: 3. If end is less than the given value, set end to the given value.
    // FIXME: 4. Set the selection range with the given value, end, and the value of this element's selectionDirection attribute.
    return {};
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#dom-textarea/input-selectionend
WebIDL::UnsignedLong HTMLTextAreaElement::selection_end() const
{
    // 1. If this element is an input element, and selectionEnd does not apply to this element, return null.

    // 2. If there is no selection, return the code unit offset within the relevant value to the character that
    //    immediately follows the text entry cursor.
    if (auto navigable = document().navigable()) {
        if (auto cursor = navigable->cursor_position())
            return cursor->offset();
    }

    // FIXME: 3. Return the code unit offset within the relevant value to the character that immediately follows the end of
    //           the selection.
    return 0;
}

// https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#textFieldSelection:dom-textarea/input-selectionend-3
WebIDL::ExceptionOr<void> HTMLTextAreaElement::set_selection_end(WebIDL::UnsignedLong)
{
    // 1. If this element is an input element, and selectionEnd does not apply to this element, throw an
    //    "InvalidStateError" DOMException.

    // FIXME: 2. Set the selection range with the value of this element's selectionStart attribute, the given value, and the
    //           value of this element's selectionDirection attribute.
    return {};
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-textarea-maxlength
WebIDL::Long HTMLTextAreaElement::max_length() const
{
    // The maxLength IDL attribute must reflect the maxlength content attribute, limited to only non-negative numbers.
    if (auto maxlength_string = get_attribute(HTML::AttributeNames::maxlength); maxlength_string.has_value()) {
        if (auto maxlength = parse_non_negative_integer(*maxlength_string); maxlength.has_value())
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
        if (auto minlength = parse_non_negative_integer(*minlength_string); minlength.has_value())
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
        if (auto cols = parse_non_negative_integer(*cols_string); cols.has_value())
            return *cols;
    }
    return 20;
}

WebIDL::ExceptionOr<void> HTMLTextAreaElement::set_cols(unsigned cols)
{
    return set_attribute(HTML::AttributeNames::cols, MUST(String::number(cols)));
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-textarea-rows
unsigned HTMLTextAreaElement::rows() const
{
    // The cols and rows attributes are limited to only positive numbers with fallback. The rows IDL attribute's default value is 2.
    if (auto rows_string = get_attribute(HTML::AttributeNames::rows); rows_string.has_value()) {
        if (auto rows = parse_non_negative_integer(*rows_string); rows.has_value())
            return *rows;
    }
    return 2;
}

WebIDL::ExceptionOr<void> HTMLTextAreaElement::set_rows(unsigned rows)
{
    return set_attribute(HTML::AttributeNames::rows, MUST(String::number(rows)));
}

void HTMLTextAreaElement::create_shadow_tree_if_needed()
{
    if (shadow_root())
        return;

    auto shadow_root = heap().allocate<DOM::ShadowRoot>(realm(), document(), *this, Bindings::ShadowRootMode::Closed);
    set_shadow_root(shadow_root);

    auto element = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    MUST(shadow_root->append_child(element));

    m_placeholder_element = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    m_placeholder_element->set_use_pseudo_element(CSS::Selector::PseudoElement::Type::Placeholder);
    MUST(element->append_child(*m_placeholder_element));

    m_placeholder_text_node = heap().allocate<DOM::Text>(realm(), document(), String {});
    m_placeholder_text_node->set_data(get_attribute_value(HTML::AttributeNames::placeholder));
    m_placeholder_text_node->set_editable_text_node_owner(Badge<HTMLTextAreaElement> {}, *this);
    MUST(m_placeholder_element->append_child(*m_placeholder_text_node));

    m_inner_text_element = MUST(DOM::create_element(document(), HTML::TagNames::div, Namespace::HTML));
    MUST(element->append_child(*m_inner_text_element));

    m_text_node = heap().allocate<DOM::Text>(realm(), document(), String {});
    handle_readonly_attribute(attribute(HTML::AttributeNames::readonly));
    m_text_node->set_editable_text_node_owner(Badge<HTMLTextAreaElement> {}, *this);
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
    m_is_mutable = !maybe_value.has_value();

    if (m_text_node)
        m_text_node->set_always_editable(m_is_mutable);
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
void HTMLTextAreaElement::children_changed()
{
    // The children changed steps for textarea elements must, if the element's dirty value flag is false,
    // set the element's raw value to its child text content.
    if (!m_dirty_value) {
        set_raw_value(child_text_content());
        if (m_text_node)
            m_text_node->set_text_content(m_raw_value);
        update_placeholder_visibility();
    }
}

void HTMLTextAreaElement::form_associated_element_attribute_changed(FlyString const& name, Optional<String> const& value)
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

void HTMLTextAreaElement::did_edit_text_node(Badge<Navigable>)
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

}
