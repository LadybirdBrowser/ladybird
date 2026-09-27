/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <AK/Random.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

NonnullRefPtr<CanonicalSimilarOriginWindowAgent> CanonicalSimilarOriginWindowAgent::create()
{
    return adopt_ref(*new CanonicalSimilarOriginWindowAgent);
}

// Random, so that it cannot collide with the id a WebContent process gives a shared worker's agent cluster.
CanonicalSimilarOriginWindowAgent::CanonicalSimilarOriginWindowAgent()
    : m_agent_cluster_id(get_random<u64>())
{
}

RefPtr<WebContentClient> CanonicalSimilarOriginWindowAgent::hosting_process() const
{
    // A process that has exited, or has been asked to, hosts nothing.
    auto process = m_hosting_process.strong_ref();
    if (!process || !process->is_open() || process->has_requested_close())
        return nullptr;
    return process;
}

void CanonicalSimilarOriginWindowAgent::set_hosting_process_if_unset(WebContentClient& process)
{
    if (hosting_process())
        return;
    m_hosting_process = process.make_weak_ptr<WebContentClient>();
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#browsing-context-group-set
struct BrowsingContextGroupSet {
    Vector<NonnullRefPtr<CanonicalBrowsingContextGroup>> groups;
    u64 next_group_id { 1 };
};

static BrowsingContextGroupSet& user_agent_browsing_context_group_set()
{
    static NeverDestroyed<BrowsingContextGroupSet> set;
    return *set;
}

NonnullRefPtr<CanonicalBrowsingContextGroup> CanonicalBrowsingContextGroup::create()
{
    return adopt_ref(*new CanonicalBrowsingContextGroup);
}

void CanonicalBrowsingContextGroup::append_to_user_agent_browsing_context_group_set(CanonicalBrowsingContextGroup& group)
{
    auto& set = user_agent_browsing_context_group_set();
    group.m_id = set.next_group_id++;
    set.groups.append(group);
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#bcg-append
void CanonicalBrowsingContextGroup::append(CanonicalBrowsingContext& browsing_context)
{
    VERIFY(!browsing_context.group());

    // 1. Append browsingContext to group's browsing context set.
    m_browsing_context_set.set(&browsing_context);

    // 2. Set browsingContext's group to group.
    browsing_context.set_group({}, this);
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#bcg-remove
void CanonicalBrowsingContextGroup::remove(CanonicalBrowsingContext& browsing_context)
{
    // Keep group alive while step 3 releases browsingContext's reference to it.
    NonnullRefPtr protected_this = *this;

    // 1. Assert: browsingContext's group is non-null.
    // 2. Let group be browsingContext's group.
    VERIFY(browsing_context.group() == this);

    // 3. Set browsingContext's group to null.
    browsing_context.set_group({}, nullptr);

    // 4. Remove browsingContext from group's browsing context set.
    m_browsing_context_set.remove(&browsing_context);

    // 5. If group's browsing context set is empty, then remove group from the user agent's browsing context group set.
    if (m_browsing_context_set.is_empty())
        user_agent_browsing_context_group_set().groups.remove_first_matching([&](auto const& group) { return group.ptr() == this; });
}

unsigned CanonicalBrowsingContextGroup::AgentClusterKeyTraits::hash(AgentClusterKey const& key)
{
    auto value_hash = key.value.visit(
        [](URL::Site const& site) { return site.serialize().hash(); },
        [](URL::Origin const& origin) { return Traits<URL::Origin>::hash(origin); });
    return pair_int_hash(key.value.index(), value_hash);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#obtain-similar-origin-window-agent
NonnullRefPtr<CanonicalSimilarOriginWindowAgent> CanonicalBrowsingContextGroup::obtain_similar_origin_window_agent(URL::Origin const& origin, bool requests_oac)
{
    // 1. Let site be the result of obtaining a site with origin.
    auto site = URL::Site::obtain(origin);

    // 2. Let key be site.
    // NB: A site is an opaque origin or a scheme-and-host. URL::Site wraps both, so normalize an opaque site to its
    //     origin to preserve which arm of the agent cluster key union it occupies.
    auto key = origin.is_opaque() ? AgentClusterKey { origin } : AgentClusterKey { move(site) };

    // 3. If group's cross-origin isolation mode is not "none", then set key to origin.
    if (m_cross_origin_isolation_mode != CrossOriginIsolationMode::None) {
        key = AgentClusterKey { origin };
    }
    // 4. Otherwise, if group's historical agent cluster key map[origin] exists, then set key to group's historical
    //    agent cluster key map[origin].
    else if (auto historical_key = m_historical_agent_cluster_key_map.get(origin); historical_key.has_value()) {
        key = historical_key.release_value();
    }
    // 5. Otherwise:
    else {
        // 1. If requestsOAC is true, then set key to origin.
        if (requests_oac)
            key = AgentClusterKey { origin };

        // 2. Set group's historical agent cluster key map[origin] to key.
        m_historical_agent_cluster_key_map.set(origin, key);
    }

    // 6. If group's agent cluster map[key] does not exist:
    if (!m_agent_cluster_map.contains(key)) {
        // 1. Let agentCluster be a new agent cluster.
        // 2. Let agent be a new similar-origin window agent.
        // 3. Set agentCluster's agents to « agent ».
        AgentCluster agent_cluster { CanonicalSimilarOriginWindowAgent::create() };

        // 4. Set group's agent cluster map[key] to agentCluster.
        m_agent_cluster_map.set(key, move(agent_cluster));
    }

    // 7. Return the single similar-origin window agent contained in group's agent cluster map[key].
    return m_agent_cluster_map.get(key)->similar_origin_window_agent;
}

RefPtr<CanonicalSimilarOriginWindowAgent> CanonicalBrowsingContextGroup::similar_origin_window_agent_for(URL::Origin const& origin) const
{
    auto key = origin.is_opaque() ? AgentClusterKey { origin } : AgentClusterKey { URL::Site::obtain(origin) };
    if (m_cross_origin_isolation_mode != CrossOriginIsolationMode::None)
        key = AgentClusterKey { origin };
    else if (auto historical_key = m_historical_agent_cluster_key_map.get(origin); historical_key.has_value())
        key = *historical_key;
    auto agent_cluster = m_agent_cluster_map.get(key);
    if (!agent_cluster.has_value())
        return nullptr;
    return agent_cluster->similar_origin_window_agent;
}

}
