/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/SiteIsolationManager.h>

#include <AK/StringBuilder.h>
#include <LibWeb/Fetch/Infrastructure/URL.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
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

bool SiteIsolationManager::top_level_navigation_requires_process_swap(CanonicalBrowsingContext const& browsing_context, URL::URL const& current_url, URL::URL const& target_url) const
{
    if (site_isolation_mode() == SiteIsolationMode::Disabled)
        return false;

    // Obtaining a browsing context to use for a navigation response only lets an implementation-defined browsing
    // context group switch happen when the group holds a single browsing context (step 8). Ladybird cannot retain
    // WindowProxy relationships across a process swap either, so related top-level browsing contexts share a process.
    auto group = browsing_context.group();
    VERIFY(group);
    if (group->browsing_context_set().size() > 1)
        return false;

    // Allow navigating from about:blank to any site.
    if (Web::HTML::url_matches_about_blank(current_url))
        return false;

    // Make sure JavaScript URLs run in the same process.
    if (target_url.scheme() == "javascript"sv)
        return false;

    // Allow cross-scheme non-HTTP(S) navigation. Disallow cross-scheme HTTP(S) navigation.
    auto current_url_is_http = Web::Fetch::Infrastructure::is_http_or_https_scheme(current_url.scheme());
    auto target_url_is_http = Web::Fetch::Infrastructure::is_http_or_https_scheme(target_url.scheme());
    if (!current_url_is_http || !target_url_is_http)
        return current_url_is_http || target_url_is_http;

    return !current_url.origin().is_same_site(target_url.origin());
}

// Whether a navigable's document is under a local root of a page without crossing a document another page hosts, so
// that its container's position is in the root's coordinates.
static bool is_under_root_in_page(CanonicalNavigable const& root, CanonicalNavigable const& navigable)
{
    for (auto const* ancestor = navigable.parent(); ancestor; ancestor = ancestor->parent()) {
        if (ancestor == &root)
            return true;
        if (ancestor->has_remote_host())
            return false;
    }
    return false;
}

Optional<SiteIsolationManager::RemoteChildFrameInputTarget> SiteIsolationManager::remote_child_frame_input_target_at(WebContentPage const& page, CanonicalNavigable const& root, Web::DevicePixelPoint position) const
{
    Optional<RemoteChildFrameInputTarget> target;
    root.for_each_in_subtree([&](CanonicalNavigable const& child_frame) {
        if (child_frame.reporting_page().ptr() != &page)
            return IterationDecision::Continue;
        if (!is_under_root_in_page(root, child_frame))
            return IterationDecision::Continue;

        auto const& viewport_rect = child_frame.viewport_rect();
        if (!child_frame.has_remote_host() || !viewport_rect.has_value())
            return IterationDecision::Continue;
        if (!viewport_rect->contains(position))
            return IterationDecision::Continue;

        target = RemoteChildFrameInputTarget {
            .remote_page = child_frame.remote_host(),
            .navigable = &child_frame,
            .compositor_context_id = child_frame.replicated_state().has_value() ? child_frame.replicated_state()->compositor_context_id : Optional<Web::Compositor::CompositorContextId> {},
            .viewport_rect = *viewport_rect,
        };
        return IterationDecision::Break;
    });

    return target;
}

void SiteIsolationManager::remove_page(WebContentPage& page)
{
    auto* traversable = page.traversable();
    if (!traversable)
        return;

    if (traversable->is_displaced_document_host(page))
        traversable->forget_displaced_document_host({});
    traversable->forget_opener_page(page);

    Vector<Web::HTML::CrossProcessId> reported_by_page;
    Vector<Web::HTML::CrossProcessId> hosted_by_page;
    Vector<Web::HTML::CrossProcessId> pending_in_page;
    traversable->for_each_in_subtree([&](CanonicalNavigable const& navigable) {
        if (navigable.reporting_page().ptr() == &page)
            reported_by_page.append(navigable.id());
        if (navigable.has_remote_host() && &navigable.remote_host() == &page)
            hosted_by_page.append(navigable.id());
        if (navigable.pending_host_matches(page))
            pending_in_page.append(navigable.id());
        return IterationDecision::Continue;
    });

    for (auto navigable_id : pending_in_page) {
        auto navigable = traversable->find(navigable_id);
        if (!navigable.has_value())
            continue;
        if (navigable_id == traversable->id())
            navigable->clear_pending_host();
        else
            navigable->discard_pending_host();
    }

    for (auto navigable_id : reported_by_page) {
        if (auto navigable = traversable->find(navigable_id); navigable.has_value())
            remove_child_frame_subtree(*navigable);
    }

    for (auto navigable_id : hosted_by_page) {
        if (auto navigable = traversable->find(navigable_id); navigable.has_value())
            transition_child_frame_to_local(*navigable);
    }
}

