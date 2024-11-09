/*
 * Copyright (c) 2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/HTMLDetailsElement.h>
#include <LibWeb/HTML/HTMLSummaryElement.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLSummaryElement);

HTMLSummaryElement::HTMLSummaryElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

bool HTMLSummaryElement::has_activation_behavior() const
{
    return true;
}

void HTMLSummaryElement::activation_behavior(DOM::Event const&)
{
    // The activation behavior of summary elements is to run the following steps:

    // 1. If this summary element is not the summary for its parent details, then return.
    if (!is_summary_for_its_parent_details())
        return;

    // 2. Let parent be this summary element's parent.
    auto* parent = this->parent_element();

    // 3. If the open attribute is present on parent, then remove it. Otherwise, set parent's open attribute to the empty string.
    if (parent->has_attribute(HTML::AttributeNames::open))
        parent->remove_attribute(HTML::AttributeNames::open);
    else
        parent->set_attribute(HTML::AttributeNames::open, String {}).release_value_but_fixme_should_propagate_errors();
}

// https://html.spec.whatwg.org/multipage/interactive-elements.html#summary-for-its-parent-details
bool HTMLSummaryElement::is_summary_for_its_parent_details()
{
    // A summary element is a summary for its parent details if the following algorithm returns true:

    // 1. If this summary element has no parent, then return false.
    if (!parent_element())
        return false;

    // 2. Let parent be this summary element's parent.
    auto* parent = this->parent_element();

    // 3. If parent is not a details element, then return false.
    if (!is<HTMLDetailsElement>(*parent))
        return false;

    // 4. If parent's first summary element child is not this summary element, then return false.
    if (parent->first_child_of_type<HTMLSummaryElement>() != this)
        return false;

    // 5. Return true.
    return true;
}

HTMLSummaryElement::~HTMLSummaryElement() = default;

void HTMLSummaryElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
}

}
