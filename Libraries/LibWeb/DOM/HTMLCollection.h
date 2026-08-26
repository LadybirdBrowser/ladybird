/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakContainer.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/HTMLCollectionCacheRegistration.h>
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

class HTMLCollection
    : public Bindings::GCAllocatedWrappable
    , public GC::WeakContainer {
    WEB_WRAPPABLE(HTMLCollection, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(HTMLCollection);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    enum class Scope {
        Children,
        Descendants,
    };

    enum class Kind {
        Generic,
        FormControls,
        SelectedOptions,
    };

    using AttributeInvalidationType = HTMLCollectionCacheRegistration::AttributeInvalidationType;
    using AttributeInvalidationTypes = HTMLCollectionCacheRegistration::AttributeInvalidationTypes;

    [[nodiscard]] static GC::Ref<HTMLCollection> create(ParentNode& root, Scope, ESCAPING Function<bool(Element const&)> filter, AttributeInvalidationType, ESCAPING Function<bool(Element const&, Element const&)> sort = nullptr, Kind = Kind::Generic);

    virtual ~HTMLCollection() override;

    size_t length() const;
    Element* item(size_t index) const;
    Element* named_item(Utf16View key) const;

    GC::RootVector<GC::Ref<Element>> collect_matching_elements() const;

    virtual Vector<Utf16FlyString> supported_property_names() const override;
    virtual bool is_supported_property_name(Utf16FlyString const&) const override;

protected:
    HTMLCollection(ParentNode& root, Scope, ESCAPING Function<bool(Element const&)> filter, AttributeInvalidationType, ESCAPING Function<bool(Element const&, Element const&)> sort = nullptr, Kind = Kind::Generic);

    GC::Ref<ParentNode> root() { return *m_root; }
    GC::Ref<ParentNode const> root() const { return *m_root; }
    bool element_matches_filter(Element const& element) const { return m_filter(element); }

    virtual void collect_elements_into(Vector<GC::RawPtr<Element>>&) const;

    virtual void visit_edges(GC::Cell::Visitor&) override;

private:
    virtual void finalize() override;
    virtual void remove_dead_cells(Badge<GC::Heap>) override;
    virtual GC::Cell const& owner_cell(Badge<GC::Heap>) const override;

    void update_cache_if_needed() const;
    void update_name_to_element_mappings_if_needed() const;
    u64 invalidation_version(Document const&) const;
    void invalidate_cache() const;
    void invalidate_name_to_element_mappings() const;
    void invalidate_cache_for_tree_mutation(Node const& mutation_parent) const;
    void invalidate_cache_for_attribute_change(Element const&, AttributeInvalidationTypes) const;

    friend class Node;
    friend class Web::Internals::Internals;

    u64 cache_generation_for_testing() const { return m_cache_generation; }

    mutable GC::Weak<Document const> m_cached_document;
    mutable u64 m_cached_invalidation_version { 0 };
    mutable u64 m_cache_generation { 0 };
    mutable Vector<GC::RawPtr<Element>> m_cached_elements;
    mutable OwnPtr<OrderedHashMap<Utf16FlyString, GC::RawPtr<Element>>> m_cached_name_to_element_mappings;

    GC::Ref<ParentNode> m_root;
    Function<bool(Element const&)> m_filter;
    Function<bool(Element const&, Element const&)> m_sort;

    mutable HTMLCollectionCacheRegistration m_cache_registration;

    Scope m_scope { Scope::Descendants };
    Kind m_kind { Kind::Generic };
    AttributeInvalidationType m_attribute_invalidation_type { AttributeInvalidationType::None };
};

}
