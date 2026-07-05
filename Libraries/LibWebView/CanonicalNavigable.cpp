/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/CanonicalNavigable.h>

#include <LibWeb/Page/ViewportIsFullscreen.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

CanonicalNavigable::CanonicalNavigable(String id, String parent_id, RefPtr<WebContentClient> reporting_client, u64 reporting_page_id)
    : m_id(move(id))
    , m_parent_id(move(parent_id))
    , m_reporting_client(move(reporting_client))
    , m_reporting_page_id(reporting_page_id)
{
}

CanonicalNavigable::~CanonicalNavigable() = default;

WebContentClient& CanonicalNavigable::reporting_client() const
{
    VERIFY(m_reporting_client);
    return *m_reporting_client;
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#nav-top
CanonicalTraversable& CanonicalNavigable::top_level_traversable()
{
    // 1. Let navigable be inputNavigable.
    auto* navigable = this;

    // 2. While navigable's parent is not null, set navigable to navigable's parent.
    while (navigable->parent())
        navigable = navigable->parent();

    // 3. Return navigable.
    VERIFY(navigable->is_top_level_traversable());
    return static_cast<CanonicalTraversable&>(*navigable);
}

CanonicalTraversable const& CanonicalNavigable::top_level_traversable() const
{
    return const_cast<CanonicalNavigable&>(*this).top_level_traversable();
}

CanonicalNavigable& CanonicalNavigable::append_child(NonnullOwnPtr<CanonicalNavigable> child)
{
    VERIFY(!child->m_parent);
    child->m_parent = this;
    m_children.append(move(child));
    return *m_children.last();
}

NonnullOwnPtr<CanonicalNavigable> CanonicalNavigable::remove_child(CanonicalNavigable& child)
{
    for (size_t i = 0; i < m_children.size(); ++i) {
        if (m_children[i].ptr() != &child)
            continue;

        auto removed_child = m_children.take(i);
        VERIFY(removed_child->m_parent == this);
        removed_child->m_parent = nullptr;
        return removed_child;
    }

    VERIFY_NOT_REACHED();
}

IterationDecision CanonicalNavigable::for_each_in_inclusive_subtree(Function<IterationDecision(CanonicalNavigable&)> const& callback)
{
    if (callback(*this) == IterationDecision::Break)
        return IterationDecision::Break;

    return for_each_in_subtree(callback);
}

IterationDecision CanonicalNavigable::for_each_in_subtree(Function<IterationDecision(CanonicalNavigable&)> const& callback)
{
    for (auto const& child : m_children) {
        if (child->for_each_in_inclusive_subtree(callback) == IterationDecision::Break)
            return IterationDecision::Break;
    }

    return IterationDecision::Continue;
}

IterationDecision CanonicalNavigable::for_each_in_inclusive_subtree(Function<IterationDecision(CanonicalNavigable const&)> const& callback) const
{
    if (callback(*this) == IterationDecision::Break)
        return IterationDecision::Break;

    return for_each_in_subtree(callback);
}

IterationDecision CanonicalNavigable::for_each_in_subtree(Function<IterationDecision(CanonicalNavigable const&)> const& callback) const
{
    for (auto const& child : m_children) {
        if (child->for_each_in_inclusive_subtree(callback) == IterationDecision::Break)
            return IterationDecision::Break;
    }

    return IterationDecision::Continue;
}

WebContentClient& CanonicalNavigable::remote_host_client() const
{
    VERIFY(m_remote_client);
    return *m_remote_client;
}

void CanonicalNavigable::set_remote_host(NonnullRefPtr<WebContentClient> remote_client, u64 remote_page_id)
{
    detach_remote_host();

    m_host_locality = HostLocality::Remote;
    m_remote_client = move(remote_client);
    m_remote_page_id = remote_page_id;
}

void CanonicalNavigable::detach_remote_host()
{
    if (has_remote_host()) {
        m_remote_client->async_set_page_parent_context(m_remote_page_id, {});
        m_remote_client->request_close(m_remote_page_id);
    }

    m_host_locality = HostLocality::Local;
    m_remote_client = nullptr;
    m_remote_page_id = 0;
}

void CanonicalNavigable::set_viewport(Web::DevicePixelRect viewport_rect, double device_pixel_ratio)
{
    m_viewport_rect = viewport_rect;
    m_device_pixel_ratio = device_pixel_ratio;

    if (has_remote_host()) {
        m_remote_client->async_set_viewport(
            m_remote_page_id,
            viewport_rect.size(),
            device_pixel_ratio,
            Web::ViewportIsFullscreen::No);
    }
}

void CanonicalNavigable::did_commit_navigation(URL::URL url)
{
    m_last_committed_url = move(url);
    m_pending_navigation.clear();
}

Optional<URL::URL> CanonicalNavigable::document_url() const
{
    if (m_last_committed_url.has_value())
        return m_last_committed_url;

    if (m_pending_navigation.has_value())
        return m_pending_navigation->target_url;

    return {};
}

void CanonicalNavigable::record_pending_navigation(URL::URL const& url, HostLocality target_locality, Optional<u64> remote_page_id)
{
    m_pending_navigation = PendingNavigation {
        .target_url = url,
        .target_locality = target_locality,
        .remote_page_id = remote_page_id,
    };
}

bool CanonicalNavigable::has_matching_pending_navigation(URL::URL const& url, HostLocality target_locality) const
{
    return m_pending_navigation.has_value()
        && m_pending_navigation->target_locality == target_locality
        && m_pending_navigation->target_url == url;
}

}
