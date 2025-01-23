/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLOptGroupElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/HTMLOptGroupElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLOptGroupElement);

HTMLOptGroupElement::HTMLOptGroupElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLOptGroupElement::~HTMLOptGroupElement() = default;

void HTMLOptGroupElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLOptGroupElement);
}

void HTMLOptGroupElement::inserted()
{
    Base::inserted();

    // AD-HOC: We update the selectedness of our <select> parent here,
    //         to ensure that the correct <option> is selected after an <optgroup> is dynamically inserted.
    if (is<HTMLSelectElement>(*parent()) && first_child_of_type<HTMLOptionElement>())
        static_cast<HTMLSelectElement&>(*parent()).update_selectedness();
}

void HTMLOptGroupElement::removed_from(Node* old_parent, Node& old_root)
{
    Base::removed_from(old_parent, old_root);

    // The optgroup HTML element removing steps, given removedNode and oldParent, are:
    // 1. If oldParent is a select element and removedNode has an option child, then run oldParent's selectedness setting algorithm.
    if (old_parent && is<HTMLSelectElement>(*old_parent) && first_child_of_type<HTMLOptionElement>())
        static_cast<HTMLSelectElement&>(*old_parent).update_selectedness();
}

}
