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
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/kmalloc.h>
#include <LibCore/Forward.h>
#include <LibIPC/Forward.h>
#include <LibIPC/TransportHandle.h>
#include <LibURL/Origin.h>
#include <LibWebCommon/HTML/BroadcastChannelMessage.h>
#include <LibWebCommon/HTML/WorkerAgentTypes.h>
#include <LibWebCommon/Loader/NavigatorCompatibilityMode.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebCommon/Permissions/Permissions.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API WorkerAgentManager {
public:
    AK_ALLOC_WITH_KMALLOC;

    static WorkerAgentManager& the();

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

    // A shared worker that is to run on a thread of its owner's process comes with the transport that thread is to use.
    struct StartedWorkerAgent {
        Web::HTML::WorkerAgentId agent_id { 0 };
        Optional<IPC::TransportHandle> thread_transport;
        bool is_hosted_in_owner_process { false };
    };
    StartedWorkerAgent start_worker_agent(WebContentClient&, Web::PageId page_id, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest);

    void register_dedicated_worker(WebContentClient&, Web::PageId page_id, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest, IPC::TransportHandle worker_transport);
    void register_dedicated_worker(WebWorkerClient&, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest, IPC::TransportHandle worker_transport);
    void update_site_compatibility_data(JsonValue const&);
    // A worker's navigator reports what its owner's page reports, so a changed spoofing choice or language list
    // reaches every live worker too.
    void update_user_agent(String const&);
    void update_navigator_compatibility_mode(Web::NavigatorCompatibilityMode);
    void update_preferred_languages(Vector<String> const&);

    void close_worker_agent(WebContentClient&, Web::HTML::WorkerAgentId, Web::HTML::WorkerAgentOwnerToken);

    void remove_web_content_owner(WebContentClient&);
    void remove_web_content_page_owner(WebContentClient&, Web::PageId);

    // The agent behind the source connection has delivered the message to its own channels. Every other agent gets it
    // over its own connection, wherever it runs.
    void post_broadcast_channel_message(Web::HTML::PostedBroadcastChannelMessage, CanonicalEnvironmentSettingsObject const& source_settings, IPC::ConnectionBase const& source_connection, IsPrivate);
    void for_each_request_server_site_bindings(Function<IterationDecision(RequestServerSiteBindings&)> const&);
    ErrorOr<void> simulate_request_server_connection_loss_for_testing(WebContentClient&, Web::PageId page_id);

    // Those of a private session that is over, whose workers only their extended lifetime keeps, are not to meet the next
    // one's documents.
    void remove_private_workers_without_owners();

    // A process goes on without pages for as long as it runs a shared worker, as the worker's owners can be anywhere.
    bool hosts_shared_worker(WebContentClient const&) const;

    size_t client_count() const { return m_agents.size(); }

    void set_extended_lifetime_shared_worker_timeout_for_testing(AK::Duration timeout) { m_extended_lifetime_shared_worker_timeout = timeout; }

    void update_permission_snapshot(PermissionManager const&);

