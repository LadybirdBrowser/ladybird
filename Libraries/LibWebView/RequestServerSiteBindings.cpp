/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibRequests/RequestControlClient.h>
#include <LibURL/Site.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalDocument.h>
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

void RequestServerSiteBindings::did_connect(int client_id)
{
    m_client_id = client_id;
    for (auto const& site : m_sites)
        send(site);
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

bool RequestServerSiteBindings::may_use_cookies_under(Utf16String const& top_level_site, URL::URL const& url) const
{
    // FIXME: The process of the document that starts a navigation makes the navigation's requests. Until the UI process
    //        makes navigation requests itself, a client may use the cookies of a navigation to the request's URL.
    if (URL::Site::serialize_for_partitioning(url.origin()) == top_level_site)
        return true;

    return any_of(m_sites, [&](Site const& site) { return site.top_level_site == top_level_site; });
}

void RequestServerSiteBindings::bind(Site site)
{
    if (m_sites.set(site) == HashSetResult::InsertedNewEntry)
        send(site);
}

void RequestServerSiteBindings::send(Site const& site) const
{
    // NB: A client that has not connected yet, or whose RequestServer has died, is bound to every site when it connects.
    if (!m_client_id.has_value())
        return;
    auto instance = RequestServerManager::the().instance_for_client(*m_client_id);
    if (!instance)
        return;

    (void)instance->control_client().send_sync_but_allow_failure<Messages::RequestServerControl::BindClientToSite>(*m_client_id, site.top_level_site, site.frame_site);
}

}
