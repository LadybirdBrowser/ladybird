/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/InsertionSort.h>
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/ParentNode.h>
#include <LibWeb/Namespace.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(HTMLCollection);

// NB: The Rust definition of the core asserts the same layout.
static_assert(sizeof(RustFFI::CollectionCore) == 56 && alignof(RustFFI::CollectionCore) == 8);
static_assert(offsetof(RustFFI::CollectionCore, stamp) == 32);
static_assert(offsetof(RustFFI::CollectionCore, filter) == 40);
static_assert(offsetof(RustFFI::CollectionCore, list) == 48);
static_assert(sizeof(RustFFI::CollectionNames) == 40 && alignof(RustFFI::CollectionNames) == 8);
static_assert(offsetof(RustFFI::CollectionNames, class_names) == 16);

// NB: DocumentCore counts the changes to the attributes of each type but None, see Rust/src/dom/attributes.rs.
static_assert(to_underlying(HTMLCollection::AttributeInvalidationType::FormControls) == RustFFI::COLLECTION_ATTRIBUTE_TYPE_COUNT);

GC::Ref<HTMLCollection> HTMLCollection::create(ParentNode& root, Scope scope, Filter filter, AttributeInvalidationType attribute_invalidation_type, Sort sort, Kind kind)
{
    return GC::Heap::the().allocate<HTMLCollection>(root, scope, filter, attribute_invalidation_type, sort, kind);
}

GC::Ref<HTMLCollection> HTMLCollection::create(ParentNode& root, Scope scope, BuiltinFilter filter, AttributeInvalidationType attribute_invalidation_type)
{
    return GC::Heap::the().allocate<HTMLCollection>(root, scope, move(filter), attribute_invalidation_type);
}

static RustFFI::CollectionCore make_core(ParentNode& root, HTMLCollection::Scope scope, HTMLCollection::AttributeInvalidationType attribute_invalidation_type, RustFFI::CollectionFilter filter, bool in_quirks_mode, HTMLCollection::Kind kind)
{
    return {
        .root = &root,
        .cursor = nullptr,
        .cursor_index = 0,
        .length = RustFFI::COLLECTION_LENGTH_UNKNOWN,
        .tree_version = 0,
        .attribute_version = 0,
        .stamp = 0,
        .filter = filter,
        .scope = static_cast<RustFFI::CollectionScope>(to_underlying(scope)),
        .validity = scope == HTMLCollection::Scope::Children ? RustFFI::CollectionValidity::ElementChildren : RustFFI::CollectionValidity::ElementTree,
        .attribute_type = to_underlying(attribute_invalidation_type),
        .flags = static_cast<u8>(in_quirks_mode ? RustFFI::COLLECTION_IN_QUIRKS_MODE : 0),
        .cpp_state = to_underlying(kind),
        .list = nullptr,
    };
}

HTMLCollection::HTMLCollection(ParentNode& root, Scope scope, Filter filter, AttributeInvalidationType attribute_invalidation_type, Sort sort, Kind kind)
    : GC::WeakContainer(heap())
    , m_core(make_core(root, scope, attribute_invalidation_type, RustFFI::CollectionFilter::Host, false, kind))
    , m_filter(filter)
{
    // NB: libweb_rust finds the core and the names of a collection at these offsets into the collection's cell.
    static_assert(offsetof(HTMLCollection, m_core) == RustFFI::COLLECTION_CORE_OFFSET);

    // NB: The fields libweb_rust reads start after all of the base class, where every C++ ABI places them, and not in
    //     its tail padding, where only the Itanium ABI could.
    static_assert(offsetof(HTMLCollection, m_core) >= sizeof(Bindings::GCAllocatedWrappable));
    static_assert(offsetof(HTMLCollection, m_names) == RustFFI::COLLECTION_NAMES_OFFSET);

    static_assert(to_underlying(Kind::SelectedOptions) <= kind_mask);
    if (sort) {
        keep_elements_in_own_order();
        ensure_rare_data().sort = sort;
    }
}

HTMLCollection::HTMLCollection(ParentNode& root, Scope scope, BuiltinFilter filter, AttributeInvalidationType attribute_invalidation_type)
    : GC::WeakContainer(heap())
    , m_core(make_core(root, scope, attribute_invalidation_type, filter.filter, filter.in_quirks_mode, Kind::Generic))
    , m_names {
        .name = move(filter.name),
        .other_name = move(filter.other_name),
        .class_names = move(filter.class_names),
    }
{
}

