/*
 * Copyright (c) 2025, Feng Yu <f3n67u@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLSelectedContentElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLSelectedContentElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLSelectedContentElement);

HTMLSelectedContentElement::HTMLSelectedContentElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLSelectedContentElement::~HTMLSelectedContentElement() = default;

void HTMLSelectedContentElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLSelectedContentElement);
    Base::initialize(realm);
}

// https://html.spec.whatwg.org/multipage/form-elements.html#clear-a-selectedcontent
void HTMLSelectedContentElement::clear_selectedcontent()
{
    // To clear a selectedcontent given a selectedcontent element selectedcontent:

    // 1. Replace all with null within selectedcontent.
    replace_all(nullptr);
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-selectedcontent-element:html-element-post-connection-steps
void HTMLSelectedContentElement::post_connection()
{
    // The selectedcontent HTML element post-connection steps, given selectedcontent, are:

    // 1. Let nearestSelectAncestor be null.
    GC::Ptr<HTMLSelectElement> nearest_select_ancestor;

    // 2. Let ancestor be selectedcontent's parent.
    // NB: This step is not necessary; the variable is never referenced.
    //     See https://github.com/whatwg/html/issues/11963.

    // 3. Set selectedcontent's disabled state to false.
    set_disabled(false);

    // 4. For each ancestor of selectedcontent's ancestors, in reverse tree order:
    for_each_ancestor([&](auto& ancestor) {
        //  1. If ancestor is a select element:
        if (auto* select_element = as_if<HTMLSelectElement>(ancestor)) {
            // 1. If nearestSelectAncestor is null, then set nearestSelectAncestor to select.
            if (!nearest_select_ancestor)
                nearest_select_ancestor = select_element;
            // 2. Otherwise, set selectedcontent's disabled state to true.
            else
                set_disabled(true);
        }

        // 2. If ancestor is an option element or a selectedcontent element,
        //    then set selectedcontent's disabled state to true.
        if (is<HTMLOptionElement>(ancestor) || is<HTMLSelectedContentElement>(ancestor))
            set_disabled(true);

        return IterationDecision::Continue;
    });

    // 5. If nearestSelectAncestor is null or nearestSelectAncestor has the multiple attribute, then return.
    if (!nearest_select_ancestor || nearest_select_ancestor->has_attribute(AttributeNames::multiple))
        return;

    // 6. Run update a select's selectedcontent given nearestSelectAncestor.
    MUST(nearest_select_ancestor->update_selectedcontent());

    // 7. Run clear a select's non-primary selectedcontent elements given nearestSelectAncestor.
    nearest_select_ancestor->clear_non_primary_selectedcontent();
}

// https://html.spec.whatwg.org/multipage/form-elements.html#the-selectedcontent-element:html-element-removing-steps
void HTMLSelectedContentElement::removed_from(DOM::Node* old_parent, DOM::Node& old_root)
{
    // The selectedcontent HTML element removing steps, given selectedcontent and oldParent, are:
    Base::removed_from(old_parent, old_root);

    // 1. For each ancestor of selectedcontent's ancestors, in reverse tree order:
    for (auto* ancestor = parent(); ancestor; ancestor = ancestor->parent()) {
        // 1. If ancestor is a select element, then return.
        if (is<HTMLSelectElement>(*ancestor)) {
            return;
        }
    }

    // 2. For each ancestor of oldParent's inclusive ancestors, in reverse tree order:
    for (auto* ancestor = old_parent; ancestor; ancestor = ancestor->parent()) {
        // 1. If ancestor is a select element, then run update a select's selectedcontent given ancestor and return.
        if (auto* select_element = as_if<HTMLSelectElement>(*ancestor)) {
            MUST(select_element->update_selectedcontent());
            return;
        }
    }
}

}
