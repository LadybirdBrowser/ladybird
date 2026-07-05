/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/SiteIsolationManager.h>

#include <AK/StringBuilder.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/SiteIsolation.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

SiteIsolationManager& SiteIsolationManager::the()
{
    static auto& manager = *new SiteIsolationManager;
    return manager;
}

static URL::URL embedding_page_url_for_child_frame_navigation(CanonicalNavigable const& child_frame, URL::URL const& fallback_url)
{
    if (auto const* parent = child_frame.parent()) {
        if (auto url = parent->document_url(); url.has_value())
            return *url;
    }

    return fallback_url;
}

Web::NavigationProcessDecision SiteIsolationManager::decide_navigation_process(WebContentClient& parent_client, u64 page_id, Optional<String> frame_id, URL::URL current_url, URL::URL target_url, Web::NavigationTarget target)
{
    Optional<CanonicalNavigable&> child_frame;
    if (target == Web::NavigationTarget::IFrame && frame_id.has_value())
        child_frame = parent_client.child_frame(page_id, *frame_id);

    if (child_frame.has_value())
        current_url = embedding_page_url_for_child_frame_navigation(*child_frame, current_url);

    auto decision = WebView::is_url_suitable_for_same_process_navigation(current_url, target_url, target)
        ? Web::NavigationProcessDecision::Local
        : Web::NavigationProcessDecision::Remote;

    if (child_frame.has_value()) {
        auto target_locality = decision == Web::NavigationProcessDecision::Local
            ? CanonicalNavigable::HostLocality::Local
            : CanonicalNavigable::HostLocality::Remote;
        child_frame->record_pending_navigation(target_url, target_locality);

        if (target_locality == CanonicalNavigable::HostLocality::Local)
            transition_child_frame_to_local(*child_frame);
    }

    return decision;
}

Optional<SiteIsolationManager::RemoteChildFrameInputTarget> SiteIsolationManager::remote_child_frame_input_target_at(WebContentClient& client, u64 page_id, Web::DevicePixelPoint position) const
{
    auto* host = client.navigable_for_page(page_id);
    if (!host)
        return {};

    Optional<RemoteChildFrameInputTarget> target;
    host->for_each_in_subtree([&](CanonicalNavigable const& child_frame) {
        if (child_frame.reporting_page_id() != page_id)
            return IterationDecision::Continue;

        auto const& viewport_rect = child_frame.viewport_rect();
        if (!child_frame.has_remote_host() || !viewport_rect.has_value())
            return IterationDecision::Continue;
        if (!viewport_rect->contains(position))
            return IterationDecision::Continue;

        target = RemoteChildFrameInputTarget {
            .remote_client = &child_frame.remote_host_client(),
            .remote_page_id = child_frame.remote_host_page_id(),
            .viewport_rect = *viewport_rect,
        };
        return IterationDecision::Break;
    });

    return target;
}

void SiteIsolationManager::remove_page(WebContentClient& client, u64 page_id)
{
    auto* host = client.navigable_for_page(page_id);
    if (!host)
        return;

    // All children of the hosting navigable are the frames the page reported; any deeper
    // frames belong to their subtrees and follow them out.
    while (!host->children().is_empty())
        remove_child_frame_subtree(*host->children().last());
}

void SiteIsolationManager::remove_all_pages_for_client(WebContentClient& client)
{
    Vector<u64> page_ids;
    page_ids.ensure_capacity(client.m_views.size() + client.m_embedded_pages.size());
    for (auto const& view_entry : client.m_views)
        page_ids.append(view_entry.key);
    for (auto const& embedded_page_entry : client.m_embedded_pages)
        page_ids.append(embedded_page_entry.key);

    for (auto page_id : page_ids)
        remove_page(client, page_id);
}

String SiteIsolationManager::dump_process_tree(WebContentClient& client, u64 page_id) const
{
    StringBuilder builder;
    Vector<WebContentClient const*> processes;

    auto process_index = [&](WebContentClient const& process) -> size_t {
        for (size_t i = 0; i < processes.size(); ++i) {
            if (processes[i] == &process)
                return i;
        }
        processes.append(&process);
        return processes.size() - 1;
    };

    Function<void(CanonicalNavigable const&, size_t)> dump_frame_tree;
    dump_frame_tree = [&](CanonicalNavigable const& parent, size_t depth) {
        for (size_t i = 0; i < parent.children().size(); ++i) {
            auto const& child_frame = *parent.children()[i];

            builder.append_repeated(' ', depth * 2);
            builder.appendff("iframe#{}: {}", i, child_frame.has_remote_host() ? "remote"sv : "local"sv);
            if (child_frame.has_remote_host())
                builder.appendff(" WebContent#{}", process_index(child_frame.remote_host_client()));
            builder.append('\n');

            dump_frame_tree(child_frame, depth + 1);
        }
    };

    builder.appendff("WebContent#{}\n", process_index(client));
    if (auto* host = client.navigable_for_page(page_id))
        dump_frame_tree(*host, 1);
    return builder.to_string_without_validation();
}

HashMap<pid_t, pid_t> SiteIsolationManager::remote_frame_process_embedders() const
{
    HashMap<pid_t, pid_t> embedders;

    WebContentClient::for_each_client([&](WebContentClient& client) {
        for (auto const& embedded_page_entry : client.m_embedded_pages) {
            auto* child_frame = client.embedded_page_host(embedded_page_entry.key);
            if (!child_frame)
                continue;

            embedders.set(client.pid(), child_frame->reporting_client().pid());
        }

        return IterationDecision::Continue;
    });

    return embedders;
}

void SiteIsolationManager::transition_child_frame_to_remote(WebContentClient& parent_client, u64 page_id, StringView frame_id, NonnullRefPtr<WebContentClient> remote_client, u64 remote_page_id)
{
    auto child_frame = parent_client.child_frame(page_id, frame_id);
    if (!child_frame.has_value())
        return;

    transition_child_frame_to_local(*child_frame);

    child_frame->set_remote_host(move(remote_client), remote_page_id);
    parent_client.async_set_remote_child_frame_compositor_context(
        page_id,
        child_frame->id(),
        Web::Compositor::compositor_context_id_for_page(remote_page_id));
}

void SiteIsolationManager::transition_child_frame_to_local(CanonicalNavigable& child_frame)
{
    child_frame.reporting_client().async_set_remote_child_frame_compositor_context(child_frame.reporting_page_id(), child_frame.id(), {});

    // The frames of the closed remote page (this frame's children) die with it; that
    // page's process is going away and will not report their destruction.
    if (child_frame.has_remote_host()) {
        while (!child_frame.children().is_empty())
            remove_child_frame_subtree(*child_frame.children().last());
    }

    child_frame.detach_remote_host();
}

void SiteIsolationManager::remove_child_frame_subtree(CanonicalNavigable& child_frame)
{
    while (!child_frame.children().is_empty())
        remove_child_frame_subtree(*child_frame.children().last());

    if (child_frame.has_remote_host())
        transition_child_frame_to_local(child_frame);

    child_frame.top_level_traversable().remove(child_frame);
}

}
