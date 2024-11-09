/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLFieldSetElement final
    : public HTMLElement
    , public FormAssociatedElement {
    WEB_PLATFORM_OBJECT(HTMLFieldSetElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLFieldSetElement);
    FORM_ASSOCIATED_ELEMENT(HTMLElement, HTMLFieldSetElement)

public:
    virtual ~HTMLFieldSetElement() override;

    String const& type() const
    {
        static String const fieldset = "fieldset"_string;
        return fieldset;
    }

    bool is_disabled() const;

    JS::GCPtr<DOM::HTMLCollection> const& elements();

    // ^FormAssociatedElement
    // https://html.spec.whatwg.org/multipage/forms.html#category-listed
    virtual bool is_listed() const override { return true; }

    // https://html.spec.whatwg.org/multipage/forms.html#category-autocapitalize
    virtual bool is_auto_capitalize_inheriting() const override { return true; }

    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::group; }

private:
    HTMLFieldSetElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    JS::GCPtr<DOM::HTMLCollection> m_elements;
};

}
