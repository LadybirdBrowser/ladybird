/*
 * Copyright (c) 2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/WeakPtr.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// Form-associated elements should invoke this macro to inject overridden FormAssociatedElement and HTMLElement
// methods as needed. If your class wished to override an HTMLElement method that is overridden here, use the
// following methods instead:
//
//    HTMLElement::inserted() -> Use form_associated_element_was_inserted()
//    HTMLElement::removed_from() -> Use form_associated_element_was_removed()
//
#define FORM_ASSOCIATED_ELEMENT(ElementBaseClass, ElementClass)                                                                      \
private:                                                                                                                             \
    virtual HTMLElement& form_associated_element_to_html_element() override                                                          \
    {                                                                                                                                \
        static_assert(IsBaseOf<HTMLElement, ElementClass>);                                                                          \
        return *this;                                                                                                                \
    }                                                                                                                                \
                                                                                                                                     \
    virtual void inserted() override                                                                                                 \
    {                                                                                                                                \
        ElementBaseClass::inserted();                                                                                                \
        form_node_was_inserted();                                                                                                    \
        form_associated_element_was_inserted();                                                                                      \
    }                                                                                                                                \
                                                                                                                                     \
    virtual void removed_from(DOM::Node* node) override                                                                              \
    {                                                                                                                                \
        ElementBaseClass::removed_from(node);                                                                                        \
        form_node_was_removed();                                                                                                     \
        form_associated_element_was_removed(node);                                                                                   \
    }                                                                                                                                \
                                                                                                                                     \
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value) override \
    {                                                                                                                                \
        ElementBaseClass::attribute_changed(name, old_value, value);                                                                 \
        form_node_attribute_changed(name, value);                                                                                    \
        form_associated_element_attribute_changed(name, value);                                                                      \
    }

class FormAssociatedElement {
public:
    HTMLFormElement* form() { return m_form; }
    HTMLFormElement const* form() const { return m_form; }

    void set_form(HTMLFormElement*);

    void element_id_changed(Badge<DOM::Document>);
    void element_with_id_was_added_or_removed(Badge<DOM::Document>);

    bool enabled() const;

    void set_parser_inserted(Badge<HTMLParser>);

    // https://html.spec.whatwg.org/multipage/forms.html#category-listed
    virtual bool is_listed() const { return false; }

    // https://html.spec.whatwg.org/multipage/forms.html#category-submit
    virtual bool is_submittable() const { return false; }

    // https://html.spec.whatwg.org/multipage/forms.html#category-reset
    virtual bool is_resettable() const { return false; }

    // https://html.spec.whatwg.org/multipage/forms.html#category-autocapitalize
    virtual bool is_auto_capitalize_inheriting() const { return false; }

    // https://html.spec.whatwg.org/multipage/forms.html#concept-button
    virtual bool is_button() const { return false; }

    // https://html.spec.whatwg.org/multipage/forms.html#concept-submit-button
    virtual bool is_submit_button() const { return false; }

    virtual String value() const { return String {}; }

    virtual HTMLElement& form_associated_element_to_html_element() = 0;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#concept-form-reset-control
    virtual void reset_algorithm() {};

protected:
    FormAssociatedElement() = default;
    virtual ~FormAssociatedElement() = default;

    virtual void form_associated_element_was_inserted() { }
    virtual void form_associated_element_was_removed(DOM::Node*) { }
    virtual void form_associated_element_attribute_changed(FlyString const&, Optional<String> const&) { }

    void form_node_was_inserted();
    void form_node_was_removed();
    void form_node_attribute_changed(FlyString const&, Optional<String> const&);

private:
    WeakPtr<HTMLFormElement> m_form;

    // https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#parser-inserted-flag
    bool m_parser_inserted { false };

    void reset_form_owner();
};

}
