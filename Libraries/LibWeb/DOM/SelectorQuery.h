/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class IsolatedSelectorQueryCacheEntry;
class IsolatedSelectorQueryEngine;

// A selectors string parsed for use by querySelector(All), matches() and closest().
// Documents cache these per selector string, so one SelectorQuery may be reused by many queries.
class SelectorQuery : public RefCounted<SelectorQuery> {
public:
    static NonnullRefPtr<SelectorQuery> create(Document& document, CSS::SelectorList&& selectors)
    {
        return adopt_ref(*new SelectorQuery(document, move(selectors)));
    }

    ~SelectorQuery();

    CSS::SelectorList const& selectors() const { return m_selectors; }

    bool is_result_cacheable() const { return m_is_result_cacheable; }
    bool depends_on_character_data() const { return m_depends_on_character_data; }

    GC::Ptr<Element> query_first(ParentNode&) const;
    GC::Ref<NodeList> query_all(ParentNode&) const;
    bool matches(Element const&, ParentNode const& scope) const;
    GC::Ptr<Element const> closest(Element const&) const;

private:
    SelectorQuery(Document&, CSS::SelectorList&&);

    bool matches_simple_selector_in_dom(Element const&) const;
    bool matches_in_style_engine(Element const&, ParentNode const& scope) const;
    IsolatedSelectorQueryEngine& isolated_engine_for(ParentNode&) const;

    CSS::SelectorList m_selectors;
    void* m_engine_query { nullptr };
    bool m_can_match_in_dom { false };

    // Whether the selector is a lone `*`, which every element matches: a query then collects the
    // subtree's elements without matching any of them.
    bool m_matches_every_element { false };

    // Whether matching can only change when the document's dom_tree_version (plus character_data_version, see below)
    // changes. Queries with selectors that depend on other state (:hover, :checked, :target, etc) are not cacheable.
    bool m_is_result_cacheable { false };

    // Whether matching also depends on character data (only :empty), so cached results must additionally be
    // validated against the document's character_data_version.
    bool m_depends_on_character_data { false };

    mutable Vector<NonnullOwnPtr<IsolatedSelectorQueryCacheEntry>> m_isolated_engine_cache;
};

// Caches querySelector first matches and querySelectorAll element lists per (query root, selector query), allowing
// repeated queries against an unchanged document to skip the subtree walk. Entries are validated lazily against
// the document's mutation version counters, so no notification on DOM mutation is needed: the whole cache is
// emptied on first use after dom_tree_version changes.
class QuerySelectorResultCache {
    AK_MAKE_NONCOPYABLE(QuerySelectorResultCache);
    AK_MAKE_NONMOVABLE(QuerySelectorResultCache);

public:
    enum class ResultType {
        FirstOnly,
        All,
    };

    QuerySelectorResultCache() = default;

    Vector<GC::RawPtr<Element>> const* get(Document const&, ParentNode const& root, SelectorQuery const&, ResultType);
    void set(Document const&, ParentNode const& root, SelectorQuery const&, ResultType, Vector<GC::RawPtr<Element>>);
    void visit_edges(GC::Cell::Visitor&);

private:
    void clear_if_dom_tree_changed(Document const&);

    struct Key {
        GC::RawPtr<ParentNode const> root;
        SelectorQuery const* query { nullptr };
        bool operator==(Key const&) const = default;
    };

    struct KeyTraits : public DefaultTraits<Key> {
        static unsigned hash(Key const& key) { return pair_int_hash(ptr_hash(key.root.ptr()), ptr_hash(key.query)); }
    };

    struct Entry {
        // Weak so the cache never keeps a disconnected query root alive, and so that a dead root can never be
        // confused with a new node allocated at the same address.
        GC::Weak<ParentNode> root;

        // Keeps the query alive (and its address unique) even if the document's selector query cache evicts it.
        NonnullRefPtr<SelectorQuery const> query;

        u64 character_data_version { 0 };
        ResultType result_type { ResultType::FirstOnly };

        // Raw pointers are safe here: the elements were descendants of root when cached, and with dom_tree_version
        // unchanged no node in this document has been inserted or removed since, so they are still descendants of
        // root and kept alive by it. Entries are only used after validating the version and that root is alive.
        Vector<GC::RawPtr<Element>> elements;
    };

    u64 m_dom_tree_version { 0 };
    HashMap<Key, Entry, KeyTraits> m_entries;
};

}
