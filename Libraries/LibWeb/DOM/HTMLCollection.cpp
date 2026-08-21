/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/InsertionSort.h>
#include <LibGC/Heap.h>
#include <LibGC/WeakInlines.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/Namespace.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(HTMLCollection);

GC::Ref<HTMLCollection> HTMLCollection::create(ParentNode& root, Scope scope, Function<bool(Element const&)> filter, AttributeInvalidationType attribute_invalidation_type, Function<bool(Element const&, Element const&)> sort, Kind kind)
{
    return GC::Heap::the().allocate<HTMLCollection>(root, scope, move(filter), attribute_invalidation_type, move(sort), kind);
}

HTMLCollection::HTMLCollection(ParentNode& root, Scope scope, Function<bool(Element const&)> filter, AttributeInvalidationType attribute_invalidation_type, Function<bool(Element const&, Element const&)> sort, Kind kind)
    : GC::WeakContainer(heap())
    , m_root(root)
    , m_filter(move(filter))
    , m_sort(move(sort))
    , m_cache_registration(*this)
    , m_scope(scope)
    , m_kind(kind)
    , m_attribute_invalidation_type(attribute_invalidation_type)
{
}

HTMLCollection::~HTMLCollection() = default;

void HTMLCollection::finalize()
{
    Base::finalize();
    invalidate_cache();
}

void HTMLCollection::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_root);
    visitor.visit_possible_values(m_filter.raw_capture_range());
    visitor.visit_possible_values(m_sort.raw_capture_range());
}

GC::Cell const& HTMLCollection::owner_cell(Badge<GC::Heap>) const
{
    return *this;
}

void HTMLCollection::remove_dead_cells(Badge<GC::Heap>)
{
    m_cached_elements.remove_all_matching([&](GC::RawPtr<Element> const& element) {
        auto* block = GC::HeapBlock::from_cell(element.ptr());
        return !heap().is_live_heap_block(block) || element->state() != Cell::State::Live || !element->is_marked();
    });
    if (m_cached_name_to_element_mappings) {
        m_cached_name_to_element_mappings->remove_all_matching([&](Utf16FlyString const&, GC::RawPtr<Element> const& element) {
            auto* block = GC::HeapBlock::from_cell(element.ptr());
            return !heap().is_live_heap_block(block) || element->state() != Cell::State::Live || !element->is_marked();
        });
    }
}

void HTMLCollection::update_name_to_element_mappings_if_needed() const
{
    update_cache_if_needed();
    if (m_cached_name_to_element_mappings)
        return;
    m_cached_name_to_element_mappings = make<OrderedHashMap<Utf16FlyString, GC::RawPtr<Element>>>();
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
    m_cached_document->register_valid_html_collection_cache(AttributeInvalidationType::IdOrName);
}

void HTMLCollection::update_cache_if_needed() const
{
    auto& document = root()->document();
    auto invalidation_version = this->invalidation_version(document);

    if (m_cache_registration.list_node.is_in_list()
        && m_cached_document.ptr().ptr() == &document
        && m_cached_invalidation_version == invalidation_version) {
        return;
    }

    invalidate_cache();

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

    if (m_sort) {
        insertion_sort(m_cached_elements, [this](auto const& a, auto const& b) {
            return this->m_sort(*a, *b);
        });
    }

    m_cached_document = document;
    m_cached_invalidation_version = invalidation_version;
    ++m_cache_generation;
    document.register_valid_html_collection_cache(m_attribute_invalidation_type);
    m_root->register_html_collection_with_valid_cache(const_cast<HTMLCollection&>(*this));
}

void HTMLCollection::invalidate_cache() const
{
    if (!m_cache_registration.list_node.is_in_list())
        return;
    invalidate_name_to_element_mappings();
    if (auto cached_document = m_cached_document.ptr())
        cached_document->unregister_valid_html_collection_cache(m_attribute_invalidation_type);
    m_cache_registration.list_node.remove();
    m_cached_elements.clear();
}

void HTMLCollection::invalidate_name_to_element_mappings() const
{
    if (!m_cached_name_to_element_mappings)
        return;
    if (auto cached_document = m_cached_document.ptr())
        cached_document->unregister_valid_html_collection_cache(AttributeInvalidationType::IdOrName);
    m_cached_name_to_element_mappings = nullptr;
}

void HTMLCollection::invalidate_cache_for_tree_mutation(Node const& mutation_parent) const
{
    if (m_scope == Scope::Children && m_root.ptr() != &mutation_parent)
        return;
    invalidate_cache();
}

void HTMLCollection::invalidate_cache_for_attribute_change(Element const& element, AttributeInvalidationTypes invalidation_types) const
{
    if (m_root.ptr() == &element)
        return;
    if (m_scope == Scope::Children && m_root.ptr() != element.parent())
        return;

    auto membership_invalidation_type = HTMLCollectionCacheRegistration::attribute_invalidation_type_mask(m_attribute_invalidation_type);
    if (!(invalidation_types & membership_invalidation_type)) {
        if (invalidation_types & HTMLCollectionCacheRegistration::attribute_invalidation_type_mask(AttributeInvalidationType::IdOrName))
            invalidate_name_to_element_mappings();
        return;
    }

    if (m_sort) {
        invalidate_cache();
        return;
    }

    // OPTIMIZATION: The cache still records whether the element matched before the attribute
    // change. Keep the element vector when reevaluating the filter produces the same result.
    bool was_matching = m_cached_elements.contains([&](auto const& cached_element) { return cached_element.ptr() == &element; });
    if (was_matching != m_filter(element)) {
        invalidate_cache();
        return;
    }

    if (invalidation_types & HTMLCollectionCacheRegistration::attribute_invalidation_type_mask(AttributeInvalidationType::IdOrName))
        invalidate_name_to_element_mappings();
}

u64 HTMLCollection::invalidation_version(Document const& document) const
{
    switch (m_kind) {
    case Kind::Generic:
        return 0;
    case Kind::FormControls:
        return document.form_controls_version();
    case Kind::SelectedOptions:
        return document.option_selectedness_version();
    }
    VERIFY_NOT_REACHED();
}

GC::RootVector<GC::Ref<Element>> HTMLCollection::collect_matching_elements() const
{
    update_cache_if_needed();
    GC::RootVector<GC::Ref<Element>> elements;
    for (auto& element : m_cached_elements)
        elements.append(*element);
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
    return m_cached_elements[index].ptr();
}

// https://dom.spec.whatwg.org/#dom-htmlcollection-nameditem-key
Element* HTMLCollection::named_item(Utf16View key) const
{
    // 1. If key is the empty string, return null.
    if (key.is_empty())
        return nullptr;

    update_name_to_element_mappings_if_needed();
    if (auto it = m_cached_name_to_element_mappings->get(key); it.has_value())
        return it.value().ptr();
    return nullptr;
}

// https://dom.spec.whatwg.org/#ref-for-dfn-supported-property-names
bool HTMLCollection::is_supported_property_name(Utf16FlyString const& name) const
{
    update_name_to_element_mappings_if_needed();
    return m_cached_name_to_element_mappings->contains(name);
}

// https://dom.spec.whatwg.org/#ref-for-dfn-supported-property-names
Vector<Utf16FlyString> HTMLCollection::supported_property_names() const
{
    // 1. Let result be an empty list.
    Vector<Utf16FlyString> result;

    // 2. For each element represented by the collection, in tree order:
    update_name_to_element_mappings_if_needed();
    for (auto const& it : *m_cached_name_to_element_mappings) {
        result.append(it.key);
    }

    // 3. Return result.
    return result;
}

}
