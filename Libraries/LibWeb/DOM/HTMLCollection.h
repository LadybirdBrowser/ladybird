/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibGC/RootVector.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakContainer.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/ClassNames.h>
#include <LibWeb/DOM/RustFFI.h>
#include <LibWeb/Forward.h>

namespace Web::Internals {

class Internals;

}

namespace Web::DOM {

// NOTE: HTMLCollection is in the DOM namespace because it's part of the DOM specification.

// This class implements a live, filtered view of a DOM subtree.
// When constructing an HTMLCollection, you provide a root node + a filter.
// The filter is a simple Function object that answers the question
// "is this Element part of the collection?"
// NB: libweb_rust walks the subtree and keeps the collection's state, see Rust/src/dom/collection.rs. It matches the
//     filters that need no C++ state itself, see BuiltinFilter.

class HTMLCollection
    : public Bindings::GCAllocatedWrappable
    , public GC::WeakContainer {
    WEB_WRAPPABLE(HTMLCollection, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(HTMLCollection);

public:
    enum class Scope : u8 {
        Children = to_underlying(RustFFI::CollectionScope::Children),
        Descendants = to_underlying(RustFFI::CollectionScope::Descendants),
    };

    enum class Kind : u8 {
        Generic,
        FormControls,
        SelectedOptions,
    };

    // The attributes whose changes can change which elements a filter matches.
    enum class AttributeInvalidationType : u8 {
        None,
        Class,
        Name,
        IdOrName,
        Href,
        FormControls,
    };

    using Filter = GC::Ref<GC::Function<bool(Element const&)>>;
    using Sort = GC::Ptr<GC::Function<bool(Element const&, Element const&)>>;

    // A filter that libweb_rust matches, see RustFFI::CollectionFilter for what each compares with its names.
    struct BuiltinFilter {
        RustFFI::CollectionFilter filter { RustFFI::CollectionFilter::All };
        Optional<Utf16FlyString> name {};
        Optional<Utf16FlyString> other_name {};
        ClassNames class_names {};
        bool in_quirks_mode { false };
    };

    [[nodiscard]] static GC::Ref<HTMLCollection> create(ParentNode& root, Scope, Filter, AttributeInvalidationType, Sort = nullptr, Kind = Kind::Generic);
    [[nodiscard]] static GC::Ref<HTMLCollection> create(ParentNode& root, Scope, BuiltinFilter, AttributeInvalidationType = AttributeInvalidationType::None);

    // NB: The core forgets the nodes the collection found when the collection is swept, not when it is finalized, as
    //     the C++ members that held them did, so that the finalizer of another dead cell can still read them.
    GC_ALLOW_CELL_DESTRUCTOR virtual ~HTMLCollection() override;

    size_t length() const;
    Element* item(size_t index) const;
    Element* named_item(Utf16View key) const;

    GC::RootVector<GC::Ref<Element>> collect_matching_elements() const;

    virtual Vector<Utf16FlyString> supported_property_names() const override;
    virtual bool is_supported_property_name(Utf16FlyString const&) const override;

    // Whether the filter matches element, for libweb_rust.
    bool element_matches_filter(Element const& element) const { return (*m_filter)(element); }

protected:
    HTMLCollection(ParentNode& root, Scope, Filter, AttributeInvalidationType, Sort = nullptr, Kind = Kind::Generic);
    HTMLCollection(ParentNode& root, Scope, BuiltinFilter, AttributeInvalidationType);

    GC::Ref<ParentNode> root();
    GC::Ref<ParentNode const> root() const;

    // Collections whose elements are in an order of their own, rather than in tree order, put them in that order in C++.
    void keep_elements_in_own_order() { m_core.cpp_state |= keeps_elements_in_own_order_bit; }
    virtual void collect_elements_into(GC::RootVector<GC::Ref<Node>>&) const;

    virtual void visit_edges(GC::Cell::Visitor&) override;

private:
    // ^GC::WeakContainer
    virtual GC::Cell const& owner_cell(Badge<GC::Heap>) const override;
    virtual void remove_dead_cells(Badge<GC::Heap>) override;

    // NB: The core's byte for C++ holds the kind of the collection and whether it keeps its elements in an order of
    //     its own. It is zero for a collection with no C++ state.
    static constexpr u8 kind_mask = 0x3;
    static constexpr u8 keeps_elements_in_own_order_bit = 1 << 2;
    Kind kind() const { return static_cast<Kind>(m_core.cpp_state & kind_mask); }
    bool keeps_elements_in_own_order() const { return m_core.cpp_state & keeps_elements_in_own_order_bit; }
    bool has_cpp_state() const { return m_core.cpp_state != 0; }

    size_t length_with_cpp_state() const;
    Element* item_with_cpp_state(size_t index) const;
    u64 invalidation_version(Document const&) const;
    void forget_elements_if_kind_changed() const
    {
        if (kind() != Kind::Generic) [[unlikely]]
            forget_elements_if_document_state_changed();
    }
    void forget_elements_if_document_state_changed() const;
    void update_elements_in_own_order_if_needed() const;
    void update_name_to_element_mappings_if_needed() const;

    friend class Web::Internals::Internals;

    bool cache_is_valid() const;

    // https://dom.spec.whatwg.org/#concept-collection
    // NB: libweb_rust reads and writes this in place, see Rust/src/dom/collection.rs. A collection that keeps its
    //     elements in an order of its own keeps them in the core's list.
    mutable RustFFI::CollectionCore m_core;
    // NB: libweb_rust reads these where the filter is a named one.
    RustFFI::CollectionNames m_names;

    GC::Ptr<GC::Function<bool(Element const&)>> m_filter;

    struct NameToElementMappings {
        // NB: The collection holds the elements weakly, see remove_dead_cells().
        OrderedHashMap<Utf16FlyString, GC::Weak<Element>> map;
        // The versions the mappings were found at.
        u64 tree_version { 0 };
        u64 attribute_version { 0 };
        u64 id_or_name_version { 0 };
    };
    // The state only some collections need.
    struct RareData {
        AK_ALLOC_WITH_KMALLOC;

        Sort sort;
        // The version of the state of the document a collection of another kind than Generic reads.
        u64 cached_kind_version { 0 };
        Optional<NameToElementMappings> name_to_element_mappings;
    };
    RareData& ensure_rare_data() const;
    mutable OwnPtr<RareData> m_rare_data;
};

}
