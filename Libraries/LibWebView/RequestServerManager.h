/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/Utf16String.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibCore/Promise.h>
#include <LibCore/Timer.h>
#include <LibIPC/TransportHandle.h>
#include <LibRequests/CacheSizes.h>
#include <LibRequests/RequestClient.h>
#include <LibRequests/RequestControlClient.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Forward.h>
#include <RequestServer/SiteBinding.h>

namespace WebView {

// One RequestServer process. The UI process speaks its control endpoint and holds a data client of its own on it, and
// hands out the data clients of the renderers and workers it serves.
class WEBVIEW_API RequestServerInstance
    : public RefCounted<RequestServerInstance>
    , public Weakable<RequestServerInstance> {
public:
    ~RequestServerInstance();

    // The top-level site whose requests this RequestServer makes, or nothing for the one that serves the UI process and
    // processes that have no site yet.
    Optional<Utf16String> const& site() const { return m_site; }
    BrowsingSession& session() const { return *m_session; }
    ByteString const& cache_path() const { return m_cache_path; }

    Requests::RequestControlClient& control_client() { return *m_control_client; }
    Requests::RequestClient& ui_client() { return *m_ui_client; }

    // The IDs of the clients this RequestServer handed out, besides the UI process's own.
    HashTable<int> const& client_ids() const { return m_client_ids; }
    bool has_client(int client_id) const { return m_client_ids.contains(client_id); }

private:
    friend class RequestServerManager;

    RequestServerInstance(NonnullRefPtr<BrowsingSession>, Optional<Utf16String> site, ByteString cache_path);

    NonnullRefPtr<BrowsingSession> m_session;
    Optional<Utf16String> m_site;
    ByteString m_cache_path;

    RefPtr<Requests::RequestControlClient> m_control_client;
    RefPtr<Requests::RequestClient> m_ui_client;
    HashTable<int> m_client_ids;
    RefPtr<Core::Timer> m_idle_timer;
};

struct RequestServerClientConnection;

// The RequestServers of the UI process: one for each top-level site that a process hosts documents of, within each
// browsing session, plus one with no site for the UI process itself and for processes that host no site yet. A site's
// RequestServer caches only that site's responses, in a directory of its own that its sandbox confines it to.
class WEBVIEW_API RequestServerManager {
public:
    AK_ALLOC_WITH_KMALLOC;

    static RequestServerManager& the();

    RequestServerManager();
    ~RequestServerManager();

    // The RequestServer with no site of the session. The default session's is launched with the browser.
    ErrorOr<NonnullRefPtr<RequestServerInstance>> browser_instance(BrowsingSession&);

    // The RequestServer of a site, launched when first asked for. Once there are as many as allowed, further sites share
    // the RequestServer with no site.
    ErrorOr<NonnullRefPtr<RequestServerInstance>> instance_for_site(BrowsingSession&, Utf16String const& top_level_site);

    RefPtr<RequestServerInstance> instance_for_client(int client_id);
    void for_each_instance(Function<IterationDecision(RequestServerInstance&)> const&);

    ErrorOr<RequestServerClientConnection> connect_new_client(RequestServerInstance&, BrowsingSession&, RequestServer::SiteBinding);

    // Moves a process to its site's RequestServer, the first time it is about to host a document of a site.
    void assign_site(WebContentClient&, Utf16String const& top_level_site);

    void disk_cache_settings_changed();
    void dns_settings_changed();

    NonnullRefPtr<Core::Promise<Requests::CacheSizes>> estimate_cache_size_accessed_since(UnixDateTime since);
    NonnullRefPtr<Core::Promise<Empty>> clear_cache(UnixDateTime since);

    bool uses_site_request_servers() const { return m_maximum_site_instances > 0; }

private:
    ErrorOr<NonnullRefPtr<RequestServerInstance>> launch(NonnullRefPtr<BrowsingSession>, Optional<Utf16String> site);
    ErrorOr<void> launch_process(RequestServerInstance&);
    void did_lose_process(RequestServerInstance&);
    void reconnect_clients(RequestServerInstance&);
    void client_disconnected(RequestServerInstance&, int client_id);
    void retire(RequestServerInstance&);
    void retire_idle_instances();

    ByteString sites_directory() const;
    static ByteString cache_path_for_site(ByteString const& sites_directory, Utf16String const& site);
    Vector<ByteString> dormant_site_cache_paths();

    Vector<NonnullRefPtr<RequestServerInstance>> m_instances;
    size_t m_maximum_site_instances { 0 };
};

}