void SiteIsolationManager::remove_all_pages_for_client(WebContentClient& client)
{
    Vector<NonnullRefPtr<WebContentPage>> pages;
    client.for_each_page([&](WebContentPage& page) {
        pages.append(page);
        return IterationDecision::Continue;
    });
    for (auto const& page : pages)
        remove_page(page);
}

String SiteIsolationManager::dump_process_tree(WebContentClient& client, Web::PageId page_id) const
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
                builder.appendff(" WebContent#{}", process_index(child_frame.remote_host().client()));
            builder.append('\n');

            dump_frame_tree(child_frame, depth + 1);
        }
    };

    builder.appendff("WebContent#{}\n", process_index(client));
    if (auto* host = client.traversable_for_page(page_id))
        dump_frame_tree(*host, 1);
    return builder.to_string_without_validation();
}

HashMap<pid_t, pid_t> SiteIsolationManager::remote_frame_process_embedders() const
{
    HashMap<pid_t, pid_t> embedders;

    WebContentClient::for_each_client([&](WebContentClient& client) {
        client.for_each_page([&](WebContentPage& page) {
            auto* traversable = page.traversable();
            if (!traversable || page.displays_tab())
                return IterationDecision::Continue;

            // The process holding the container of a navigable the page hosts embeds the page.
            traversable->for_each_in_subtree([&](CanonicalNavigable const& navigable) {
                if (!navigable.has_remote_host() || &navigable.remote_host() != &page)
                    return IterationDecision::Continue;
                embedders.set(client.pid(), navigable.reporting_page()->client().pid());
                return IterationDecision::Break;
            });
            return IterationDecision::Continue;
        });

        return IterationDecision::Continue;
    });

    return embedders;
}

// The specification keys the agent cluster of an opaque origin by that origin, so each such document is isolated in
// an agent cluster of its own, and leaves which process hosts an agent cluster to the user agent. Nothing can address
// an opaque origin but the documents it was created from, so its agent cluster is hosted where the agent cluster of
// the navigation's initiator origin is.
void SiteIsolationManager::host_opaque_origin_agent_with_initiator(CanonicalBrowsingContextGroup& group, CanonicalSimilarOriginWindowAgent& agent, URL::Origin const& origin, Optional<URL::Origin> const& initiator_origin)
{
    if (!origin.is_opaque() || agent.hosting_process() || !initiator_origin.has_value())
        return;
    if (auto initiator_host = group.obtain_similar_origin_window_agent(*initiator_origin, false)->hosting_process())
        agent.set_hosting_process_if_unset(*initiator_host);
}

ErrorOr<NonnullRefPtr<WebContentPage>> SiteIsolationManager::obtain_child_document_host(CanonicalNavigable& navigable, CanonicalSimilarOriginWindowAgent& agent)
{
    auto& traversable = navigable.top_level_traversable();
    auto current_step = traversable.session_history().current_step();
    VERIFY(current_step.has_value());
    auto const* current_entry = traversable.session_history().get_the_target_history_entry(navigable, *current_step);
    VERIFY(current_entry);

    // The host takes the navigable's node over once the document it is to display is activated; until then, the page
    // hosting the displayed document keeps it.
    auto host = agent.hosting_process();
    if (host && host == &navigable.reporting_page()->client()) {
        // The page holding the container populates the document in a provisional navigable while another page hosts
        // the displayed document.
        if (navigable.has_remote_host())
            host->async_begin_hosting_navigable(navigable.reporting_page()->id(), navigable.id(), *current_entry, traversable.system_visibility_state());
        navigable.set_pending_host(*navigable.reporting_page());
        return *navigable.reporting_page();
    }
    if (host && navigable.has_remote_host() && host == &navigable.remote_host().client()) {
        navigable.set_pending_host(navigable.remote_host());
        return navigable.remote_host();
    }

    // A process holds one page per tab, with the tab's whole graph: the process displaying the tab hosts a document
    // in the view's page, another process in the page it has for the tab, or in a page created for it.
    Web::PageId page_id;
    if (host && host->page_id_for_traversable(traversable).has_value()) {
        page_id = *host->page_id_for_traversable(traversable);
        host->async_begin_hosting_navigable(page_id, navigable.id(), *current_entry, traversable.system_visibility_state());
    } else if (host) {
        page_id = Application::the().allocate_page_id();
        host->async_create_embedded_page(page_id, traversable.remote_navigable_graph(), navigable.id(), *current_entry, traversable.system_visibility_state());
        host->register_embedded_page(page_id, traversable);
        traversable.represent_openers_in(*host);
    } else {
        auto process = TRY(Application::the().launch_child_frame_web_content_process(navigable.reporting_page()->client().is_private(), traversable.remote_navigable_graph(), navigable.id(), *current_entry));
        host = move(process.client);
        page_id = process.page_id;
        agent.set_hosting_process_if_unset(*host);
        host->register_embedded_page(page_id, traversable);
        traversable.represent_openers_in(*host);
    }

    host->async_update_visibility_state(page_id, navigable.id(), traversable.system_visibility_state());
    NonnullRefPtr page = *host->page(page_id);
    navigable.set_pending_host(page);
    return page;
}

