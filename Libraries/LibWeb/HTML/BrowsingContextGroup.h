/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/WeakPtr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/CrossOrigin/CrossOriginIsolationMode.h>

namespace Web::HTML {

class BrowsingContextGroup final : public JS::Cell {
    GC_CELL(BrowsingContextGroup, JS::Cell);
    GC_DECLARE_ALLOCATOR(BrowsingContextGroup);

public:
    struct BrowsingContextGroupAndDocument {
        GC::Ref<HTML::BrowsingContextGroup> browsing_context;
        GC::Ref<DOM::Document> document;
    };
    static constexpr bool OVERRIDES_FINALIZE = true;

    static BrowsingContextGroupAndDocument create_a_new_browsing_context_group_and_document(GC::Ref<Page>);

    Page& page() { return m_page; }
    Page const& page() const { return m_page; }

    auto& browsing_context_set() { return m_browsing_context_set; }
    auto const& browsing_context_set() const { return m_browsing_context_set; }

    void append(BrowsingContext&);

    // https://html.spec.whatwg.org/multipage/document-sequences.html#bcg-cross-origin-isolation
    CrossOriginIsolationMode cross_origin_isolation_mode() const { return m_cross_origin_isolation_mode; }
    void set_cross_origin_isolation_mode(CrossOriginIsolationMode mode) { m_cross_origin_isolation_mode = mode; }

private:
    explicit BrowsingContextGroup(GC::Ref<Web::Page>);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    // https://html.spec.whatwg.org/multipage/browsers.html#browsing-context-group-set
    OrderedHashTable<GC::Ref<BrowsingContext>> m_browsing_context_set;

    GC::Ref<Page> m_page;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#bcg-cross-origin-isolation
    // A browsing context group has an associated cross-origin isolation mode, which is a cross-origin isolation mode.
    // It is initially "none".
    CrossOriginIsolationMode m_cross_origin_isolation_mode { CrossOriginIsolationMode::None };
};

}
