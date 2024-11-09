/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLLegendElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/HTMLFieldSetElement.h>
#include <LibWeb/HTML/HTMLLegendElement.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLLegendElement);

HTMLLegendElement::HTMLLegendElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLLegendElement::~HTMLLegendElement() = default;

void HTMLLegendElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLLegendElement);
}

// https://html.spec.whatwg.org/multipage/form-elements.html#dom-legend-form
HTMLFormElement* HTMLLegendElement::form()
{
    // The form IDL attribute's behavior depends on whether the legend element is in a fieldset element or not.
    // If the legend has a fieldset element as its parent, then the form IDL attribute must return the same value as the form IDL attribute on that fieldset element.
    if (is<HTML::HTMLFieldSetElement>(parent_element())) {
        return verify_cast<HTML::HTMLFieldSetElement>(parent_element())->form();
    }

    // Otherwise, it must return null.
    return nullptr;
}

}
