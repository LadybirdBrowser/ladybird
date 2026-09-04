/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Variant.h>
#include <AK/WeakPtr.h>
#include <LibURL/Origin.h>
#include <LibURL/Site.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// https://html.spec.whatwg.org/multipage/webappapis.html#similar-origin-window-agent
class WEBVIEW_API CanonicalSimilarOriginWindowAgent final : public RefCounted<CanonicalSimilarOriginWindowAgent> {
public:
    static NonnullRefPtr<CanonicalSimilarOriginWindowAgent> create();

    RefPtr<WebContentClient> hosting_process() const;
    void set_hosting_process_if_unset(WebContentClient&);

private:
    CanonicalSimilarOriginWindowAgent() = default;

    WeakPtr<WebContentClient> m_hosting_process;
};

// https://html.spec.whatwg.org/multipage/document-sequences.html#browsing-context-group
class WEBVIEW_API CanonicalBrowsingContextGroup : public RefCounted<CanonicalBrowsingContextGroup> {
public:
    static NonnullRefPtr<CanonicalBrowsingContextGroup> create();

    // https://html.spec.whatwg.org/multipage/document-sequences.html#browsing-context-set
    OrderedHashTable<CanonicalBrowsingContext*> const& browsing_context_set() const { return m_browsing_context_set; }

    void append(CanonicalBrowsingContext&);
    void remove(CanonicalBrowsingContext&);

    // https://html.spec.whatwg.org/multipage/webappapis.html#obtain-similar-origin-window-agent
    NonnullRefPtr<CanonicalSimilarOriginWindowAgent> obtain_similar_origin_window_agent(URL::Origin const&, bool requests_oac);

private:
    CanonicalBrowsingContextGroup() = default;

    // https://html.spec.whatwg.org/multipage/webappapis.html#agent-cluster-key
    // An agent cluster key is equivalently a scheme-and-host or an origin. URL::Site wraps both cases, so normalize
    // an opaque site to its origin to keep the alternatives disjoint.
    struct AgentClusterKey {
        bool operator==(AgentClusterKey const&) const = default;

        Variant<URL::Site, URL::Origin> value;
    };

    struct AgentClusterKeyTraits : public DefaultTraits<AgentClusterKey> {
        static unsigned hash(AgentClusterKey const&);
        static bool equals(AgentClusterKey const& a, AgentClusterKey const& b) { return a == b; }
        static constexpr bool may_have_slow_equality_check() { return true; }
    };

    struct AgentCluster {
        NonnullRefPtr<CanonicalSimilarOriginWindowAgent> similar_origin_window_agent;
    };

    // Browsing contexts own their group, so this inverse membership relation must remain non-owning.
    OrderedHashTable<CanonicalBrowsingContext*> m_browsing_context_set;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#agent-cluster-map
    // FIXME: Make this weak once canonical agent clusters have a lifetime independent of the browsing context group.
    HashMap<AgentClusterKey, AgentCluster, AgentClusterKeyTraits> m_agent_cluster_map;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#historical-agent-cluster-key-map
    HashMap<URL::Origin, AgentClusterKey> m_historical_agent_cluster_key_map;
};

}
