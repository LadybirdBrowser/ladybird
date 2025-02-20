/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/PopoverInvokerElement.h>

namespace Web::HTML {

#define ENUMERATE_HTML_BUTTON_TYPE_ATTRIBUTES              \
    __ENUMERATE_HTML_BUTTON_TYPE_ATTRIBUTE(submit, Submit) \
    __ENUMERATE_HTML_BUTTON_TYPE_ATTRIBUTE(reset, Reset)   \
    __ENUMERATE_HTML_BUTTON_TYPE_ATTRIBUTE(button, Button) \
    __ENUMERATE_HTML_BUTTON_TYPE_ATTRIBUTE(auto, Auto)

class HTMLButtonElement final
    : public HTMLElement
    , public FormAssociatedElement
    , public PopoverInvokerElement {
    WEB_PLATFORM_OBJECT(HTMLButtonElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLButtonElement);
    FORM_ASSOCIATED_ELEMENT(HTMLElement, HTMLButtonElement)

public:
    virtual ~HTMLButtonElement() override;

    virtual void initialize(JS::Realm&) override;

    enum class TypeAttributeState {
#define __ENUMERATE_HTML_BUTTON_TYPE_ATTRIBUTE(_, state) state,
        ENUMERATE_HTML_BUTTON_TYPE_ATTRIBUTES
#undef __ENUMERATE_HTML_BUTTON_TYPE_ATTRIBUTE
    };

    TypeAttributeState type_state() const;
    String type_for_bindings() const;
    WebIDL::ExceptionOr<void> set_type_for_bindings(String const&);

    virtual void form_associated_element_attribute_changed(FlyString const& name, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    // ^EventTarget
    // https://html.spec.whatwg.org/multipage/interaction.html#the-tabindex-attribute:the-button-element
    // https://html.spec.whatwg.org/multipage/interaction.html#focusable-area
    // https://html.spec.whatwg.org/multipage/semantics-other.html#concept-element-disabled
    virtual bool is_focusable() const override;

    // ^FormAssociatedElement
    // https://html.spec.whatwg.org/multipage/forms.html#category-listed
    virtual bool is_listed() const override { return true; }

    // https://html.spec.whatwg.org/multipage/forms.html#category-submit
    virtual bool is_submittable() const override { return true; }

    // https://html.spec.whatwg.org/multipage/forms.html#category-autocapitalize
    virtual bool is_auto_capitalize_inheriting() const override { return true; }

    // https://html.spec.whatwg.org/multipage/forms.html#concept-button
    // https://html.spec.whatwg.org/multipage/form-elements.html#the-button-element:concept-button
    virtual bool is_button() const override { return true; }

    virtual bool is_submit_button() const override;

    // ^HTMLElement
    // https://html.spec.whatwg.org/multipage/forms.html#category-label
    virtual bool is_labelable() const override { return true; }

    // https://www.w3.org/TR/html-aria/#el-button
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::button; }

    virtual String value() const override;

    virtual bool has_activation_behavior() const override;
    virtual void activation_behavior(DOM::Event const&) override;

private:
    virtual void visit_edges(Visitor&) override;

    virtual bool is_html_button_element() const override { return true; }

    HTMLButtonElement(DOM::Document&, DOM::QualifiedName);

    // ^DOM::Element
    virtual i32 default_tab_index_value() const override;
};

}

namespace Web::DOM {
template<>
inline bool Node::fast_is<HTML::HTMLButtonElement>() const { return is_html_button_element(); }
}
