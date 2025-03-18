/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/PropertyKey.h>
#include <LibWeb/Bindings/HTMLAllCollectionPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/HTMLAllCollection.h>
#include <LibWeb/HTML/HTMLAnchorElement.h>
#include <LibWeb/HTML/HTMLButtonElement.h>
#include <LibWeb/HTML/HTMLEmbedElement.h>
#include <LibWeb/HTML/HTMLFormElement.h>
#include <LibWeb/HTML/HTMLFrameElement.h>
#include <LibWeb/HTML/HTMLFrameSetElement.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLMapElement.h>
#include <LibWeb/HTML/HTMLMetaElement.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/Namespace.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLAllCollection);

GC::Ref<HTMLAllCollection> HTMLAllCollection::create(DOM::ParentNode& root, Scope scope, Function<bool(DOM::Element const&)> filter)
{
    return root.realm().create<HTMLAllCollection>(root, scope, move(filter));
}

HTMLAllCollection::HTMLAllCollection(DOM::ParentNode& root, Scope scope, Function<bool(DOM::Element const&)> filter)
    : PlatformObject(root.realm())
    , m_root(root)
    , m_filter(move(filter))
    , m_scope(scope)
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags {
        .supports_indexed_properties = true,
        .supports_named_properties = true,
        .has_legacy_unenumerable_named_properties_interface_extended_attribute = true,
    };
}

HTMLAllCollection::~HTMLAllCollection() = default;

void HTMLAllCollection::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLAllCollection);
}

void HTMLAllCollection::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_root);
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#all-named-elements
static bool is_all_named_element(DOM::Element const& element)
{
    // The following elements are "all"-named elements: a, button, embed, form, frame, frameset, iframe, img, input, map, meta, object, select, and textarea
    return is<HTML::HTMLAnchorElement>(element)
        || is<HTML::HTMLButtonElement>(element)
        || is<HTML::HTMLEmbedElement>(element)
        || is<HTML::HTMLFormElement>(element)
        || is<HTML::HTMLFrameElement>(element)
        || is<HTML::HTMLFrameSetElement>(element)
        || is<HTML::HTMLIFrameElement>(element)
        || is<HTML::HTMLImageElement>(element)
        || is<HTML::HTMLInputElement>(element)
        || is<HTML::HTMLMapElement>(element)
        || is<HTML::HTMLMetaElement>(element)
        || is<HTML::HTMLObjectElement>(element)
        || is<HTML::HTMLSelectElement>(element)
        || is<HTML::HTMLTextAreaElement>(element);
}

