/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibRequests/RequestControlClient.h>
#include <LibURL/Site.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalDocument.h>
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
    auto& browsing_context = document.browsing_context();
    auto& top_level_browsing_context = browsing_context.top_level_browsing_context();

    // A top-level document is not yet its browsing context's active document when it is placed in a process.
    Optional<URL::Origin> top_level_origin;
    if (&top_level_browsing_context == &browsing_context)
        top_level_origin = document.origin();
    else if (!top_level_browsing_context.has_been_discarded())
        top_level_origin = top_level_browsing_context.active_document()->origin();
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

void RequestServerSiteBindings::bind(Site site)
{
    if (m_sites.set(site) == HashSetResult::InsertedNewEntry)
        send(site);
}

void RequestServerSiteBindings::send(Site const& site) const
{
    // NB: A client that has not connected yet, or whose RequestServer has died, is bound to every site when it connects.
    if (!m_client_id.has_value() || !Application::has_request_server_control_client())
        return;

    (void)Application::request_server_control_client().send_sync_but_allow_failure<Messages::RequestServerControl::BindClientToSite>(*m_client_id, site.top_level_site, site.frame_site);
}

}
