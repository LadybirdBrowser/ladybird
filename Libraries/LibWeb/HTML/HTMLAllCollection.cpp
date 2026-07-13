/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/NumericLimits.h>
#include <LibJS/Runtime/PropertyKey.h>
#include <LibWeb/Bindings/HTMLAllCollection.h>
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
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLAllCollection);
    Base::initialize(realm);
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

static Optional<u32> parse_array_index_property_name(Utf16View name_or_index)
{
    if (name_or_index.is_empty())
        return {};

    auto first_code_unit = name_or_index.code_unit_at(0);
    if (!is_ascii_digit(first_code_unit) || (first_code_unit == '0' && name_or_index.length_in_code_units() > 1))
        return {};

    auto property_index = name_or_index.to_number<u32>(TrimWhitespace::No);
    if (!property_index.has_value() || property_index.value() >= NumericLimits<u32>::max())
        return {};

    return property_index;
}

static bool all_collection_element_matches_name(DOM::Element const& element, Utf16View name)
{
    // * "all"-named elements with a name attribute equal to name, or
    if (is_all_named_element(element) && element.name().has_value() && element.name()->view() == name)
        return true;

    // * elements with an ID equal to name.
    return element.id().has_value() && element.id()->view() == name;
}

struct AllNamedElementsLookupResult {
    GC::Ptr<DOM::Element> first_matching_element;
    bool has_multiple_matching_elements { false };
};

static AllNamedElementsLookupResult find_all_named_elements(DOM::ParentNode& root, Utf16View name)
{
    AllNamedElementsLookupResult result;
    root.for_each_in_subtree_of_type<DOM::Element>([&](DOM::Element& element) {
        if (!all_collection_element_matches_name(element, name))
            return TraversalDecision::Continue;

        if (result.first_matching_element) {
            result.has_multiple_matching_elements = true;
            return TraversalDecision::Break;
        }

        result.first_matching_element = element;
        return TraversalDecision::Continue;
    });
    return result;
}

static GC::Ref<DOM::HTMLCollection> create_all_named_elements_sub_collection(DOM::ParentNode& root, Utf16String name)
{
    // 2. Let subCollection be an HTMLCollection object rooted at the same Document as collection, whose filter matches only elements that are either:
    return DOM::HTMLCollection::create(root, DOM::HTMLCollection::Scope::Descendants, [name = move(name)](DOM::Element const& element) {
        return all_collection_element_matches_name(element, name);
    });
}

static Variant<GC::Ref<DOM::HTMLCollection>, GC::Ref<DOM::Element>, Empty> get_the_all_named_elements(DOM::ParentNode& root, Utf16View name)
{
    // 1. If name is the empty string, return null.
    if (name.is_empty())
        return Empty {};

    auto lookup_result = find_all_named_elements(root, name);

    // 4. If subCollection is empty, return null.
    if (!lookup_result.first_matching_element)
        return Empty {};

    // 3. If there is exactly one element in subCollection, then return that element.
    if (!lookup_result.has_multiple_matching_elements)
        return GC::Ref { *lookup_result.first_matching_element };

    // 5. Otherwise, return subCollection.
    return create_all_named_elements_sub_collection(root, Utf16String::from_utf16(name));
}

GC::RootVector<GC::Ref<DOM::Element>> HTMLAllCollection::collect_matching_elements() const
{
    GC::RootVector<GC::Ref<DOM::Element>> elements;
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
Variant<GC::Ref<DOM::HTMLCollection>, GC::Ref<DOM::Element>, Empty> HTMLAllCollection::item(Optional<Utf16String> const& name_or_index) const
{
    // 1. If nameOrIndex was not provided, return null.
    if (!name_or_index.has_value())
        return Empty {};

    // 2. Return the result of getting the "all"-indexed or named element(s) from this, given nameOrIndex.
    return get_the_all_indexed_or_named_elements(name_or_index->utf16_view());
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#dom-htmlallcollection-nameditem
Variant<GC::Ref<DOM::HTMLCollection>, GC::Ref<DOM::Element>, Empty> HTMLAllCollection::named_item(Utf16View name) const
{
    // The namedItem(name) method steps are to return the result of getting the "all"-named element(s) from this given name.
    return get_the_all_named_elements(name);
}

// https://dom.spec.whatwg.org/#ref-for-dfn-supported-property-names
Vector<Utf16FlyString> HTMLAllCollection::supported_property_names() const
{
    // The supported property names consist of the non-empty values of all the id attributes of all the
    // elements represented by the collection, and the non-empty values of all the name attributes of
    // all the "all"-named elements represented by the collection, in tree order, ignoring later duplicates,
    // with the id of an element preceding its name if it contributes both, they differ from each other, and
    // neither is the duplicate of an earlier entry.

    Vector<Utf16FlyString> names;
    auto elements = collect_matching_elements();

    for (auto const& element : elements) {
        if (auto const& id = element->id(); id.has_value() && !id->is_empty()) {
            if (!names.contains_slow(id.value()))
                names.append(id.value());
        }

        if (is_all_named_element(*element) && element->name().has_value() && !element->name()->is_empty()) {
            auto name = element->name().value();
            if (!name.is_empty() && !names.contains_slow(name))
                names.append(move(name));
        }
    }

    // 3. Return result.
    Vector<Utf16FlyString> result;
    result.ensure_capacity(names.size());
    for (auto const& name : names)
        result.append(name);
    return result;
}

// https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#concept-get-all-named
Variant<GC::Ref<DOM::HTMLCollection>, GC::Ref<DOM::Element>, Empty> HTMLAllCollection::get_the_all_named_elements(Utf16View name) const
{
    return Web::HTML::get_the_all_named_elements(m_root, name);
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
Variant<GC::Ref<DOM::HTMLCollection>, GC::Ref<DOM::Element>, Empty> HTMLAllCollection::get_the_all_indexed_or_named_elements(Utf16View name_or_index) const
{
    // 1. If nameOrIndex, converted to a JavaScript String value, is an array index property name, return the result of getting the "all"-indexed element from
    //    collection given the number represented by nameOrIndex.
    if (auto index = parse_array_index_property_name(name_or_index); index.has_value()) {
        auto maybe_element = get_the_all_indexed_element(*index);
        if (!maybe_element)
            return Empty {};
        return GC::Ref<DOM::Element> { *maybe_element };
    }

    // 2. Return the result of getting the "all"-named element(s) from collection given nameOrIndex.
    return get_the_all_named_elements(name_or_index);
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
    return get_the_all_named_elements(name_or_index.as_string().view());
}

Optional<JS::Value> HTMLAllCollection::item_value(size_t index) const
{
    if (auto value = get_the_all_indexed_element(index))
        return value;
    return {};
}

JS::Value HTMLAllCollection::named_item_value(Utf16FlyString const& name) const
{
    return get_the_all_named_elements(name.view()).visit([](Empty) -> JS::Value { return JS::js_undefined(); }, [](auto const& value) -> JS::Value { return value; });
}

}
