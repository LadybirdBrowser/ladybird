/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibRequests/RequestControlClient.h>
#include <LibURL/Site.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/HelperProcess.h>
#include <LibWebView/RequestServerManager.h>
#include <LibWebView/RequestServerSiteBindings.h>

namespace WebView {

unsigned RequestServerSiteBindings::SiteTraits::hash(Site const& site)
{
    auto hash = site.top_level_site.hash();
    if (site.frame_site.has_value())
        hash = pair_int_hash(hash, site.frame_site->hash());
    return hash;
}

RequestServerSiteBindings::RequestServerSiteBindings(ConnectionHandler connection_handler)
    : m_connection_handler(move(connection_handler))
{
}

RequestServerSiteBindings::~RequestServerSiteBindings() = default;

ErrorOr<void> RequestServerSiteBindings::connect(BrowsingSession& session)
{
    m_session = session.make_weak_ptr();

    auto instance = TRY(RequestServerManager::the().browser_instance(session));
    auto connection = TRY(connect_client(*instance, {}));
    m_connection_handler(connection, {});

    for (auto const& top_level_site : top_level_sites_to_connect())
        TRY(connect_to_site(top_level_site));
    return {};
}

Optional<int> RequestServerSiteBindings::client_id_for(Optional<Utf16String> const& top_level_site) const
{
    if (!top_level_site.has_value())
        return m_client_id;
    return m_site_client_ids.get(*top_level_site);
}

bool RequestServerSiteBindings::has_client(int client_id) const
{
    if (m_client_id == client_id)
        return true;
    return any_of(m_site_client_ids, [&](auto const& entry) { return entry.value == client_id; });
}

void RequestServerSiteBindings::bind_sites_of(CanonicalDocument const& document)
{
    auto top_level_origin = document.top_level_origin();
    if (!top_level_origin.has_value())
        return;

    // NB: A document whose top-level document has an opaque origin makes its requests without a network isolation key.
    auto top_level_site = URL::Site::serialize_for_partitioning(*top_level_origin);
    if (!top_level_site.has_value())
        return;

    bind({ top_level_site.release_value(), URL::Site::serialize_for_partitioning(document.origin()) });
}

void RequestServerSiteBindings::bind_sites_of(RequestServerSiteBindings const& other)
{
    for (auto const& site : other.m_sites)
        bind(site);
}

bool RequestServerSiteBindings::may_use_cookies_under(int client_id, Utf16String const& top_level_site, URL::URL const& url) const
{
    // A client of a site's RequestServer makes requests under that site alone.
    for (auto const& [client_top_level_site, site_client_id] : m_site_client_ids) {
        if (site_client_id == client_id && client_top_level_site != top_level_site)
            return false;
    }

    // FIXME: The process of the document that starts a navigation makes the navigation's requests. Until the UI process
    //        makes navigation requests itself, a client may use the cookies of a navigation to the request's URL.
    if (URL::Site::serialize_for_partitioning(url.origin()) == top_level_site)
        return true;

    return any_of(m_sites, [&](Site const& site) { return site.top_level_site == top_level_site; });
}

ErrorOr<void> RequestServerSiteBindings::reconnect_lost_clients(RequestServerInstance& instance, HashTable<int> const& lost_client_ids)
{
    // A process whose session is gone has nothing left to fetch for.
    if (!m_session)
        return {};

    if (m_client_id.has_value() && lost_client_ids.contains(*m_client_id)) {
        auto connection = TRY(connect_client(instance, {}));
        m_connection_handler(connection, {});
    }

    for (auto const& top_level_site : top_level_sites()) {
        auto client_id = m_site_client_ids.get(top_level_site);
        if (!client_id.has_value() || !lost_client_ids.contains(*client_id))
            continue;
        auto connection = TRY(connect_client(instance, top_level_site));
        m_connection_handler(connection, top_level_site);
    }
    return {};
}

ErrorOr<RequestServerClientConnection> RequestServerSiteBindings::replace_client(Optional<Utf16String> const& top_level_site, bool navigation_authorized)
{
    if (!client_id_for(top_level_site).has_value() && !navigation_authorized)
        return Error::from_string_literal("No existing client or authorized navigation for this site");

    auto session = m_session.strong_ref();
    if (!session)
        return Error::from_string_literal("The process's browsing session is gone");

    RefPtr<RequestServerInstance> instance;
    if (auto client_id = client_id_for(top_level_site); client_id.has_value())
        instance = RequestServerManager::the().instance_for_client(*client_id);
    if (!instance) {
        instance = top_level_site.has_value()
            ? TRY(RequestServerManager::the().instance_for_site(*session, *top_level_site))
            : TRY(RequestServerManager::the().browser_instance(*session));
    }
    return connect_client(*instance, top_level_site);
}

Vector<Utf16String> RequestServerSiteBindings::top_level_sites() const
{
    Vector<Utf16String> top_level_sites;
    for (auto const& entry : m_site_client_ids)
        top_level_sites.append(entry.key);
    return top_level_sites;
}

Vector<Utf16String> RequestServerSiteBindings::top_level_sites_to_connect() const
{
    HashTable<Utf16String> top_level_sites;
    for (auto const& site : m_sites)
        top_level_sites.set(site.top_level_site);

    Vector<Utf16String> result;
    for (auto const& top_level_site : top_level_sites)
        result.append(top_level_site);
    return result;
}

ErrorOr<RequestServerClientConnection> RequestServerSiteBindings::connect_client(RequestServerInstance& instance, Optional<Utf16String> const& top_level_site)
{
    auto session = m_session.strong_ref();
    VERIFY(session);

    auto connection = TRY(RequestServerManager::the().connect_new_client(instance, *session, RequestServer::SiteBinding::Bound));
    if (top_level_site.has_value()) {
        m_site_client_ids.set(*top_level_site, connection.client_id);
        for (auto const& site : m_sites) {
            if (site.top_level_site == *top_level_site)
                send(site);
        }
    } else {
        m_client_id = connection.client_id;
    }
    return connection;
}

ErrorOr<void> RequestServerSiteBindings::connect_to_site(Utf16String const& top_level_site)
{
    auto session = m_session.strong_ref();
    VERIFY(session);

    auto instance = TRY(RequestServerManager::the().instance_for_site(*session, top_level_site));
    auto connection = TRY(connect_client(*instance, top_level_site));
    m_connection_handler(connection, top_level_site);
    return {};
}

void RequestServerSiteBindings::bind(Site site)
{
    if (m_sites.set(site) != HashSetResult::InsertedNewEntry)
        return;

    // NB: connect() handles sites bound before the process connects.
    if (!m_session || !m_client_id.has_value())
        return;

    if (m_site_client_ids.contains(site.top_level_site)) {
        send(site);
        return;
    }

    if (auto result = connect_to_site(site.top_level_site); result.is_error())
        warnln("Unable to connect a process to the RequestServer for {}: {}", site.top_level_site, result.error());
}

void RequestServerSiteBindings::send(Site const& site) const
{
    // NB: Replacement clients regain their site bindings when they connect.
    auto client_id = m_site_client_ids.get(site.top_level_site);
    if (!client_id.has_value())
        return;
    auto instance = RequestServerManager::the().instance_for_client(*client_id);
    if (!instance)
        return;

    (void)instance->control_client().send_sync_but_allow_failure<Messages::RequestServerControl::BindClientToSite>(*client_id, site.top_level_site, site.frame_site);
}

}