HTMLCollection::~HTMLCollection()
{
    RustFFI::rust_dom_collection_forget_nodes(this);
}

void HTMLCollection::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_core.root);
    visitor.visit(m_filter);
    if (m_rare_data)
        visitor.visit(m_rare_data->sort);
}

HTMLCollection::RareData& HTMLCollection::ensure_rare_data() const
{
    if (!m_rare_data)
        m_rare_data = make<RareData>();
    return *m_rare_data;
}

GC::Cell const& HTMLCollection::owner_cell(Badge<GC::Heap>) const
{
    return *this;
}

// NB: The collection holds the elements it found weakly. While the versions they were found at are current, they are
//     all in the tree below the root, which it keeps alive, so one that died means the collection has to find them
//     again anyway.
void HTMLCollection::remove_dead_cells(Badge<GC::Heap>)
{
    auto is_dead = [&](Node const* node) { return node && (!heap().is_live_cell(node) || !node->is_marked()); };
    bool found_dead_node = is_dead(m_core.cursor.ptr());
    for (u32 index = 0; m_core.list && !found_dead_node && index < m_core.length; ++index)
        found_dead_node = is_dead(m_core.list[index].ptr());
    if (found_dead_node)
        RustFFI::rust_dom_collection_forget_nodes(this);
    if (m_rare_data && m_rare_data->name_to_element_mappings.has_value()) {
        m_rare_data->name_to_element_mappings->map.remove_all_matching([](Utf16FlyString const&, GC::Weak<Element> const& element) {
            return !element;
        });
    }
}

GC::Ref<ParentNode> HTMLCollection::root()
{
    return as<ParentNode>(*m_core.root);
}

GC::Ref<ParentNode const> HTMLCollection::root() const
{
    return as<ParentNode>(*m_core.root);
}

u64 HTMLCollection::invalidation_version(Document const& document) const
{
    switch (kind()) {
    case Kind::Generic:
        return 0;
    case Kind::FormControls:
        return document.form_controls_version();
    case Kind::SelectedOptions:
        return document.option_selectedness_version();
    }
    VERIFY_NOT_REACHED();
}

// A collection of another kind than Generic also reads state of the document that no tree version counts, so it finds
// its elements again whenever that changes.
void HTMLCollection::forget_elements_if_document_state_changed() const
{
    auto& rare_data = ensure_rare_data();
    auto invalidation_version = this->invalidation_version(root()->document());
    if (invalidation_version == rare_data.cached_kind_version)
        return;
    rare_data.cached_kind_version = invalidation_version;
    RustFFI::rust_dom_collection_forget_nodes(this);
    rare_data.name_to_element_mappings.clear();
}

void HTMLCollection::collect_elements_into(GC::RootVector<GC::Ref<Node>>& elements) const
{
    for (u32 index = 0;; ++index) {
        auto const* node = RustFFI::rust_dom_collection_item(this, index);
        if (!node)
            return;
        elements.append(const_cast<Node&>(*node));
    }
}

void HTMLCollection::update_elements_in_own_order_if_needed() const
{
    forget_elements_if_kind_changed();
    if (RustFFI::rust_dom_collection_validate(this) && m_core.list)
        return;
    GC::RootVector<GC::Ref<Node>> elements;
    collect_elements_into(elements);
    if (m_rare_data && m_rare_data->sort) {
        insertion_sort(elements, [this](auto const& a, auto const& b) {
            return (*m_rare_data->sort)(as<Element>(*a), as<Element>(*b));
        });
    }
    RustFFI::rust_dom_collection_remember_nodes_in_own_order(this, elements.span());
}

bool HTMLCollection::cache_is_valid() const
{
    if (kind() != Kind::Generic && (!m_rare_data || invalidation_version(root()->document()) != m_rare_data->cached_kind_version))
        return false;
    if (!RustFFI::rust_dom_collection_versions_are_current(this))
        return false;
    if (keeps_elements_in_own_order())
        return m_core.list;
    return m_core.length != RustFFI::COLLECTION_LENGTH_UNKNOWN || m_core.cursor;
}

