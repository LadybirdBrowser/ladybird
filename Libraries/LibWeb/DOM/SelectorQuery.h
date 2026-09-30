/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::SelectorFFI {

struct DomSelectorProgram;

}

namespace Web::DOM {

// A selectors string parsed for use by querySelector(All), matches() and closest().
// Documents cache these per selector string, so one SelectorQuery may be reused by many queries.
class SelectorQuery : public RefCounted<SelectorQuery> {
public:
    static NonnullRefPtr<SelectorQuery> create(CSS::SelectorList&& selectors)
    {
        return adopt_ref(*new SelectorQuery(move(selectors)));
    }

    ~SelectorQuery();

    CSS::SelectorList const& selectors() const { return m_selectors; }

    bool is_result_cacheable() const { return m_is_result_cacheable; }
    bool depends_on_character_data() const { return m_depends_on_character_data; }

    // Stamped by the document's selector query cache each time the query is handed out, so that a full cache can
    // evict the query used least recently.
    u64 last_use() const { return m_last_use; }
    void set_last_use(u64 use) const { m_last_use = use; }

    GC::Ptr<Element> query_first(ParentNode&) const;
    GC::Ref<NodeList> query_all(ParentNode&) const;
    bool matches(Element const&, ParentNode const& scope) const;
    GC::Ptr<Element const> closest(Element const&) const;

private:
    explicit SelectorQuery(CSS::SelectorList&&);

    CSS::SelectorFFI::DomSelectorProgram const& program(Document const&) const;

    CSS::SelectorList m_selectors;
    // The selectors compiled for the DOM matcher, when a query first needs them, for an HTML document or not.
    mutable CSS::SelectorFFI::DomSelectorProgram* m_program { nullptr };
    mutable bool m_program_is_for_html_document { false };
    mutable u64 m_last_use { 0 };

    // Whether the selector is a lone `*`, which every element matches: a query then collects the
    // subtree's elements without matching any of them.
    bool m_matches_every_element { false };

    // Whether matching can only change when the dom_tree_version (plus character_data_version, see below) of the
    // tree the element is in changes. Queries with selectors that depend on other state (:hover, :checked, :target,
    // etc) are not cacheable.
    bool m_is_result_cacheable { false };

    // Whether matching also depends on character data (only :empty), so cached results must additionally be
    // validated against the tree's character_data_version.
    bool m_depends_on_character_data { false };
};

// Caches querySelector first matches and querySelectorAll element lists per (query root, selector query), allowing
// repeated queries against an unchanged tree to skip the subtree walk. Entries are validated lazily against the
// mutation version counters of the tree the query root is in, so no notification on DOM mutation is needed.
class QuerySelectorResultCache {
    AK_MAKE_NONCOPYABLE(QuerySelectorResultCache);
    AK_MAKE_NONMOVABLE(QuerySelectorResultCache);

public:
    AK_ALLOC_WITH_KMALLOC;

    enum class ResultType {
        FirstOnly,
        All,
    };

    QuerySelectorResultCache() = default;

    Vector<GC::RawPtr<Element>> const* get(ParentNode const& root, SelectorQuery const&, ResultType);
    void set(ParentNode const& root, SelectorQuery const&, ResultType, Vector<GC::RawPtr<Element>>);
    void clear() { m_entries.clear(); }
    void visit_edges(GC::Cell::Visitor&);

private:
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

        u64 dom_tree_version { 0 };
        u64 character_data_version { 0 };
        ResultType result_type { ResultType::FirstOnly };

        // Raw pointers are safe here: the elements were descendants of root when cached, and with the tree's
        // dom_tree_version unchanged no node has been inserted into or removed from it since, so they are still
        // descendants of root and kept alive by it. Entries are only used after validating the version and that
        // root is alive.
        Vector<GC::RawPtr<Element>> elements;
    };

    HashMap<Key, Entry, KeyTraits> m_entries;
};

}
