/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/HTMLOptGroupElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLOptGroupElement);

HTMLOptGroupElement::HTMLOptGroupElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLOptGroupElement::~HTMLOptGroupElement() = default;

void HTMLOptGroupElement::inserted()
{
    Base::inserted();

    // AD-HOC: We update the selectedness of our <select> parent here,
    //         to ensure that the correct <option> is selected after an <optgroup> is dynamically inserted.
    if (is<HTMLSelectElement>(*parent()) && first_child_of_type<HTMLOptionElement>())
        static_cast<HTMLSelectElement&>(*parent()).update_selectedness();
}

void HTMLOptGroupElement::removed_from(IsSubtreeRoot is_subtree_root, Node* old_ancestor, Node& old_root)
{
    Base::removed_from(is_subtree_root, old_ancestor, old_root);

    // The optgroup HTML element removing steps, given removedNode and oldParent, are:
    // 1. If oldParent is a select element and removedNode has an option child, then run oldParent's selectedness setting algorithm.
    if (old_ancestor && is<HTMLSelectElement>(*old_ancestor) && first_child_of_type<HTMLOptionElement>())
        static_cast<HTMLSelectElement&>(*old_ancestor).update_selectedness();
}

void HTMLOptGroupElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // NB: The rows of a list box show the label of the group, leave out a hidden group, and gray out the options of a
    //     disabled one.
    if (namespace_.has_value() || old_value == value)
        return;
    if (name == AttributeNames::label || name == AttributeNames::hidden || name == AttributeNames::disabled) {
        if (auto* select = as_if<HTMLSelectElement>(parent()))
            select->list_box_options_did_change();
    }
}

}