void HTMLCollection::update_name_to_element_mappings_if_needed() const
{
    auto length = this->length();
    auto id_or_name_version = root()->document().collection_attribute_version(to_underlying(AttributeInvalidationType::IdOrName));
    auto& rare_data = ensure_rare_data();
    if (rare_data.name_to_element_mappings.has_value()
        && rare_data.name_to_element_mappings->tree_version == m_core.tree_version
        && rare_data.name_to_element_mappings->attribute_version == m_core.attribute_version
        && rare_data.name_to_element_mappings->id_or_name_version == id_or_name_version) {
        return;
    }
    rare_data.name_to_element_mappings = NameToElementMappings {};
    auto& mappings = rare_data.name_to_element_mappings->map;
    for (size_t index = 0; index < length; ++index) {
        auto& element = *item(index);

        // 1. If element has an ID which is not in result, append element’s ID to result.
        if (auto const& id = element.id(); id.has_value()) {
            if (!id.value().is_empty() && !mappings.contains(id.value()))
                mappings.set(id.value(), element);
        }

        // 2. If element is in the HTML namespace and has a name attribute whose value is neither the empty string nor is in result, append element’s name attribute value to result.
        if (element.namespace_uri() == Namespace::HTML && element.name().has_value()) {
            auto element_name = element.name().value();
            if (!element_name.is_empty() && !mappings.contains(element_name))
                mappings.set(move(element_name), element);
        }
    }
    rare_data.name_to_element_mappings->tree_version = m_core.tree_version;
    rare_data.name_to_element_mappings->attribute_version = m_core.attribute_version;
    rare_data.name_to_element_mappings->id_or_name_version = id_or_name_version;
}

GC::RootVector<GC::Ref<Element>> HTMLCollection::collect_matching_elements() const
{
    GC::RootVector<GC::Ref<Element>> elements;
    auto length = this->length();
    elements.ensure_capacity(length);
    for (size_t index = 0; index < length; ++index)
        elements.unchecked_append(*item(index));
    return elements;
}

// https://dom.spec.whatwg.org/#dom-htmlcollection-length
size_t HTMLCollection::length() const
{
    // The length getter steps are to return the number of nodes represented by the collection.
    if (has_cpp_state()) [[unlikely]]
        return length_with_cpp_state();
    return RustFFI::rust_dom_collection_length(this);
}

size_t HTMLCollection::length_with_cpp_state() const
{
    if (keeps_elements_in_own_order()) {
        update_elements_in_own_order_if_needed();
        return m_core.length;
    }
    forget_elements_if_kind_changed();
    return RustFFI::rust_dom_collection_length(this);
}

// https://dom.spec.whatwg.org/#dom-htmlcollection-item
Element* HTMLCollection::item(size_t index) const
{
    // The item(index) method steps are to return the indexth element in the collection. If there is no indexth element in the collection, then the method must return null.
    if (has_cpp_state()) [[unlikely]]
        return item_with_cpp_state(index);
    if (index >= RustFFI::COLLECTION_LENGTH_UNKNOWN)
        return nullptr;
    return static_cast<Element*>(const_cast<Node*>(RustFFI::rust_dom_collection_item(this, index)));
}

Element* HTMLCollection::item_with_cpp_state(size_t index) const
{
    if (keeps_elements_in_own_order()) {
        update_elements_in_own_order_if_needed();
        if (index >= m_core.length)
            return nullptr;
        return static_cast<Element*>(m_core.list[index].ptr());
    }
    if (index >= RustFFI::COLLECTION_LENGTH_UNKNOWN)
        return nullptr;
    forget_elements_if_kind_changed();
    return static_cast<Element*>(const_cast<Node*>(RustFFI::rust_dom_collection_item(this, index)));
}

// https://dom.spec.whatwg.org/#dom-htmlcollection-nameditem-key
Element* HTMLCollection::named_item(Utf16View key) const
{
    // 1. If key is the empty string, return null.
    if (key.is_empty())
        return nullptr;

    update_name_to_element_mappings_if_needed();
    if (auto it = m_rare_data->name_to_element_mappings->map.get(key); it.has_value())
        return it.value().ptr().ptr();
    return nullptr;
}

// https://dom.spec.whatwg.org/#ref-for-dfn-supported-property-names
bool HTMLCollection::is_supported_property_name(Utf16FlyString const& name) const
{
    update_name_to_element_mappings_if_needed();
    return m_rare_data->name_to_element_mappings->map.contains(name);
}

// https://dom.spec.whatwg.org/#ref-for-dfn-supported-property-names
Vector<Utf16FlyString> HTMLCollection::supported_property_names() const
{
    // 1. Let result be an empty list.
    Vector<Utf16FlyString> result;

    // 2. For each element represented by the collection, in tree order:
    update_name_to_element_mappings_if_needed();
    for (auto const& it : m_rare_data->name_to_element_mappings->map) {
        result.append(it.key);
    }

    // 3. Return result.
    return result;
}

}
