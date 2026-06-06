/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/HTML/HTMLFormControlsCollection.h>
#include <LibWeb/HTML/RadioNodeList.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLFormControlsCollection);

GC::Ref<HTMLFormControlsCollection> HTMLFormControlsCollection::create(DOM::ParentNode& root, Scope scope, Function<bool(DOM::Element const&)> filter)
{
    return GC::Heap::the().allocate<HTMLFormControlsCollection>(root, scope, move(filter));
}

HTMLFormControlsCollection::HTMLFormControlsCollection(DOM::ParentNode& root, Scope scope, Function<bool(DOM::Element const&)> filter)
    : DOM::HTMLCollection(root, scope, move(filter))
{
}

HTMLFormControlsCollection::~HTMLFormControlsCollection() = default;

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-htmlformcontrolscollection-nameditem
Variant<Empty, GC::Ref<DOM::Element>, GC::Ref<RadioNodeList>> HTMLFormControlsCollection::named_item_or_radio_node_list(FlyString const& name) const
{
    // 1. If name is the empty string, return null and stop the algorithm.
    if (name.is_empty())
        return {};

    // 2. If, at the time the method is called, there is exactly one node in the collection that has either an id attribute or a name attribute equal to name, then return that node and stop the algorithm.
    // 3. Otherwise, if there are no nodes in the collection that have either an id attribute or a name attribute equal to name, then return null and stop the algorithm.
    DOM::Element* matching_element = nullptr;
    bool multiple_matching = false;

    auto collection = collect_matching_elements();
    for (auto const& element : collection) {
        if (element->id() != name && element->name() != name)
            continue;

        if (matching_element) {
            multiple_matching = true;
            break;
        }

        matching_element = element;
    }

    if (!matching_element)
        return {};

    if (!multiple_matching)
        return GC::Ref { *matching_element };

    // 4. Otherwise, create a new RadioNodeList object representing a live view of the HTMLFormControlsCollection object, further filtered so that the only nodes in the
    //    RadioNodeList object are those that have either an id attribute or a name attribute equal to name. The nodes in the RadioNodeList object must be sorted in tree
    //    order. Return that RadioNodeList object.
    return RadioNodeList::create(root(), DOM::LiveNodeList::Scope::Descendants, [name](auto const& node) {
        if (!is<DOM::Element>(node))
            return false;

        auto const& element = as<DOM::Element>(node);
        return element.id() == name || element.name() == name;
    });
}

JS::Value HTMLFormControlsCollection::named_item_value(Bindings::WrapperWorld& wrapper_world, JS::Realm& realm, FlyString const& name) const
{
    return named_item_or_radio_node_list(name).visit(
        [](Empty) -> JS::Value { return JS::js_undefined(); },
        [&wrapper_world, &realm](GC::Ref<DOM::Element> const& value) -> JS::Value { return Bindings::wrap(wrapper_world, realm, value); },
        [&wrapper_world, &realm](GC::Ref<RadioNodeList> const& value) -> JS::Value { return Bindings::wrap(wrapper_world, realm, value); });
}

}
