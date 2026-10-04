/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibURL/Forward.h>
#include <LibWebView/Forward.h>

namespace WebView {

// The sites that the RequestServer client of one process is bound to. RequestServer refuses a bound client the network
// isolation keys of any other sites, so a compromised process cannot reach another site's cache entries.
//
// NB: Bindings last for the life of the process and are never revoked. A process that has hosted a site's document may
//     have kept anything that document could reach, so leaving the site grants it nothing it lacked.
class RequestServerSiteBindings {
    AK_MAKE_NONCOPYABLE(RequestServerSiteBindings);
    AK_MAKE_NONMOVABLE(RequestServerSiteBindings);

public:
    RequestServerSiteBindings() = default;

    Optional<int> client_id() const { return m_client_id; }

    // The process has a new RequestServer client. It is bound to every site the previous client was bound to.
    void did_connect(int client_id);

    // Binds the client to the sites of a document, before the process is told to host it.
    void bind_sites_of(CanonicalDocument const&);

    // Binds the client to every site that another process's client is bound to, as for a worker that process starts.
    void bind_sites_of(RequestServerSiteBindings const&);

    // Whether the client may use the cookies of top_level_site's partition for a request for url.
    bool may_use_cookies_under(Utf16String const& top_level_site, URL::URL const& url) const;

private:
    struct Site {
        Utf16String top_level_site;
        Optional<Utf16String> frame_site;

        bool operator==(Site const&) const = default;
    };
    struct SiteTraits : public DefaultTraits<Site> {
        static unsigned hash(Site const&);
    };

    void bind(Site);
    void send(Site const&) const;

    Optional<int> m_client_id;
    HashTable<Site, SiteTraits> m_sites;
};

}
