/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/SiteIsolationManager.h>

#include <AK/StringBuilder.h>
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

void SiteIsolationManager::remove_page(WebContentPage& page)
{
    auto& traversable = page.traversable();
    traversable.forget_opener_page(page);

    Vector<Web::HTML::CrossProcessId> reported_by_page;
    Vector<Web::HTML::CrossProcessId> hosted_by_page;
    Vector<Web::HTML::CrossProcessId> pending_in_page;
    traversable.for_each_in_subtree([&](CanonicalNavigable const& navigable) {
        if (navigable.reporting_page().ptr() == &page)
            reported_by_page.append(navigable.id());
        if (navigable.has_remote_host() && &navigable.remote_host() == &page)
            hosted_by_page.append(navigable.id());
        if (navigable.pending_host_matches(page))
            pending_in_page.append(navigable.id());
        return IterationDecision::Continue;
    });

    for (auto navigable_id : pending_in_page) {
        if (auto navigable = traversable.find(navigable_id); navigable.has_value())
            navigable->discard_pending_host();
    }

    // The documents the page hosted are destroyed, their child navigables first.
    for (auto navigable_id : reported_by_page) {
        if (auto navigable = traversable.find(navigable_id); navigable.has_value())
            remove_child_frame_subtree(*navigable);
    }

    for (auto navigable_id : hosted_by_page) {
        if (auto navigable = traversable.find(navigable_id); navigable.has_value())
            transition_child_frame_to_local(*navigable);
    }

    if (traversable.active_document().host() == &page)
        traversable.active_document().set_host(nullptr);
}

String SiteIsolationManager::dump_process_tree(WebContentClient& client, Compositing::PageId page_id) const
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
    if (auto* page = client.page(page_id))
        dump_frame_tree(page->traversable(), 1);
    return builder.to_string_without_validation();
}

ErrorOr<NonnullRefPtr<WebContentPage>> SiteIsolationManager::obtain_child_document_host(CanonicalNavigable& navigable, CanonicalDocument const& document, Optional<URL::Origin> const& initiator_origin)
{
    auto& traversable = navigable.top_level_traversable();
    // A page beginning to host the navigable starts from a document standing in for the current entry's.
    auto current_entry_descriptor = [&] {
        auto current_step = traversable.session_history().current_step();
        VERIFY(current_step.has_value());
        auto const* current_entry = traversable.session_history().get_the_target_history_entry(navigable, *current_step);
        VERIFY(current_entry);
        return current_entry->descriptor();
    };

    // The host takes the navigable's node over once the document it is to display is activated; until then, the page
    // hosting the displayed document keeps it.
    auto host = navigable.process_to_host(document, initiator_origin);
    if (host && host == &navigable.reporting_page()->client()) {
        // The page holding the container populates the document in a provisional navigable while another page hosts
        // the displayed document.
        if (navigable.has_remote_host())
            host->async_begin_hosting_navigable(navigable.reporting_page()->id(), navigable.id(), current_entry_descriptor(), traversable.system_visibility_state());
        return *navigable.reporting_page();
    }
    if (host && navigable.has_remote_host() && host == &navigable.remote_host().client())
        return navigable.remote_host();

    // A process holds one page per tab, with the tab's whole graph: the process displaying the tab hosts a document
    // in the view's page, another process in the page it has for the tab, or in a page created for it.
    Compositing::PageId page_id;
    if (host && host->page_id_for_traversable(traversable).has_value()) {
        page_id = *host->page_id_for_traversable(traversable);
        host->async_begin_hosting_navigable(page_id, navigable.id(), current_entry_descriptor(), traversable.system_visibility_state());
    } else if (host) {
        page_id = Application::the().allocate_page_id();
        host->async_create_embedded_page(page_id, traversable.remote_navigable_graph(), navigable.id(), current_entry_descriptor(), traversable.system_visibility_state());
        host->register_embedded_page(page_id, traversable);
        traversable.represent_openers_in(*host);
    } else {
        auto process = TRY(Application::the().launch_child_frame_web_content_process(navigable.reporting_page()->client().is_private(), traversable.remote_navigable_graph(), navigable.id(), current_entry_descriptor()));
        host = move(process.client);
        page_id = process.page_id;
        host->register_embedded_page(page_id, traversable);
        traversable.represent_openers_in(*host);
    }

    host->async_update_visibility_state(page_id, navigable.id(), traversable.system_visibility_state());
    return *host->page(page_id);
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
    return current_entry->descriptor();
}

void SiteIsolationManager::transition_child_frame_to_local(CanonicalNavigable& child_frame)
{
    child_frame.hand_pending_webdriver_commands_to(*child_frame.reporting_page());
    child_frame.active_document().set_host(nullptr);
    auto current_history_entry = current_history_entry_for(child_frame);
    if (!current_history_entry.has_value())
        return;
    child_frame.reporting_page()->async_host_navigable(child_frame.id(), current_history_entry.release_value(), child_frame.top_level_traversable().system_visibility_state());
}

void SiteIsolationManager::remove_child_frame_subtree(CanonicalNavigable& child_frame)
{
    while (!child_frame.children().is_empty())
        remove_child_frame_subtree(*child_frame.children().last());

    // The page hosting the navigable's document, when that is not the page holding its container, retires the
    // navigable's node when told, unloading the document if it still displays it; it holds the tab's graph without the
    // navigable from then on, or is released once it hosts nothing of the tab.
    auto& traversable = child_frame.top_level_traversable();
    RefPtr<WebContentPage> host = child_frame.has_remote_host() ? &child_frame.remote_host() : nullptr;
    if (host && host->is_open()) {
        VERIFY(child_frame.replicated_state().has_value());
        host->async_stop_hosting_navigable(child_frame.id(), *child_frame.replicated_state());
    } else {
        host = nullptr;
    }
    traversable.remove(child_frame);
    if (host)
        traversable.release_page_if_unused(host.release_nonnull());
}

}
