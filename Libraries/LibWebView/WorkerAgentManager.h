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

    // A worker that is to run on a thread of its owner's process comes with the transport that thread is to use.
    struct StartedWorkerAgent {
        Web::HTML::WorkerAgentId agent_id { 0 };
        Optional<IPC::TransportHandle> thread_transport;
        bool is_hosted_in_owner_process { false };
    };
    StartedWorkerAgent start_worker_agent(WebContentClient&, Web::PageId page_id, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest);
    StartedWorkerAgent start_worker_agent(WebWorkerClient&, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest);
    Optional<CanonicalWorkerEnvironmentSettingsObject const&> inside_settings(Web::HTML::WorkerAgentId) const;
    void update_site_compatibility_data(JsonValue const&);
    // A worker's navigator reports what its owner's page reports, so a changed spoofing choice or language list
    // reaches every live worker too.
    void update_user_agent(String const&);
    void update_navigator_compatibility_mode(Web::NavigatorCompatibilityMode);
    void update_preferred_languages(Vector<String> const&);

    void close_worker_agent(WebContentClient&, Web::HTML::WorkerAgentId, Web::HTML::WorkerAgentOwnerToken);
    void close_worker_agent(WebWorkerClient&, Web::HTML::WorkerAgentId, Web::HTML::WorkerAgentOwnerToken);

    void rtc_transform_init(WebContentClient&, Web::HTML::WorkerAgentId, u64 transform_id, Web::HTML::SerializedTransferRecord options_record);
    void rtc_transform_teardown(WebContentClient&, Web::HTML::WorkerAgentId, u64 transform_id);
    void rtc_transform_encoded_audio_frame(WebContentClient&, Web::HTML::WorkerAgentId, u64 transform_id, ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number);
    void remove_web_content_owner(WebContentClient&);
    void remove_web_content_page_owner(WebContentClient&, Web::PageId);
    void remove_web_worker_owner(WebWorkerClient&);

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
    Optional<URL::Origin> top_level_origin(Web::HTML::WorkerAgentId) const;

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

    WorkerAgentManager() = default;

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

    StartedWorkerAgent start_worker_agent(Owner, Optional<CanonicalEnvironmentSettingsObject const&> outside_settings, Web::HTML::WorkerAgentStartRequest, IsPrivate);
    static bool owner_process_may_host_shared_worker(WebContentOwner const&, CanonicalEnvironmentSettingsObject const& outside_settings);

    void notify_worker_script_load_failure(Owner const&);
    void notify_worker_exception(Owner const&, Utf16String const& message, Utf16String const& filename, u32 lineno, u32 colno);
    void notify_worker_content_security_policy_violation(Owner const&, u64 policy_index, String const& effective_directive, URL::URL const& resource);
    void notify_worker_close(Owner const&);
    void notify_worker_death(Owner const&);

    void worker_did_finish_loading_script(Web::HTML::WorkerAgentId, bool worker_is_secure_context);
    void worker_did_fail_loading_script(Web::HTML::WorkerAgentId);
    void worker_did_report_exception(Web::HTML::WorkerAgentId, Utf16String message, Utf16String filename, u32 lineno, u32 colno);
    void worker_did_report_outside_settings_content_security_policy_violation(Web::HTML::WorkerAgentId, u64 policy_index, String effective_directive, URL::URL resource);
    void worker_did_close(Web::HTML::WorkerAgentId);
    void worker_did_die(Web::HTML::WorkerAgentId);
    void worker_thread_did_exit(WebWorkerClient&);
    void worker_did_request_file(Web::HTML::WorkerAgentId, ByteString path, i32 request_id);
    void worker_did_write_rtc_transform_encoded_audio_frame(Web::HTML::WorkerAgentId, u64 transform_id, ByteBuffer payload, u32 ssrc, u8 payload_type, u32 rtp_timestamp, u16 sequence_number);

    struct WorkerAgent;
    WorkerAgent* agent_owned_by(WebContentClient&, Web::HTML::WorkerAgentId);

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
        Web::HTML::AgentType agent_type { Web::HTML::AgentType::DedicatedWorker };
        Web::HTML::WorkerType worker_type { Web::HTML::WorkerType::Classic };
        Web::HTML::RequestCredentials credentials { Web::HTML::RequestCredentials::SameOrigin };
        bool extended_lifetime { false };
        RefPtr<Core::Timer> extended_lifetime_timer;
        Optional<bool> worker_is_secure_context;
        bool closing { false };
        IsPrivate is_private { IsPrivate::No };
        Optional<URL::Origin> top_level_origin;
        Optional<SharedWorkerKey> shared_worker_key;
        Vector<Owner> owners;
        // The owner that started the worker, whose outside settings are the client of the worker's script fetch, until
        // that fetch completes or the owner goes away.
        Optional<Owner> outside_settings_owner;
        NonnullOwnPtr<CanonicalWorkerEnvironmentSettingsObject> inside_settings;
        bool may_read_local_files { false };
        // Keeps the blob URL entry of the worker's script alive until the script is loaded, so that the owner can
        // revoke the URL right after constructing the worker.
        BlobURLHandle script_blob_url;
    };

    static bool is_same_owner(Owner const&, Owner const& identity);
    static void forget_removed_outside_settings_owner(WorkerAgent&);

    // A worker whose agent is gone counts toward the workers of its top-level site until its thread closes its connection
    // as it ends.
    struct ExitingWorker {
        NonnullRefPtr<WebWorkerClient> client;
        IsPrivate is_private { IsPrivate::No };
        Optional<URL::Origin> top_level_origin;
    };
    Vector<ExitingWorker> m_exiting_workers;

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
