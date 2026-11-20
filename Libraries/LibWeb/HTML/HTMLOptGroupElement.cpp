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
