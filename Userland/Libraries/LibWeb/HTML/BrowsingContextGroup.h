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
    JS_CELL(BrowsingContextGroup, JS::Cell);
    JS_DECLARE_ALLOCATOR(BrowsingContextGroup);

public:
    struct BrowsingContextGroupAndDocument {
        JS::NonnullGCPtr<HTML::BrowsingContextGroup> browsing_context;
        JS::NonnullGCPtr<DOM::Document> document;
    };
    static WebIDL::ExceptionOr<BrowsingContextGroupAndDocument> create_a_new_browsing_context_group_and_document(JS::NonnullGCPtr<Page>);

    ~BrowsingContextGroup();

    Page& page() { return m_page; }
    Page const& page() const { return m_page; }

    auto& browsing_context_set() { return m_browsing_context_set; }
    auto const& browsing_context_set() const { return m_browsing_context_set; }

    void append(BrowsingContext&);

private:
    explicit BrowsingContextGroup(JS::NonnullGCPtr<Web::Page>);

    virtual void visit_edges(Cell::Visitor&) override;

    // https://html.spec.whatwg.org/multipage/browsers.html#browsing-context-group-set
    OrderedHashTable<JS::NonnullGCPtr<BrowsingContext>> m_browsing_context_set;

    JS::NonnullGCPtr<Page> m_page;
};

}
