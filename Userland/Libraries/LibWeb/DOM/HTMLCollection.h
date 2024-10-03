/*
 * Copyright (c) 2021-2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

// NOTE: HTMLCollection is in the DOM namespace because it's part of the DOM specification.

// This class implements a live, filtered view of a DOM subtree.
// When constructing an HTMLCollection, you provide a root node + a filter.
// The filter is a simple Function object that answers the question
// "is this Element part of the collection?"

class HTMLCollection : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(HTMLCollection, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(HTMLCollection);

public:
    enum class Scope {
        Children,
        Descendants,
    };
    [[nodiscard]] static JS::NonnullGCPtr<HTMLCollection> create(ParentNode& root, Scope, ESCAPING Function<bool(Element const&)> filter);

    virtual ~HTMLCollection() override;

    size_t length() const;
    Element* item(size_t index) const;
    Element* named_item(FlyString const& key) const;

    JS::MarkedVector<JS::NonnullGCPtr<Element>> collect_matching_elements() const;

    virtual Optional<JS::Value> item_value(size_t index) const override;
    virtual JS::Value named_item_value(FlyString const& name) const override;
    virtual Vector<FlyString> supported_property_names() const override;
    virtual bool is_supported_property_name(FlyString const&) const override;

protected:
    HTMLCollection(ParentNode& root, Scope, ESCAPING Function<bool(Element const&)> filter);

    virtual void initialize(JS::Realm&) override;

    JS::NonnullGCPtr<ParentNode> root() { return *m_root; }
    JS::NonnullGCPtr<ParentNode const> root() const { return *m_root; }

private:
    virtual void visit_edges(Cell::Visitor&) override;

    void update_cache_if_needed() const;
    void update_name_to_element_mappings_if_needed() const;

    mutable u64 m_cached_dom_tree_version { 0 };
    mutable Vector<JS::NonnullGCPtr<Element>> m_cached_elements;
    mutable OwnPtr<OrderedHashMap<FlyString, JS::NonnullGCPtr<Element>>> m_cached_name_to_element_mappings;

    JS::NonnullGCPtr<ParentNode> m_root;
    Function<bool(Element const&)> m_filter;

    Scope m_scope { Scope::Descendants };
};

}
