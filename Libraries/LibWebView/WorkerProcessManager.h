/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonValue.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/kmalloc.h>
#include <LibWebCommon/HTML/BroadcastChannelMessage.h>
#include <LibWebCommon/HTML/WorkerAgentTypes.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WorkerProcessManager {
public:
    AK_ALLOC_WITH_KMALLOC;

    static WorkerProcessManager& the();

    struct SharedWorkerKey {
        IsPrivate is_private { IsPrivate::No };
        Web::StorageAPI::StorageKey storage_key;
        URL::URL url;
        Utf16String name;

        // AD-HOC: A shared worker uses the network state partition of the top-level site it was created under, and of
        //         whether its creator had a cross-site ancestor, so owners in different partitions never share one.
        Optional<Utf16String> top_level_site;
        bool has_cross_site_ancestor { false };

        bool operator==(SharedWorkerKey const&) const = default;
    };

    Web::HTML::WorkerAgentId start_worker_agent(WebContentClient&, Web::PageId page_id, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest);
    Web::HTML::WorkerAgentId start_worker_agent(WebWorkerClient&, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest);
    Optional<CanonicalWorkerEnvironmentSettingsObject const&> inside_settings(Web::HTML::WorkerAgentId) const;
    void update_site_compatibility_data(JsonValue const&);

    void close_worker_agent(WebContentClient&, Web::HTML::WorkerAgentId, Web::HTML::WorkerAgentOwnerToken);
    void close_worker_agent(WebWorkerClient&, Web::HTML::WorkerAgentId, Web::HTML::WorkerAgentOwnerToken);
    void remove_web_content_owner(WebContentClient&);
    void remove_web_worker_owner(WebWorkerClient&);

    void post_broadcast_channel_message(Web::HTML::PostedBroadcastChannelMessage, CanonicalEnvironmentSettingsObject const& source_settings, pid_t source_process_id, IsPrivate);
    ErrorOr<void> reconnect_to_request_server();
    void for_each_request_server_site_bindings(Function<IterationDecision(RequestServerSiteBindings&)> const&);
    ErrorOr<void> simulate_request_server_connection_loss_for_testing(WebContentClient&, Web::PageId page_id);

    Optional<u64> exclusive_performance_owner(pid_t) const;

    size_t client_count() const { return m_agents.size(); }

    template<CallableAs<IterationDecision, WebWorkerClient&> Callback>
    void for_each_client(Callback callback)
    {
        for (auto& agent : m_agents) {
            if (callback(*agent.value.client) == IterationDecision::Break)
                break;
        }
    }

private:
    friend class WebWorkerClient;

    WorkerProcessManager() = default;

    struct WebContentOwner {
        WeakPtr<WebContentClient> client;
        Web::PageId page_id { 0 };
    };

    struct WebWorkerOwner {
        NonnullRefPtr<WebWorkerClient> client;
    };

    struct Owner {
        Variant<WebContentOwner, WebWorkerOwner> client;
        Web::HTML::WorkerAgentOwnerToken token { 0 };
    };

    Web::HTML::WorkerAgentId start_worker_agent(Owner, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest, IsPrivate);

    void notify_worker_script_load_failure(Owner const&);
    void notify_worker_exception(Owner const&, Utf16String const& message, Utf16String const& filename, u32 lineno, u32 colno);
    void notify_worker_close(Owner const&);
    void notify_worker_death(Owner const&);

    void worker_did_finish_loading_script(Web::HTML::WorkerAgentId, bool worker_is_secure_context);
    void worker_did_fail_loading_script(Web::HTML::WorkerAgentId);
    void worker_did_report_exception(Web::HTML::WorkerAgentId, Utf16String message, Utf16String filename, u32 lineno, u32 colno);
    void worker_did_close(Web::HTML::WorkerAgentId);
    void worker_did_die(Web::HTML::WorkerAgentId);
    void worker_did_request_file(Web::HTML::WorkerAgentId, ByteString path, i32 request_id);

    enum class AgentRemovalCause {
        OwnerSetEmptied,
        AgentTerminated,
    };
    void remove_agent(Web::HTML::WorkerAgentId, AgentRemovalCause);
    void remove_owner(Web::HTML::WorkerAgentId, Owner const& identity);

    struct WorkerAgent {
        Web::HTML::WorkerAgentId id { 0 };
        NonnullRefPtr<WebWorkerClient> client;
        Web::HTML::AgentType agent_type { Web::HTML::AgentType::DedicatedWorker };
        Web::HTML::WorkerType worker_type { Web::HTML::WorkerType::Classic };
        Web::HTML::RequestCredentials credentials { Web::HTML::RequestCredentials::SameOrigin };
        bool extended_lifetime { false };
        Optional<bool> worker_is_secure_context;
        bool closing { false };
        IsPrivate is_private { IsPrivate::No };
        Optional<SharedWorkerKey> shared_worker_key;
        Vector<Owner> owners;
        NonnullOwnPtr<CanonicalWorkerEnvironmentSettingsObject> inside_settings;
        bool may_read_local_files { false };
    };

    ErrorOr<void> reconnect_to_request_server(Function<bool(WorkerAgent const&)> should_reconnect);

    Web::HTML::WorkerAgentId m_next_agent_id { 0 };
    HashMap<Web::HTML::WorkerAgentId, WorkerAgent> m_agents;
    HashMap<SharedWorkerKey, Web::HTML::WorkerAgentId> m_shared_workers;
};

}

namespace AK {

template<>
struct Traits<WebView::WorkerProcessManager::SharedWorkerKey> : public DefaultTraits<WebView::WorkerProcessManager::SharedWorkerKey> {
    static unsigned hash(WebView::WorkerProcessManager::SharedWorkerKey const& key)
    {
        auto hash = pair_int_hash(pair_int_hash(pair_int_hash(Traits<Web::StorageAPI::StorageKey>::hash(key.storage_key), Traits<URL::URL>::hash(key.url)), key.name.hash()), static_cast<unsigned>(key.is_private));
        if (key.top_level_site.has_value())
            hash = pair_int_hash(hash, key.top_level_site->hash());
        return pair_int_hash(hash, static_cast<unsigned>(key.has_cross_site_ancestor));
    }
};

}