void SiteIsolationManager::set_child_document_host(CanonicalNavigable& navigable, WebContentPage& host)
{
    if (navigable.pending_host_matches(host))
        navigable.clear_pending_host();

    if (navigable.reporting_page().ptr() == &host) {
        if (navigable.has_remote_host())
            transition_child_frame_to_local(navigable);
    } else if (!navigable.has_remote_host() || &navigable.remote_host() != &host) {
        transition_child_frame_to_remote(*navigable.reporting_page(), navigable.id(), host);
    }
}

// A local navigable taking a child's container back starts from a document standing in for the canonical current
// entry's, as the root of an embedded page does.
static Optional<Web::HTML::SessionHistoryEntryDescriptor> current_history_entry_for(CanonicalNavigable& navigable)
{
    auto& traversable = navigable.top_level_traversable();
    auto current_step = traversable.session_history().current_step();
    if (!current_step.has_value())
        return {};
    // NB: The canonical session history can still lack the nested history of a newly created navigable.
    auto const* current_entry = traversable.session_history().get_the_target_history_entry(navigable, *current_step);
    if (!current_entry)
        return {};
    return *current_entry;
}

void SiteIsolationManager::transition_child_frame_to_remote(WebContentPage& parent_page, Web::HTML::CrossProcessId frame_id, NonnullRefPtr<WebContentPage> remote_page)
{
    auto child_frame = parent_page.client().child_frame(parent_page.id(), frame_id);
    if (!child_frame.has_value())
        return;

    detach_child_frame_host(*child_frame);

    child_frame->set_remote_host(move(remote_page));
    // The page holding the container represents the child from its replicated state, which names the compositor
    // context the host paints it through.
    parent_page.client().async_stop_hosting_navigable(parent_page.id(), child_frame->id(), *child_frame->replicated_state());
}

// The child's next document, or none after its host went away, is hosted by the page holding its container.
void SiteIsolationManager::transition_child_frame_to_local(CanonicalNavigable& child_frame)
{
    detach_child_frame_host(child_frame);
    auto current_history_entry = current_history_entry_for(child_frame);
    if (!current_history_entry.has_value())
        return;
    child_frame.reporting_page()->client().async_host_navigable(child_frame.reporting_page()->id(), child_frame.id(), current_history_entry.release_value(), child_frame.top_level_traversable().system_visibility_state());
}

void SiteIsolationManager::detach_child_frame_host(CanonicalNavigable& child_frame)
{
    // The frames of the displaced document, which its host reported, die with it and are not reported destroyed
    // again. The frames of the next document, reported by its host, stay.
    if (child_frame.has_remote_host()) {
        auto const& host = child_frame.remote_host();
        Vector<Web::HTML::CrossProcessId> displaced_frames;
        for (auto const& child : child_frame.children()) {
            if (child->reporting_page().ptr() == &host)
                displaced_frames.append(child->id());
        }
        for (auto frame_id : displaced_frames) {
            if (auto frame = child_frame.top_level_traversable().find(frame_id); frame.has_value())
                remove_child_frame_subtree(*frame);
        }
    }

    child_frame.detach_remote_host();
}

void SiteIsolationManager::remove_child_frame_subtree(CanonicalNavigable& child_frame)
{
    while (!child_frame.children().is_empty())
        remove_child_frame_subtree(*child_frame.children().last());

    if (child_frame.has_remote_host())
        detach_child_frame_host(child_frame);

    child_frame.top_level_traversable().remove(child_frame);
}

}
