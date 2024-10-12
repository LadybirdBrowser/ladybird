/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLOptionsCollectionPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/HTML/HTMLOptGroupElement.h>
#include <LibWeb/HTML/HTMLOptionElement.h>
#include <LibWeb/HTML/HTMLOptionsCollection.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/Namespace.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(HTMLOptionsCollection);

JS::NonnullGCPtr<HTMLOptionsCollection> HTMLOptionsCollection::create(DOM::ParentNode& root, Function<bool(DOM::Element const&)> filter)
{
    return root.heap().allocate<HTMLOptionsCollection>(root.realm(), root, move(filter));
}

HTMLOptionsCollection::HTMLOptionsCollection(DOM::ParentNode& root, Function<bool(DOM::Element const&)> filter)
    : DOM::HTMLCollection(root, Scope::Descendants, move(filter))
{
    m_legacy_platform_object_flags->has_indexed_property_setter = true;
    m_legacy_platform_object_flags->indexed_property_setter_has_identifier = true;
}

HTMLOptionsCollection::~HTMLOptionsCollection() = default;

void HTMLOptionsCollection::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLOptionsCollection);
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-htmloptionscollection-length
WebIDL::ExceptionOr<void> HTMLOptionsCollection::set_length(WebIDL::UnsignedLong value)
{
    // 1. Let current be the number of nodes represented by the collection.
    auto current = static_cast<WebIDL::UnsignedLong>(length());

    // 2. If the given value is greater than current, then:
    if (value > current) {
        // 2.1. If the given value is greater than 100,000, then return.
        if (value > 100'000)
            return {};

        // 2.2. Let n be value − current.
        auto n = value - current;

        // 2.3. Append n new option elements with no attributes and no child nodes to the select element on which this is rooted.
        // Mutation events must be fired as if a DocumentFragment containing the new option elements had been inserted.
        auto root_element = root();
        for (WebIDL::UnsignedLong i = 0; i < n; i++)
            TRY(root_element->append_child(TRY(DOM::create_element(root_element->document(), HTML::TagNames::option, Namespace::HTML))));
    }

    // 3. If the given value is less than current, then:
    if (value < current) {
        // 3.1. Let n be current − value.
        auto n = current - value;

        // 3.2. Remove the last n nodes in the collection from their parent nodes.
        for (auto i = 0u; i < n; i++)
            item(length() - 1)->remove();
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-htmloptionscollection-setter
WebIDL::ExceptionOr<void> HTMLOptionsCollection::set_value_of_indexed_property(u32 index, JS::Value unconverted_option)
{
    // The spec doesn't seem to require this, but it's consistent with length handling and other browsers
    if (index >= 100'000) {
        return {};
    }

    // 1. If value is null, invoke the steps for the remove method with index as the argument, and return.
    if (unconverted_option.is_null()) {
        remove(static_cast<WebIDL::Long>(index));
        return {};
    }

    if (!unconverted_option.is_object() || !is<HTMLOptionElement>(unconverted_option.as_object())) {
        return WebIDL::TypeMismatchError::create(realm(), "The value provided is not an HTMLOptionElement"_string);
    }

    auto& option = static_cast<HTMLOptionElement&>(unconverted_option.as_object());

    // 2. Let length be the number of nodes represented by the collection.
    auto length = this->length();

    auto root_element = root();

    if (index >= length) {
        // 3. Let n be index minus length.
        auto n = index - length;

        // 4. If n is greater than zero, then append a DocumentFragment consisting of n-1 new option elements with no attributes and no child nodes to the select element on which the HTMLOptionsCollection is rooted.
        if (n > 0) {
            for (WebIDL::UnsignedLong i = 0; i < n - 1; i++) {
                TRY(root_element->append_child(TRY(DOM::create_element(root_element->document(), HTML::TagNames::option, Namespace::HTML))));
            }
        }

        // 5. If n is greater than or equal to zero, append value to the select element.
        TRY(root_element->append_child(option));
    } else {
        // 5 (cont). Otherwise, replace the indexth element in the collection by value.
        TRY(root_element->replace_child(option, *item(index)));
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-htmloptionscollection-add
WebIDL::ExceptionOr<void> HTMLOptionsCollection::add(HTMLOptionOrOptGroupElement element, Optional<HTMLElementOrElementIndex> before)
{
    auto resolved_element = element.visit(
        [](auto& e) -> JS::Handle<HTMLElement> {
            return JS::make_handle(static_cast<HTML::HTMLElement&>(*e));
        });

    JS::GCPtr<DOM::Node> before_element;
    if (before.has_value() && before->has<JS::Handle<HTMLElement>>())
        before_element = before->get<JS::Handle<HTMLElement>>().ptr();

    // 1. If element is an ancestor of the select element on which the HTMLOptionsCollection is rooted, then throw a "HierarchyRequestError" DOMException.
    if (resolved_element->is_ancestor_of(root()))
        return WebIDL::HierarchyRequestError::create(realm(), "The provided element is an ancestor of the root select element."_string);

    // 2. If before is an element, but that element isn't a descendant of the select element on which the HTMLOptionsCollection is rooted, then throw a "NotFoundError" DOMException.
    if (before_element && !before_element->is_descendant_of(root()))
        return WebIDL::NotFoundError::create(realm(), "The 'before' element is not a descendant of the root select element."_string);

    // 3. If element and before are the same element, then return.
    if (before_element && (resolved_element.ptr() == before_element.ptr()))
        return {};

    // 4. If before is a node, then let reference be that node. Otherwise, if before is an integer, and there is a beforeth node in the collection, let reference be that node. Otherwise, let reference be null.
    JS::GCPtr<DOM::Node> reference;

    if (before_element)
        reference = move(before_element);
    else if (before.has_value() && before->has<i32>())
        reference = item(before->get<i32>());

    // 5. If reference is not null, let parent be the parent node of reference. Otherwise, let parent be the select element on which the HTMLOptionsCollection is rooted.
    DOM::Node* parent = reference ? reference->parent() : root().ptr();

    // 6. Pre-insert element into parent node before reference.
    (void)TRY(parent->pre_insert(*resolved_element, reference));

    return {};
}

// https://html.spec.whatwg.org/#dom-htmloptionscollection-remove
void HTMLOptionsCollection::remove(WebIDL::Long index)
{
    // 1. If the number of nodes represented by the collection is zero, return.
    if (length() == 0)
        return;

    // 2. If index is not a number greater than or equal to 0 and less than the number of nodes represented by the collection, return.
    if (index < 0 || static_cast<WebIDL::UnsignedLong>(index) >= length())
        return;

    // 3. Let element be the indexth element in the collection.
    auto* element = this->item(index);

    // 4. Remove element from its parent node.
    element->remove();
}

// https://html.spec.whatwg.org/#dom-htmloptionscollection-selectedindex
WebIDL::Long HTMLOptionsCollection::selected_index() const
{
    // The selectedIndex IDL attribute must act like the identically named attribute
    // on the select element on which the HTMLOptionsCollection is rooted.
    return verify_cast<HTMLSelectElement>(*root()).selected_index();
}

void HTMLOptionsCollection::set_selected_index(WebIDL::Long index)
{
    verify_cast<HTMLSelectElement>(*root()).set_selected_index(index);
}

}
