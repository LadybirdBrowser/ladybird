/*
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLFormControlsCollection.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/HTML/HTMLFormControlsCollection.h>
#include <LibWeb/HTML/RadioNodeList.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLFormControlsCollection);

static Utf16View radio_node_list_name_view(Utf16String const& name)
{
    return name.utf16_view();
}

static Utf16View radio_node_list_name_view(Utf16FlyString const& name)
{
    return name.view();
}

static bool radio_node_list_element_matches_name(DOM::Element const& element, Utf16View name)
{
    return (element.id().has_value() && element.id()->view() == name)
        || (element.name().has_value() && element.name()->view() == name);
}

GC::Ref<HTMLFormControlsCollection> HTMLFormControlsCollection::create(DOM::ParentNode& root, Scope scope, Function<bool(DOM::Element const&)> filter)
{
    return root.realm().create<HTMLFormControlsCollection>(root, scope, move(filter));
}

HTMLFormControlsCollection::HTMLFormControlsCollection(DOM::ParentNode& root, Scope scope, Function<bool(DOM::Element const&)> filter)
    : DOM::HTMLCollection(root, scope, move(filter))
{
}

HTMLFormControlsCollection::~HTMLFormControlsCollection() = default;

void HTMLFormControlsCollection::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLFormControlsCollection);
    Base::initialize(realm);
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-htmlformcontrolscollection-nameditem
Variant<Empty, GC::Ref<DOM::Element>, GC::Ref<RadioNodeList>> HTMLFormControlsCollection::named_item_or_radio_node_list(Utf16View name) const
{
    // 1. If name is the empty string, return null and stop the algorithm.
    if (name.is_empty())
        return {};

    // 2. If, at the time the method is called, there is exactly one node in the collection that has either an id attribute or a name attribute equal to name, then return that node and stop the algorithm.
    // 3. Otherwise, if there are no nodes in the collection that have either an id attribute or a name attribute equal to name, then return null and stop the algorithm.
    bool multiple_matching = false;
    auto* matching_element = first_matching_named_element(name, multiple_matching);

    if (!matching_element)
        return {};

    if (!multiple_matching)
        return GC::Ref { *matching_element };

    // 4. Otherwise, create a new RadioNodeList object representing a live view of the HTMLFormControlsCollection object, further filtered so that the only nodes in the
    //    RadioNodeList object are those that have either an id attribute or a name attribute equal to name. The nodes in the RadioNodeList object must be sorted in tree
    //    order. Return that RadioNodeList object.
    return create_radio_node_list(Utf16String::from_utf16(name));
}

DOM::Element* HTMLFormControlsCollection::first_matching_named_element(Utf16View name, bool& multiple_matching) const
{
    DOM::Element* matching_element = nullptr;
    multiple_matching = false;
    auto collection = collect_matching_elements();
    for (auto const& element : collection) {
        auto id_matches = element->id().has_value() && element->id()->view() == name;
        auto name_matches = element->name().has_value() && element->name()->view() == name;
        if (!id_matches && !name_matches)
            continue;

        if (matching_element) {
            multiple_matching = true;
            break;
        }

        matching_element = element;
    }

    return matching_element;
}

GC::Ref<RadioNodeList> HTMLFormControlsCollection::create_radio_node_list(Utf16String name) const
{
    return RadioNodeList::create(realm(), root(), DOM::LiveNodeList::Scope::Descendants, [name = move(name)](auto const& node) {
        if (!is<DOM::Element>(node))
            return false;

        auto const& element = as<DOM::Element>(node);
        return radio_node_list_element_matches_name(element, radio_node_list_name_view(name));
    });
}

GC::Ref<RadioNodeList> HTMLFormControlsCollection::create_radio_node_list(Utf16FlyString name) const
{
    return RadioNodeList::create(realm(), root(), DOM::LiveNodeList::Scope::Descendants, [name = move(name)](auto const& node) {
        if (!is<DOM::Element>(node))
            return false;

        auto const& element = as<DOM::Element>(node);
        return radio_node_list_element_matches_name(element, radio_node_list_name_view(name));
    });
}

JS::Value HTMLFormControlsCollection::named_item_value(Utf16FlyString const& name) const
{
    if (name.is_empty())
        return JS::js_undefined();

    bool multiple_matching = false;
    auto* matching_element = first_matching_named_element(name.view(), multiple_matching);
    if (!matching_element)
        return JS::js_undefined();
    if (!multiple_matching)
        return GC::Ref { *matching_element };

    return create_radio_node_list(name);
}

}