private:
    friend class WebWorkerClient;

    WorkerAgentManager() = default;

    struct Owner {
        WeakPtr<WebContentClient> client;
        Web::PageId page_id { 0 };
        Web::HTML::WorkerAgentOwnerToken token { 0 };
    };

    struct DedicatedWorkerOwner {
        IsPrivate is_private { IsPrivate::No };
        WebContentClient& process;
        RequestServerSiteBindings const& request_server_site_bindings;
        bool may_read_local_files { false };
        bool has_cross_site_ancestor { false };
    };
    void register_dedicated_worker(DedicatedWorkerOwner const&, CanonicalEnvironmentSettingsObject const& outside_settings, Web::HTML::WorkerAgentStartRequest, IPC::TransportHandle worker_transport);

    static bool owner_process_may_host_shared_worker(Owner const&, CanonicalEnvironmentSettingsObject const& outside_settings);
    static bool page_environment_has_cross_site_ancestor(WebContentClient&, Web::PageId, CanonicalEnvironmentSettingsObject const&);
    bool has_reached_worker_limit(IsPrivate, CanonicalEnvironmentSettingsObject const& outside_settings) const;
    ErrorOr<NonnullRefPtr<WebWorkerClient>> create_worker_client(NonnullOwnPtr<IPC::Transport>, IsPrivate, Web::HTML::WorkerAgentId, WebContentClient& host, RequestServerSiteBindings const& owner_request_server_site_bindings);
    static void start_worker(WebWorkerClient&, Web::HTML::WorkerAgentStartRequest, CanonicalEnvironmentSettingsObject const& outside_settings, bool has_cross_site_ancestor, bool owner_may_read_local_files, bool has_process_to_itself);

    void notify_worker_script_load_failure(Owner const&);
    void notify_worker_content_security_policy_violation(Owner const&, u64 policy_index, String const& effective_directive, URL::URL const& resource);
    void notify_worker_close(Owner const&);
    void notify_worker_death(Owner const&);

    void worker_did_finish_loading_script(Web::HTML::WorkerAgentId, bool worker_is_secure_context);
    void worker_did_fail_loading_script(Web::HTML::WorkerAgentId);
    void worker_did_report_outside_settings_content_security_policy_violation(Web::HTML::WorkerAgentId, u64 policy_index, String effective_directive, URL::URL resource);
    void worker_did_close(Web::HTML::WorkerAgentId);
    void shared_worker_did_close(Web::HTML::WorkerAgentId);
    void worker_did_die(Web::HTML::WorkerAgentId);
    void worker_thread_did_exit(WebWorkerClient&);

    enum class AgentRemovalCause {
        OwnerSetEmptied,
        ExtendedLifetimeEnded,
        AgentTerminated,
    };
    void remove_agent(Web::HTML::WorkerAgentId, AgentRemovalCause);
    void remove_owner(Web::HTML::WorkerAgentId, Owner const& identity);

    struct WorkerAgent {
        Web::HTML::WorkerAgentId id { 0 };
        NonnullRefPtr<WebWorkerClient> client;
        Web::HTML::WorkerType worker_type { Web::HTML::WorkerType::Classic };
        Web::HTML::RequestCredentials credentials { Web::HTML::RequestCredentials::SameOrigin };
        bool extended_lifetime { false };
        RefPtr<Core::Timer> extended_lifetime_timer;
        Optional<bool> worker_is_secure_context;
        bool closing { false };
        SharedWorkerKey shared_worker_key;
        Vector<Owner> owners;
        // The owner that started the worker, whose outside settings are the client of the worker's script fetch, until
        // that fetch completes or the owner goes away.
        Optional<Owner> outside_settings_owner;
    };

    static bool is_same_owner(Owner const&, Owner const& identity);
    static void forget_removed_outside_settings_owner(WorkerAgent&);

    void for_each_client_with_inside_settings(Function<IterationDecision(WebWorkerClient&, CanonicalWorkerEnvironmentSettingsObject const&)> const&);

    // A worker counts toward the workers of its top-level site until its thread closes its connection as it ends, also
    // once its agent is gone.
    Vector<NonnullRefPtr<WebWorkerClient>> m_clients;

    Web::HTML::WorkerAgentId m_next_agent_id { 0 };
    HashMap<Web::HTML::WorkerAgentId, WorkerAgent> m_agents;
    HashMap<SharedWorkerKey, Web::HTML::WorkerAgentId> m_shared_workers;

    // https://html.spec.whatwg.org/multipage/workers.html#extended-lifetime-shared-worker-timeout
    AK::Duration m_extended_lifetime_shared_worker_timeout { AK::Duration::from_seconds(30) };
};

}

namespace AK {

template<>
struct Traits<WebView::WorkerAgentManager::SharedWorkerKey> : public DefaultTraits<WebView::WorkerAgentManager::SharedWorkerKey> {
    static unsigned hash(WebView::WorkerAgentManager::SharedWorkerKey const& key)
    {
        auto hash = pair_int_hash(pair_int_hash(pair_int_hash(Traits<Web::StorageAPI::StorageKey>::hash(key.storage_key), Traits<URL::URL>::hash(key.url)), key.name.hash()), static_cast<unsigned>(key.is_private));
        if (key.top_level_site.has_value())
            hash = pair_int_hash(hash, key.top_level_site->hash());
        return pair_int_hash(hash, static_cast<unsigned>(key.has_cross_site_ancestor));
    }
};

}