GC::RootVector<GC::Ref<DOM::Element>> HTMLAllCollection::collect_matching_elements() const
{
    GC::RootVector<GC::Ref<DOM::Element>> elements(m_root->heap());
    if (m_scope == Scope::Descendants) {
        m_root->for_each_in_subtree_of_type<DOM::Element>([&](auto& element) {
            if (m_filter(element))
                elements.append(element);
            return TraversalDecision::Continue;
        });
    } else {
        m_root->for_each_child_of_type<DOM::Element>([&](auto& element) {
            if (m_filter(element))
                elements.append(element);
            return IterationDecision::Continue;
        });
    }
    return elements;
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-htmlallcollection-length
size_t HTMLAllCollection::length() const
{
    // The length getter steps are to return the number of nodes represented by the collection.
    return collect_matching_elements().size();
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-htmlallcollection-item
Variant<GC::Ref<DOM::HTMLCollection>, GC::Ref<DOM::Element>, Empty> HTMLAllCollection::item(Optional<FlyString> const& name_or_index) const
{
    // 1. If nameOrIndex was not provided, return null.
    if (!name_or_index.has_value())
        return Empty {};

    // 2. Return the result of getting the "all"-indexed or named element(s) from this, given nameOrIndex.
    return get_the_all_indexed_or_named_elements(name_or_index.value());
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-htmlallcollection-nameditem
Variant<GC::Ref<DOM::HTMLCollection>, GC::Ref<DOM::Element>, Empty> HTMLAllCollection::named_item(FlyString const& name) const
{
    // The namedItem(name) method steps are to return the result of getting the "all"-named element(s) from this given name.
    return get_the_all_named_elements(name);
}

// https://dom.spec.whatwg.org/#ref-for-dfn-supported-property-names
Vector<FlyString> HTMLAllCollection::supported_property_names() const
{
    // The supported property names consist of the non-empty values of all the id attributes of all the
    // elements represented by the collection, and the non-empty values of all the name attributes of
    // all the "all"-named elements represented by the collection, in tree order, ignoring later duplicates,
    // with the id of an element preceding its name if it contributes both, they differ from each other, and
    // neither is the duplicate of an earlier entry.

    Vector<FlyString> result;
    auto elements = collect_matching_elements();

    for (auto const& element : elements) {
        if (auto const& id = element->id(); id.has_value() && !id->is_empty()) {
            if (!result.contains_slow(id.value()))
                result.append(id.value());
        }

        if (is_all_named_element(*element) && element->name().has_value() && !element->name()->is_empty()) {
            auto name = element->name().value();
            if (!name.is_empty() && !result.contains_slow(name))
                result.append(move(name));
        }
    }

    // 3. Return result.
    return result;
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#concept-get-all-named
Variant<GC::Ref<DOM::HTMLCollection>, GC::Ref<DOM::Element>, Empty> HTMLAllCollection::get_the_all_named_elements(FlyString const& name) const
{
    // 1. If name is the empty string, return null.
    if (name.is_empty())
        return Empty {};

    // 2. Let subCollection be an HTMLCollection object rooted at the same Document as collection, whose filter matches only elements that are either:
    auto sub_collection = DOM::HTMLCollection::create(m_root, DOM::HTMLCollection::Scope::Descendants, [name](DOM::Element const& element) {
        // * "all"-named elements with a name attribute equal to name, or,
        if (is_all_named_element(element) && element.name() == name)
            return true;

        // * elements with an ID equal to name.
        return element.id() == name;
    });

    // 3. If there is exactly one element in subCollection, then return that element.
    auto matching_elements = sub_collection->collect_matching_elements();
    if (matching_elements.size() == 1)
        return matching_elements.first();

    // 4. Otherwise, if subCollection is empty, return null.
    if (matching_elements.is_empty())
        return Empty {};

    // 5. Otherwise, return subCollection.
    return sub_collection;
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#concept-get-all-indexed
GC::Ptr<DOM::Element> HTMLAllCollection::get_the_all_indexed_element(u32 index) const
{
    // To get the "all"-indexed element from an HTMLAllCollection collection given an index index, return the indexth
    // element in collection, or null if there is no such indexth element.
    auto elements = collect_matching_elements();
    if (index >= elements.size())
        return nullptr;
    return elements[index];
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#concept-get-all-indexed-or-named
Variant<GC::Ref<DOM::HTMLCollection>, GC::Ref<DOM::Element>, Empty> HTMLAllCollection::get_the_all_indexed_or_named_elements(JS::PropertyKey const& name_or_index) const
{
    // 1. If nameOrIndex, converted to a JavaScript String value, is an array index property name, return the result of getting the "all"-indexed element from
    //    collection given the number represented by nameOrIndex.
    if (name_or_index.is_number()) {
        auto maybe_element = get_the_all_indexed_element(name_or_index.as_number());
        if (!maybe_element)
            return Empty {};
        return GC::Ref<DOM::Element> { *maybe_element };
    }

    // 2. Return the result of getting the "all"-named element(s) from collection given nameOrIndex.
    return get_the_all_named_elements(name_or_index.as_string());
}

Optional<JS::Value> HTMLAllCollection::item_value(size_t index) const
{
    if (auto value = get_the_all_indexed_element(index))
        return value;
    return {};
}

JS::Value HTMLAllCollection::named_item_value(FlyString const& name) const
{
    return named_item(name).visit(
        [](Empty) -> JS::Value { return JS::js_undefined(); },
        [](auto const& value) -> JS::Value { return value; });
}

}
