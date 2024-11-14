/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/WeakPtr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class BrowsingContextGroup final : public JS::Cell {
    GC_CELL(BrowsingContextGroup, JS::Cell);
    GC_DECLARE_ALLOCATOR(BrowsingContextGroup);

public:
    struct BrowsingContextGroupAndDocument {
        GC::Ref<HTML::BrowsingContextGroup> browsing_context;
        GC::Ref<DOM::Document> document;
    };
    static WebIDL::ExceptionOr<BrowsingContextGroupAndDocument> create_a_new_browsing_context_group_and_document(GC::Ref<Page>);

    ~BrowsingContextGroup();

    Page& page() { return m_page; }
    Page const& page() const { return m_page; }

    auto& browsing_context_set() { return m_browsing_context_set; }
    auto const& browsing_context_set() const { return m_browsing_context_set; }

    void append(BrowsingContext&);

private:
    explicit BrowsingContextGroup(GC::Ref<Web::Page>);

    virtual void visit_edges(Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/browsers.html#browsing-context-group-set
    OrderedHashTable<GC::Ref<BrowsingContext>> m_browsing_context_set;

    GC::Ref<Page> m_page;
};

}
