/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibWeb/HTML/BroadcastChannelMessage.h>
#include <LibWeb/HTML/WorkerAgentTypes.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WorkerProcessManager {
public:
    static WorkerProcessManager& the();

    struct SharedWorkerKey {
        Web::StorageAPI::StorageKey storage_key;
        URL::URL url;
        String name;

        bool operator==(SharedWorkerKey const&) const = default;
    };

    Web::HTML::WorkerAgentId start_worker_agent(WebContentClient&, u64 page_id, Web::HTML::WorkerAgentStartRequest);
    Web::HTML::WorkerAgentId start_worker_agent(WebWorkerClient&, Web::HTML::WorkerAgentStartRequest);

    void close_worker_agent(WebContentClient&, Web::HTML::WorkerAgentId, Web::HTML::WorkerAgentOwnerToken);
    void close_worker_agent(WebWorkerClient&, Web::HTML::WorkerAgentId, Web::HTML::WorkerAgentOwnerToken);
    void remove_web_content_owner(WebContentClient&);
    void remove_web_worker_owner(WebWorkerClient&);

    void broadcast_channel_message_from_web_content(Web::HTML::BroadcastChannelMessage const&);

private:
    friend class WebWorkerClient;

    WorkerProcessManager() = default;

    struct WebContentOwner {
        WeakPtr<WebContentClient> client;
        u64 page_id { 0 };
    };

    struct WebWorkerOwner {
        NonnullRefPtr<WebWorkerClient> client;
    };

    struct Owner {
        Variant<WebContentOwner, WebWorkerOwner> client;
        Web::HTML::WorkerAgentOwnerToken token { 0 };
    };

    Web::HTML::WorkerAgentId start_worker_agent(Owner, Web::HTML::WorkerAgentStartRequest);

    void notify_worker_script_load_success(Owner const&);
    void notify_worker_script_load_failure(Owner const&);
    void notify_worker_exception(Owner const&, String const& message, String const& filename, u32 lineno, u32 colno);
    void notify_worker_close(Owner const&);

    void worker_did_finish_loading_script(Web::HTML::WorkerAgentId, bool worker_is_secure_context);
    void worker_did_fail_loading_script(Web::HTML::WorkerAgentId);
    void worker_did_report_exception(Web::HTML::WorkerAgentId, String message, String filename, u32 lineno, u32 colno);
    void worker_did_close(Web::HTML::WorkerAgentId);
    void worker_did_die(Web::HTML::WorkerAgentId);
    void worker_did_request_file(Web::HTML::WorkerAgentId, ByteString path, i32 request_id);
    void worker_did_post_broadcast_channel_message(Web::HTML::WorkerAgentId, Web::HTML::BroadcastChannelMessage);

    void remove_agent(Web::HTML::WorkerAgentId);
    void remove_owner(Web::HTML::WorkerAgentId, Owner const& identity);

    struct WorkerAgent {
        Web::HTML::WorkerAgentId id { 0 };
        NonnullRefPtr<WebWorkerClient> client;
        Web::Bindings::AgentType agent_type { Web::Bindings::AgentType::DedicatedWorker };
        Web::Bindings::WorkerType worker_type { Web::Bindings::WorkerType::Classic };
        Web::Bindings::RequestCredentials credentials { Web::Bindings::RequestCredentials::SameOrigin };
        bool extended_lifetime { false };
        Optional<bool> worker_is_secure_context;
        bool closing { false };
        Optional<SharedWorkerKey> shared_worker_key;
        Vector<Owner> owners;
    };

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
        return pair_int_hash(pair_int_hash(Traits<Web::StorageAPI::StorageKey>::hash(key.storage_key), Traits<URL::URL>::hash(key.url)), key.name.hash());
    }
};

}
