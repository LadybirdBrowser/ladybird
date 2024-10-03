/*
 * Copyright (c) 2021-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLCollectionPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/Namespace.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(HTMLCollection);

JS::NonnullGCPtr<HTMLCollection> HTMLCollection::create(ParentNode& root, Scope scope, Function<bool(Element const&)> filter)
{
    return root.heap().allocate<HTMLCollection>(root.realm(), root, scope, move(filter));
}

HTMLCollection::HTMLCollection(ParentNode& root, Scope scope, Function<bool(Element const&)> filter)
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

HTMLCollection::~HTMLCollection() = default;

void HTMLCollection::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLCollection);
}

void HTMLCollection::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_root);
    visitor.visit(m_cached_elements);
    if (m_cached_name_to_element_mappings)
        visitor.visit(*m_cached_name_to_element_mappings);
}

void HTMLCollection::update_name_to_element_mappings_if_needed() const
{
    update_cache_if_needed();
    if (m_cached_name_to_element_mappings)
        return;
    m_cached_name_to_element_mappings = make<OrderedHashMap<FlyString, JS::NonnullGCPtr<Element>>>();
    for (auto const& element : m_cached_elements) {
        // 1. If element has an ID which is not in result, append element’s ID to result.
        if (auto const& id = element->id(); id.has_value()) {
            if (!id.value().is_empty() && !m_cached_name_to_element_mappings->contains(id.value()))
                m_cached_name_to_element_mappings->set(id.value(), element);
        }

        // 2. If element is in the HTML namespace and has a name attribute whose value is neither the empty string nor is in result, append element’s name attribute value to result.
        if (element->namespace_uri() == Namespace::HTML && element->name().has_value()) {
            auto element_name = element->name().value();
            if (!element_name.is_empty() && !m_cached_name_to_element_mappings->contains(element_name))
                m_cached_name_to_element_mappings->set(move(element_name), element);
        }
    }
}

void HTMLCollection::update_cache_if_needed() const
{
    // Nothing to do, the DOM hasn't updated since we last built the cache.
    if (m_cached_dom_tree_version == root()->document().dom_tree_version())
        return;

    m_cached_elements.clear();
    m_cached_name_to_element_mappings = nullptr;
    if (m_scope == Scope::Descendants) {
        m_root->for_each_in_subtree_of_type<Element>([&](auto& element) {
            if (m_filter(element))
                m_cached_elements.append(element);
            return TraversalDecision::Continue;
        });
    } else {
        m_root->for_each_child_of_type<Element>([&](auto& element) {
            if (m_filter(element))
                m_cached_elements.append(element);
            return IterationDecision::Continue;
        });
    }
    m_cached_dom_tree_version = root()->document().dom_tree_version();
}

JS::MarkedVector<JS::NonnullGCPtr<Element>> HTMLCollection::collect_matching_elements() const
{
    update_cache_if_needed();
    JS::MarkedVector<JS::NonnullGCPtr<Element>> elements(heap());
    for (auto& element : m_cached_elements)
        elements.append(element);
    return elements;
}

// https://dom.spec.whatwg.org/#dom-htmlcollection-length
size_t HTMLCollection::length() const
{
    // The length getter steps are to return the number of nodes represented by the collection.
    update_cache_if_needed();
    return m_cached_elements.size();
}

// https://dom.spec.whatwg.org/#dom-htmlcollection-item
Element* HTMLCollection::item(size_t index) const
{
    // The item(index) method steps are to return the indexth element in the collection. If there is no indexth element in the collection, then the method must return null.
    update_cache_if_needed();
    if (index >= m_cached_elements.size())
        return nullptr;
    return m_cached_elements[index];
}

// https://dom.spec.whatwg.org/#dom-htmlcollection-nameditem-key
Element* HTMLCollection::named_item(FlyString const& key) const
{
    // 1. If key is the empty string, return null.
    if (key.is_empty())
        return nullptr;

    update_name_to_element_mappings_if_needed();
    if (auto it = m_cached_name_to_element_mappings->get(key); it.has_value())
        return it.value();
    return nullptr;
}

// https://dom.spec.whatwg.org/#ref-for-dfn-supported-property-names
bool HTMLCollection::is_supported_property_name(FlyString const& name) const
{
    update_name_to_element_mappings_if_needed();
    return m_cached_name_to_element_mappings->contains(name);
}

// https://dom.spec.whatwg.org/#ref-for-dfn-supported-property-names
Vector<FlyString> HTMLCollection::supported_property_names() const
{
    // 1. Let result be an empty list.
    Vector<FlyString> result;

    // 2. For each element represented by the collection, in tree order:
    update_name_to_element_mappings_if_needed();
    for (auto const& it : *m_cached_name_to_element_mappings) {
        result.append(it.key);
    }

    // 3. Return result.
    return result;
}

Optional<JS::Value> HTMLCollection::item_value(size_t index) const
{
    auto* element = item(index);
    if (!element)
        return {};
    return element;
}

JS::Value HTMLCollection::named_item_value(FlyString const& name) const
{
    auto* element = named_item(name);
    if (!element)
        return JS::js_undefined();
    return element;
}
}
