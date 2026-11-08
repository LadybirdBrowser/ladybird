/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibWebCommon/HTML/SandboxingFlagSet.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalTraversable.h>
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
    HashMap<u64, Vector<ProcessTabSnapshot>> process_tabs;
    HashMap<u64, Vector<ProcessFrameSnapshot>> process_frames;
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
        auto top_level_child_id = view.page().client().child_id();
        add_page(top_level_child_id, view.url(), ProcessPageKind::TopLevel);
        process_tabs.ensure(top_level_child_id).append(ProcessTabSnapshot {
            .title = view.title(),
            .page = {
                .view_id = view.view_id(),
                .url = view.url(),
                .kind = ProcessPageKind::TopLevel,
            },
        });
        view.traversable().for_each_in_subtree([&](auto const& navigable) {
            if (navigable.has_remote_host() && navigable.replicated_state().has_value()) {
                auto child_id = navigable.remote_host().client().child_id();
                add_page(child_id, navigable.replicated_state()->active_document_url, ProcessPageKind::Frame);
                if (child_id != top_level_child_id) {
                    process_frames.ensure(child_id).append({
                        .url = navigable.replicated_state()->active_document_url,
                        .is_sandboxed = has_flag(navigable.active_document().active_sandboxing_flag_set(), Web::HTML::SandboxingFlagSet::SandboxedOrigin),
                    });
                }
            }
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
            .tabs = process_tabs.get(process.child_id()).value_or({}),
            .frames = process_frames.get(process.child_id()).value_or({}),
        };
        if (auto page = process_pages.get(process.child_id()); page.has_value()) {
            snapshot.site_url = page->url;
            snapshot.page = ProcessPageSnapshot {
                .view_id = page->view->view_id(),
                .url = page->url,
                .kind = page->kind,
            };
        }
        snapshots.append(move(snapshot));
    });
    return snapshots;
}

bool end_process(ProcessManager& process_manager, u64 child_id)
{
    auto result = process_manager.terminate_child(child_id, Core::Process::TerminationMode::Forceful);
    return !result.is_error() || (result.error().is_errno() && result.error().code() == ESRCH);
}

}
