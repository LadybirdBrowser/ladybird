/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/WeakPtr.h>
#include <LibURL/Forward.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct RequestServerClientConnection;

// One process's RequestServer clients and site bindings. Each top-level site's client is bound to the sites of
// documents hosted under it; other requests use the server with no site through a client bound to no site.
// RequestServer rejects other sites' network isolation keys, preventing a compromised process from reaching
// their cache entries and restricting each site's RequestServer to that top-level site's requests.
//
// NB: Bindings last for the life of the process and are never revoked. A process that has hosted a site's document may
//     have kept anything that document could reach, so leaving the site grants it nothing it lacked.
class RequestServerSiteBindings {
    AK_MAKE_NONCOPYABLE(RequestServerSiteBindings);
    AK_MAKE_NONMOVABLE(RequestServerSiteBindings);

public:
    // Hands the process a new client, for the RequestServer of the given top-level site or the one with no site.
    using ConnectionHandler = Function<void(RequestServerClientConnection&, Optional<Utf16String> const& top_level_site)>;

    explicit RequestServerSiteBindings(ConnectionHandler);
    ~RequestServerSiteBindings();

    // Connects to the server with no site and the servers of all bound top-level sites.
    // The session supplies cookies and must be the process's session.
    ErrorOr<void> connect(BrowsingSession&);

    // The process's client of the RequestServer of a top-level site, or of the one with no site.
    Optional<int> client_id() const { return m_client_id; }
    Optional<int> client_id_for(Optional<Utf16String> const& top_level_site) const;
    bool has_client(int client_id) const;

    // Binds the process to the sites of a document, before it is told to host it.
    void bind_sites_of(CanonicalDocument const&);

    // Binds the process to every site that another process is bound to, as for a worker that process starts.
    void bind_sites_of(RequestServerSiteBindings const&);

    // Whether the client may use the cookies of top_level_site's partition for a request for url.
    bool may_use_cookies_under(int client_id, Utf16String const& top_level_site, URL::URL const& url) const;

    ErrorOr<void> reconnect_lost_clients(RequestServerInstance&, HashTable<int> const& lost_client_ids);

    // Replaces the client for top_level_site (empty for the server with no site).
    // A new site requires navigation authorization authenticated by the UI process.
    // Returns the connection without calling the connection handler.
    ErrorOr<RequestServerClientConnection> replace_client(Optional<Utf16String> const& top_level_site, bool navigation_authorized = false);

    // Sites with clients, excluding the server with no site.
    Vector<Utf16String> top_level_sites() const;

private:
    struct Site {
        Utf16String top_level_site;
        Optional<Utf16String> frame_site;

        bool operator==(Site const&) const = default;
    };
    struct SiteTraits : public DefaultTraits<Site> {
        static unsigned hash(Site const&);
    };

    ErrorOr<RequestServerClientConnection> connect_client(RequestServerInstance&, Optional<Utf16String> const& top_level_site);
    ErrorOr<void> connect_to_site(Utf16String const& top_level_site);
    Vector<Utf16String> top_level_sites_to_connect() const;
    void bind(Site);
    void send(Site const&) const;

    ConnectionHandler m_connection_handler;
    WeakPtr<BrowsingSession> m_session;
    Optional<int> m_client_id;
    HashMap<Utf16String, int> m_site_client_ids;
    HashTable<Site, SiteTraits> m_sites;
};

}
