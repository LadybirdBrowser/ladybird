/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/FaviconStore.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/ProcessSnapshot.h>
#include <LibWebView/ViewImplementation.h>

namespace WebView {

Vector<ProcessSnapshot> process_snapshots(ProcessManager& manager)
{
    // A process that hosts pages of several views is shown with the most relevant one: the active view's, then a
    // visible view's, with ties going to the lower view ID. Within a view, its top-level page wins over its frames.
    struct ProcessPage {
        ViewImplementation* view;
        URL::URL url;
        ProcessPageKind kind;
    };
    HashMap<u64, ProcessPage> process_pages;
    // NB: The Application is only asked once there is a view, so that a ProcessManager without one can be described.
    bool did_look_up_active_view = false;
    Optional<ViewImplementation&> active_view;
    ViewImplementation::for_each_view([&](auto& view) {
        // An owned view's page shares its owner's process, which the owner's tab already lists.
        if (view.owner_view_id().has_value())
            return IterationDecision::Continue;
        if (!did_look_up_active_view) {
            active_view = Application::the().active_web_view();
            did_look_up_active_view = true;
        }
        auto priority = [&](auto const& candidate) {
            if (active_view.has_value() && &*active_view == &candidate)
                return 2;
            return candidate.traversable().system_visibility_state() == Web::HTML::VisibilityState::Visible ? 1 : 0;
        };
        auto add_page = [&](u64 child_id, URL::URL const& url, ProcessPageKind kind) {
            auto current = process_pages.get(child_id);
            if (!current.has_value() || priority(view) > priority(*current->view)
                || (priority(view) == priority(*current->view) && view.view_id() < current->view->view_id())
                || (current->view == &view && kind == ProcessPageKind::TopLevel && current->kind == ProcessPageKind::Frame))
                process_pages.set(child_id, ProcessPage { &view, url, kind });
        };
        add_page(view.page().client().child_id(), view.url(), ProcessPageKind::TopLevel);
        view.traversable().for_each_in_subtree([&](auto const& navigable) {
            if (navigable.has_remote_host() && navigable.replicated_state().has_value())
                add_page(navigable.remote_host().client().child_id(), navigable.replicated_state()->active_document_url, ProcessPageKind::Frame);
            return IterationDecision::Continue;
        });
        return IterationDecision::Continue;
    });

    manager.update_all_process_statistics();
    Vector<ProcessSnapshot> snapshots;
    manager.for_each_process_statistics([&](auto& process, auto const& statistics) {
        ProcessSnapshot snapshot {
            .child_id = process.child_id(),
            .pid = statistics.pid,
            .type = process.type(),
            .title = process.title(),
            .site_url = process.site_url(),
            .owner_child_id = process.owner_child_id(),
            .cpu_percent = statistics.cpu_percent,
            .memory_usage_bytes = statistics.memory_usage_bytes,
            .page = {},
        };
        if (auto page = process_pages.get(process.child_id()); page.has_value()) {
            snapshot.site_url = page->url;
            snapshot.page = ProcessPageSnapshot {
                .view_id = page->view->view_id(),
                .url = page->url,
                .kind = page->kind,
                .favicon_hash = page->view->favicon_hash(),
            };
        }
        snapshots.append(move(snapshot));
    });
    return snapshots;
}

Optional<ByteBuffer> favicon_png_for_page(ProcessPageSnapshot const& page)
{
    if (!page.favicon_hash.has_value())
        return {};
    auto view = ViewImplementation::find_view_by_id(page.view_id);
    if (!view.has_value())
        return {};
    return view->session().favicon_store->favicon_png(*page.favicon_hash);
}

}
