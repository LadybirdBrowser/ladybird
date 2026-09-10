/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/Origin.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/SerializedEnvironmentSettingsObject.h>

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

    // https://html.spec.whatwg.org/multipage/webappapis.html#agent-cluster-map
    // AD-HOC: We don't model agent clusters, only name them: one id per key, so every document that "obtain a
    //         similar-origin window agent" would put in one cluster names the same one. The spec keys a group whose
    //         cross-origin isolation mode isn't "none" by origin, and any other group by site. We track neither, so
    //         every document is keyed by its origin — with the cross-origin isolated ones kept apart from the rest,
    //         since without COOP enforcement one group can hold both kinds of the same origin, and a SharedArrayBuffer
    //         must never reach the non-isolated kind. Keying by origin rather than site only splits documents that
    //         document.domain could make same-origin, and nothing that names a cluster depends on those sharing one.
    u64 agent_cluster_id(URL::Origin const&, CanUseCrossOriginIsolatedAPIs);

private:
    explicit BrowsingContextGroup(GC::Ref<Web::Page>);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    // https://html.spec.whatwg.org/multipage/browsers.html#browsing-context-group-set
    OrderedHashTable<GC::Ref<BrowsingContext>> m_browsing_context_set;

    struct AgentCluster {
        URL::Origin origin;
        CanUseCrossOriginIsolatedAPIs cross_origin_isolated;
        u64 id;
    };
    Vector<AgentCluster> m_agent_clusters;

    GC::Ref<Page> m_page;
};

}
