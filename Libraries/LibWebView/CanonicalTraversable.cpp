/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibCore/EventLoop.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/SiteIsolationManager.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebContentClient.h>

namespace WebView {

CanonicalTraversable::CanonicalTraversable()
    : CanonicalNavigable({}, {}, nullptr, 0)
{
}

static Web::HTML::CrossProcessId allocate_ui_process_document_state_id()
{
    return Application::the().allocate_ui_process_cross_process_id();
}

CanonicalNavigable& CanonicalTraversable::insert(WebContentClient& reporting_client, u64 page_id, Web::HTML::CrossProcessId parent_frame_id, Web::HTML::CrossProcessId frame_id, CanonicalNavigable& fallback_parent)
{
    if (auto existing_navigable = find(frame_id); existing_navigable.has_value())
        remove(*existing_navigable);

    auto navigable = make<CanonicalNavigable>(frame_id, parent_frame_id, &reporting_client, page_id);

    // A frame's parent frame is always created (and thus reported) before the frame
    // itself, so if the parent is not in the index, the parent is the top-level document
    // of the reporting page: the fallback parent.
    auto* parent = &fallback_parent;
    if (auto indexed_parent = find(parent_frame_id); indexed_parent.has_value())
        parent = &*indexed_parent;

    auto& navigable_ref = parent->append_child(move(navigable));
    m_navigable_index.set(navigable_ref.id(), navigable_ref.make_weak_ptr());
    return navigable_ref;
}

Optional<CanonicalNavigable&> CanonicalTraversable::find(Web::HTML::CrossProcessId navigable_id)
{
    if (id() == navigable_id)
        return *this;

    auto navigable = m_navigable_index.get(navigable_id);
    if (!navigable.has_value() || !navigable.value())
        return {};

    return *navigable.value();
}

Optional<CanonicalNavigable const&> CanonicalTraversable::find(Web::HTML::CrossProcessId navigable_id) const
{
    if (id() == navigable_id)
        return *this;

    auto navigable = m_navigable_index.get(navigable_id);
    if (!navigable.has_value() || !navigable.value())
        return {};

    return *navigable.value();
}

void CanonicalTraversable::remove(CanonicalNavigable& navigable)
{
    VERIFY(&navigable != this);
    remove_from_index(navigable);

    auto* parent = navigable.parent();
    VERIFY(parent);
    (void)parent->remove_child(navigable);
}

void CanonicalTraversable::remove_from_index(CanonicalNavigable& navigable)
{
    navigable.for_each_in_inclusive_subtree([&](CanonicalNavigable& child) {
        m_navigable_index.remove(child.id());
        return IterationDecision::Continue;
    });
}

ProcessSwapNavigationPreparation CanonicalTraversable::prepare_for_process_swap_navigation(URL::URL const& url, Web::HTML::DocumentResource document_resource, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    ProcessSwapNavigationPreparation result;

    auto ui_session_history_already_points_to_url = false;
    if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url)
        ui_session_history_already_points_to_url = true;

    if (m_pending_session_history_navigation.has_value())
        m_pending_session_history_navigation->web_content_restore_mode = PendingSessionHistoryNavigation::WebContentRestoreMode::RestoreFromUIProcess;
    m_current_web_content_session_history_matches_mirror = false;
    m_session_history.forget_web_content_state();

    if (!ui_session_history_already_points_to_url) {
        if (m_session_history.current_entry()) {
            m_pending_session_history_navigation = PendingSessionHistoryNavigation {
                url,
                m_session_history,
                PendingSessionHistoryNavigation::WebContentRestoreMode::RestoreFromUIProcess,
            };
        } else {
            m_pending_session_history_navigation.clear();
        }

        if (history_handling == Web::Bindings::NavigationHistoryBehavior::Replace)
            m_session_history.replace_current_entry(url, allocate_ui_process_document_state_id(), move(document_resource));
        else
            m_session_history.navigate(url, allocate_ui_process_document_state_id(), move(document_resource));
        m_current_web_content_session_history_matches_mirror = false;
        result.should_update_navigation_action_state = true;
    }

    result.history_load = prepare_history_load();
    VERIFY(result.history_load.has_value());
    return result;
}

PageLoadPreparation CanonicalTraversable::prepare_for_page_load(URL::URL const& url, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    PageLoadPreparation result;

    m_pending_session_history_traversal.clear();
    auto const* current_entry = m_session_history.current_entry();
    auto is_javascript_navigation = url.scheme() == "javascript"sv;
    result.should_defer_ui_process_history_update = is_javascript_navigation;
    if (current_entry && !is_javascript_navigation)
        m_pending_session_history_navigation = PendingSessionHistoryNavigation { url, m_session_history };
    else
        m_pending_session_history_navigation.clear();

    if (is_javascript_navigation)
        return result;

    auto ui_process_history_handling = history_handling;
    if (ui_process_history_handling == Web::Bindings::NavigationHistoryBehavior::Auto) {
        // https://html.spec.whatwg.org/multipage/browsing-the-web.html#navigate
        // If url equals navigable's active document's URL, and
        // initiatorOriginSnapshot is same origin with targetNavigable's
        // active document's origin, then set historyHandling to "replace".
        if (current_entry && current_entry->url == url)
            ui_process_history_handling = Web::Bindings::NavigationHistoryBehavior::Replace;
        else
            ui_process_history_handling = Web::Bindings::NavigationHistoryBehavior::Push;
    }

    if (ui_process_history_handling == Web::Bindings::NavigationHistoryBehavior::Replace)
        m_session_history.replace_current_entry(url, allocate_ui_process_document_state_id(), Empty {});
    else
        m_session_history.navigate(url, allocate_ui_process_document_state_id());
    m_current_web_content_session_history_matches_mirror = false;
    result.should_update_navigation_action_state = true;
    return result;
}

void CanonicalTraversable::prepare_for_non_history_page_load()
{
    m_current_web_content_session_history_matches_mirror = false;
    m_session_history.forget_web_content_state();
}

void CanonicalTraversable::prepare_for_reload()
{
    m_session_history.mark_current_entry_reload_pending();
    m_current_web_content_session_history_matches_mirror = false;
}

WebContentSessionHistoryUpdateDecision CanonicalTraversable::did_receive_web_content_session_history_update_for_testing(Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index)
{
    return {
        .update = update_session_history_from_web_content(move(entries), move(used_steps), current_used_step_index),
    };
}

void CanonicalTraversable::did_create_top_level_traversable(Web::HTML::SessionHistoryEntryDescriptor initial_history_entry)
{
    if (!m_session_history.entries().is_empty())
        return;
    m_session_history.initialize_with_initial_history_entry(move(initial_history_entry));
    m_current_web_content_session_history_matches_mirror = true;
}

Optional<Web::HTML::CrossProcessId> CanonicalTraversable::nested_history_id_for(CanonicalNavigable const& navigable) const
{
    if (&navigable == this)
        return {};
    return navigable.id();
}

bool CanonicalTraversable::update_session_history_entry_navigation_api_state(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, Web::HTML::StorageSerializationRecord navigation_api_state)
{
    VERIFY(&navigable.top_level_traversable() == this);

    return m_session_history.update_entry(nested_history_id_for(navigable), navigation_api_key, [&](auto& entry) {
        entry.navigation_api_state = navigation_api_state;
    });
}

bool CanonicalTraversable::update_session_history_entry_scroll_restoration_mode(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, Web::HTML::ScrollRestorationMode scroll_restoration_mode)
{
    VERIFY(&navigable.top_level_traversable() == this);

    return m_session_history.update_entry(nested_history_id_for(navigable), navigation_api_key, [&](auto& entry) {
        entry.scroll_restoration_mode = scroll_restoration_mode;
    });
}

bool CanonicalTraversable::update_session_history_entry_scroll_position_data(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, Web::HTML::SessionHistoryEntryScrollPositionData scroll_position_data)
{
    VERIFY(&navigable.top_level_traversable() == this);

    return m_session_history.update_entry(nested_history_id_for(navigable), navigation_api_key, [&](auto& entry) {
        entry.scroll_position_data = scroll_position_data;
    });
}

bool CanonicalTraversable::update_session_history_entry_document_state_navigable_target_name(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, Utf16String navigable_target_name)
{
    VERIFY(&navigable.top_level_traversable() == this);

    return m_session_history.update_document_state(nested_history_id_for(navigable), navigation_api_key, [&](auto& document_state) {
        document_state.navigable_target_name = navigable_target_name;
    });
}

bool CanonicalTraversable::set_session_history_entry_document_state_reload_pending(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, bool reload_pending)
{
    VERIFY(&navigable.top_level_traversable() == this);

    auto did_update = m_session_history.update_document_state(nested_history_id_for(navigable), navigation_api_key, [&](auto& document_state) {
        document_state.reload_pending = reload_pending;
    });
    if (did_update)
        m_current_web_content_session_history_matches_mirror = m_session_history.web_content_history_matches_mirror();
    return did_update;
}

Optional<i32> CanonicalTraversable::append_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id, Web::HTML::PendingSessionHistoryEntryDescriptor initial_history_entry)
{
    VERIFY(&parent_navigable.top_level_traversable() == this);

    auto child_navigable = find(child_navigable_id);
    if (!child_navigable.has_value() || child_navigable->parent() != &parent_navigable)
        return {};
    return m_session_history.append_nested_history(parent_navigable, parent_document_state_id, child_navigable_id, move(initial_history_entry));
}

bool CanonicalTraversable::remove_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id)
{
    VERIFY(&parent_navigable.top_level_traversable() == this);

    return m_session_history.remove_nested_history(parent_navigable, parent_document_state_id, child_navigable_id);
}

Optional<TraversableSessionHistory::SameDocumentNavigationFinalization> CanonicalTraversable::request_to_finalize_same_document_navigation(CanonicalNavigable const& navigable, Web::HTML::SameDocumentNavigationEntry target_entry, bool replaces_current_entry, Web::HTML::HistoryHandlingBehavior history_handling, Web::HTML::UserNavigationInvolvement user_involvement, bool applies_history_step_in_coordinator)
{
    VERIFY(&navigable.top_level_traversable() == this);

    auto finalization = m_session_history.finalize_same_document_navigation(navigable, move(target_entry), replaces_current_entry, history_handling, user_involvement, maximum_claimed_session_history_step());

    if (finalization.has_value() && applies_history_step_in_coordinator && !replaces_current_entry)
        claim_step_for_pending_same_document_history_operation(navigable.id(), finalization->target_step);

    // AD-HOC: A synchronous same-document commit that does not run through the coordinated apply-history-step
    //         operation still moves the canonical current step; record it as the newest commit so an operation
    //         still in flight cannot move the current step backwards past it when it completes.
    //         See https://github.com/whatwg/html/issues/12576.
    if (finalization.has_value() && !applies_history_step_in_coordinator)
        m_apply_history_step_traversable_state.committed_generation = ++m_apply_history_step_traversable_state.generation_counter;

    // WebContent has already committed this navigation locally, so a request the canonical history could not apply
    // leaves the two out of sync until the mirror is reconciled.
    m_current_web_content_session_history_matches_mirror = finalization.has_value() && m_session_history.web_content_history_matches_mirror();

    return finalization;
}

void CanonicalTraversable::finalize_cross_document_navigation(u64 operation_id, CanonicalNavigable const& navigable, Web::HTML::SessionHistoryEntryDescriptor history_entry, Optional<Utf16String> entry_to_replace_navigation_api_key)
{
    VERIFY(&navigable.top_level_traversable() == this);
    finalize_cross_document_navigation_for_history_operation(operation_id, navigable, move(history_entry), move(entry_to_replace_navigation_api_key));
}

Optional<i32> CanonicalTraversable::navigation_api_traversal_target(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key) const
{
    VERIFY(&navigable.top_level_traversable() == this);

    // 1. Let navigableSHEs be the result of getting session history entries given navigable.
    auto navigable_session_history_entries = m_session_history.get_session_history_entries(navigable);
    if (!navigable_session_history_entries.has_value())
        return {};

    // 2. Let targetSHE be the session history entry in navigableSHEs whose navigation API key is key. If no such entry exists, then:
    auto target_entry = navigable_session_history_entries->find_if([&](auto const& entry) {
        return entry.navigation_api_key == navigation_api_key;
    });
    if (target_entry == navigable_session_history_entries->end())
        return {};

    return target_entry->step;
}

WebContentSessionHistoryUpdateResult CanonicalTraversable::update_session_history_from_web_content(Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index)
{
    auto update_result = m_session_history.update_from_web_content(move(entries), move(used_steps), current_used_step_index);
    m_current_web_content_session_history_matches_mirror = update_result == TraversableSessionHistory::UpdateResult::CompleteSnapshot
        && m_session_history.web_content_history_matches_mirror();

    WebContentSessionHistoryUpdateResult result {
        .update_result = update_result,
    };

    if (update_result != TraversableSessionHistory::UpdateResult::InvalidSnapshot) {
        if (update_result == TraversableSessionHistory::UpdateResult::CompleteSnapshot)
            m_pending_session_history_navigation.clear();
        if (auto* current_entry = m_session_history.current_entry())
            result.current_url = current_entry->url;
    }

    return result;
}

NavigationStartResult CanonicalTraversable::did_start_navigation(URL::URL const& url, Web::HTML::DocumentResource document_resource, bool is_redirect, Web::Bindings::NavigationHistoryBehavior history_handling, bool is_showing_crash_page)
{
    if (is_showing_crash_page) {
        if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url)
            return { .dump_reason = "did-start-navigation-from-crash-page"sv, .did_clear_crash_page = true };
    }

    if (is_redirect) {
        m_session_history.replace_current_entry_url(url, allocate_ui_process_document_state_id());
        if (m_pending_session_history_navigation.has_value())
            m_pending_session_history_navigation->url = url;
        m_current_web_content_session_history_matches_mirror = false;
        return { .dump_reason = "did-start-navigation-redirect"sv, .should_update_navigation_action_state = true, .should_update_webdriver_pending_navigation_url = true, .did_clear_crash_page = is_showing_crash_page };
    }

    if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url) {
        if (m_pending_session_history_navigation.has_value() && m_pending_session_history_navigation->url == url)
            return { .did_clear_crash_page = is_showing_crash_page };

        if (history_handling == Web::Bindings::NavigationHistoryBehavior::Push && m_current_web_content_session_history_matches_mirror)
            m_pending_session_history_navigation = PendingSessionHistoryNavigation { url, m_session_history };
        else
            m_pending_session_history_navigation.clear();

        if (history_handling == Web::Bindings::NavigationHistoryBehavior::Replace) {
            m_session_history.replace_current_entry(url, allocate_ui_process_document_state_id(), move(document_resource));
            m_current_web_content_session_history_matches_mirror = false;
            return { .dump_reason = "did-start-navigation-replace-current-url"sv, .should_update_navigation_action_state = true, .did_clear_crash_page = is_showing_crash_page };
        }
        if (history_handling == Web::Bindings::NavigationHistoryBehavior::Push) {
            m_session_history.navigate(url, allocate_ui_process_document_state_id(), move(document_resource));
            m_current_web_content_session_history_matches_mirror = false;
            return { .dump_reason = "did-start-navigation-push-current-url"sv, .should_update_navigation_action_state = true, .did_clear_crash_page = is_showing_crash_page };
        }
        return { .did_clear_crash_page = is_showing_crash_page };
    }

    if (m_session_history.current_entry())
        m_pending_session_history_navigation = PendingSessionHistoryNavigation { url, m_session_history };
    else
        m_pending_session_history_navigation.clear();
    if (history_handling == Web::Bindings::NavigationHistoryBehavior::Replace)
        m_session_history.replace_current_entry(url, allocate_ui_process_document_state_id(), move(document_resource));
    else
        m_session_history.navigate(url, allocate_ui_process_document_state_id(), move(document_resource));
    m_current_web_content_session_history_matches_mirror = false;
    return { .dump_reason = "did-start-navigation"sv, .should_update_navigation_action_state = true, .did_clear_crash_page = is_showing_crash_page };
}

NavigationCancelResult CanonicalTraversable::did_cancel_navigation(URL::URL const& url, bool has_webdriver_pending_navigation)
{
    if (m_pending_session_history_navigation.has_value() && m_pending_session_history_navigation->url == url)
        return { .status = NavigationCancelStatus::RestorePendingSessionHistoryNavigation };

    if (has_webdriver_pending_navigation) {
        m_session_history.clear_current_entry_reload_pending();
        return { .status = NavigationCancelStatus::CompleteWebdriverPendingNavigation };
    }

    return {};
}

NavigationFinishResult CanonicalTraversable::did_finish_navigation(URL::URL const& url)
{
    NavigationFinishResult result;
    if (m_pending_session_history_navigation.has_value()) {
        if (m_pending_session_history_navigation->url == url) {
            m_pending_session_history_navigation.clear();
        } else if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url) {
            m_pending_session_history_navigation.clear();
            result.should_update_webdriver_pending_navigation_url = true;
        }
    }

    if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url)
        m_session_history.clear_current_entry_reload_pending();
    return result;
}

RestorePendingSessionHistoryNavigationResult CanonicalTraversable::restore_pending_session_history_navigation()
{
    VERIFY(m_pending_session_history_navigation.has_value());

    auto web_content_restore_mode = m_pending_session_history_navigation->web_content_restore_mode;
    m_session_history = move(m_pending_session_history_navigation->previous_session_history);
    m_pending_session_history_navigation.clear();
    m_pending_session_history_traversal.clear();

    RestorePendingSessionHistoryNavigationResult result { .web_content_restore_mode = web_content_restore_mode };
    if (auto* current_entry = m_session_history.current_entry()) {
        result.current_url = current_entry->url;
        if (web_content_restore_mode == PendingSessionHistoryNavigation::WebContentRestoreMode::PreserveCurrentProcessState)
            m_current_web_content_session_history_matches_mirror = m_session_history.web_content_history_matches_mirror();
    } else {
        m_current_web_content_session_history_matches_mirror = false;
    }
    return result;
}

bool CanonicalTraversable::web_content_can_apply_traversal(TraversableSessionHistory::TraversalTarget const& target) const
{
    return m_session_history.web_content_can_traverse_to(target);
}

bool CanonicalTraversable::traversal_requires_process_replacement(TraversableSessionHistory::TraversalTarget const& target, URL::URL const& current_url) const
{
    return SiteIsolationManager::the().navigation_requires_process_swap(current_url, target.target_top_level_entry->url);
}

void CanonicalTraversable::traverse_the_history_by_delta(int delta, CheckForCancelation check_for_cancelation, Function<void(HistoryTraversalOutcome)> on_complete, Function<void()> on_top_level_traversal_applied)
{
    m_history_traversal_queue.append_session_history_traversal_steps(
        [this, delta, check_for_cancelation, on_complete = move(on_complete), on_top_level_traversal_applied = move(on_top_level_traversal_applied)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
            auto target = m_session_history.traversal_target_for_delta(delta);
            if (!target.has_value()) {
                auto view = ViewImplementation::find_view_for_traversable(*this);
                VERIFY(view.has_value());
                view->dump_session_history("traverse-no-entry"sv);
                if (on_complete)
                    on_complete({ .status = HistoryTraversalStatus::NoEntry });
                promise->resolve({});
                return;
            }

            traverse_the_history(*target, check_for_cancelation, move(on_complete), move(on_top_level_traversal_applied), move(promise));
        });
}

void CanonicalTraversable::traverse_the_history_to_step(i32 step, CheckForCancelation check_for_cancelation, Function<void(HistoryTraversalOutcome)> on_complete, Function<void()> on_top_level_traversal_applied)
{
    m_history_traversal_queue.append_session_history_traversal_steps(
        [this, step, check_for_cancelation, on_complete = move(on_complete), on_top_level_traversal_applied = move(on_top_level_traversal_applied)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
            auto target = m_session_history.traversal_target_for_step(step);
            if (!target.has_value()) {
                auto view = ViewImplementation::find_view_for_traversable(*this);
                VERIFY(view.has_value());
                view->dump_session_history("traverse-no-entry"sv);
                if (on_complete)
                    on_complete({ .status = HistoryTraversalStatus::NoEntry });
                promise->resolve({});
                return;
            }

            traverse_the_history(*target, check_for_cancelation, move(on_complete), move(on_top_level_traversal_applied), move(promise));
        });
}

void CanonicalTraversable::reconstruct_the_history_to_step(i32 step, bool requires_process_replacement, Function<void()> on_top_level_traversal_applied)
{
    m_history_traversal_queue.append_session_history_traversal_steps(
        [this, step, requires_process_replacement, on_top_level_traversal_applied = move(on_top_level_traversal_applied)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
            auto target = m_session_history.traversal_target_for_step(step);
            if (!target.has_value()) {
                promise->resolve({});
                return;
            }

            traverse_the_history(*target, CheckForCancelation::No, nullptr, move(on_top_level_traversal_applied), move(promise), requires_process_replacement);
        });
}

void CanonicalTraversable::traverse_the_history(TraversableSessionHistory::TraversalTarget const& target, CheckForCancelation check_for_cancelation, Function<void(HistoryTraversalOutcome)> on_complete, Function<void()> on_top_level_traversal_applied, NonnullRefPtr<Core::Promise<Empty>> promise, bool requires_process_replacement)
{
    auto view = ViewImplementation::find_view_for_traversable(*this);
    VERIFY(view.has_value());

    auto will_replace_web_content_process = requires_process_replacement
        || SiteIsolationManager::the().navigation_requires_process_swap(view->url(), target.target_top_level_entry->url);
    auto pending_traversal = PendingSessionHistoryTraversal {
        .target_step = target.target_step,
        .target_step_index = target.target_step_index,
        .will_change_top_level_entry = target.changes_top_level_entry,
        .will_replace_web_content_process = will_replace_web_content_process,
        .on_cancelation_check_complete = nullptr,
        .on_top_level_traversal_applied = move(on_top_level_traversal_applied),
    };

    if (web_content_can_apply_traversal(target) && !will_replace_web_content_process) {
        // NB: web_content_can_apply_history_traversal mirrors this condition for requested traversals.
        m_pending_session_history_traversal = move(pending_traversal);
        auto webdriver_pending_navigation_completes_with_session_history_update = false;
        if (auto const* current_entry = m_session_history.current_entry()) {
            webdriver_pending_navigation_completes_with_session_history_update = current_entry->document_state.id == target.target_top_level_entry->document_state.id;
        }
        view->will_apply_history_traversal_step(target.target_top_level_entry->url, webdriver_pending_navigation_completes_with_session_history_update);
        run_ui_history_operation_at_queue_position(
            BrowserHistoryTraversalOperation {
                .target_step = target.target_step,
                .check_for_cancelation = check_for_cancelation != CheckForCancelation::No,
            },
            [this, step = target.target_step](Web::HTML::HistoryStepResult result, Optional<i32>) {
                auto view = ViewImplementation::find_view_for_traversable(*this);
                VERIFY(view.has_value());
                view->apply_history_traversal_step_result(step, result);
            },
            move(promise));
        if (on_complete)
            on_complete({ .status = HistoryTraversalStatus::Started, .will_change_top_level_entry = target.changes_top_level_entry });
        return;
    }

    auto needs_cancelation_check = check_for_cancelation == CheckForCancelation::Yes
        || (check_for_cancelation == CheckForCancelation::IfWebContentCannotTraverseTarget && !web_content_can_apply_traversal(target));
    if (needs_cancelation_check) {
        pending_traversal.stage = PendingSessionHistoryTraversal::Stage::CheckingCancelation;
        pending_traversal.cancelation_check_request_id = m_next_traverse_history_step_cancelation_check_request_id++;
        pending_traversal.on_cancelation_check_complete = move(on_complete);
        auto request_id = pending_traversal.cancelation_check_request_id;
        m_pending_session_history_traversal = move(pending_traversal);
        view->will_check_history_traversal_cancelation();
        run_ui_history_operation_at_queue_position(
            HistoryStepCancelationCheckOperation { .target_step = target.target_step },
            [this, request_id, step = target.target_step](Web::HTML::HistoryStepResult result, Optional<i32>) {
                auto view = ViewImplementation::find_view_for_traversable(*this);
                VERIFY(view.has_value());
                view->apply_history_step_cancelation_check_result(request_id, step, result);
            },
            move(promise));
        return;
    }

    m_pending_session_history_navigation.clear();
    m_pending_session_history_traversal = move(pending_traversal);
    view->will_apply_history_traversal_step(target.target_top_level_entry->url, true);
    run_ui_history_operation_at_queue_position(
        BrowserHistoryTraversalOperation {
            .target_step = target.target_step,
            .check_for_cancelation = false,
            .reconstructs_web_content_history = true,
            .requires_process_replacement = will_replace_web_content_process,
        },
        [this, step = target.target_step](Web::HTML::HistoryStepResult result, Optional<i32>) {
            auto view = ViewImplementation::find_view_for_traversable(*this);
            VERIFY(view.has_value());
            view->apply_history_traversal_step_result(step, result);
        },
        move(promise));
    if (on_complete)
        on_complete({ .status = HistoryTraversalStatus::Started, .will_replace_web_content_process = will_replace_web_content_process, .will_change_top_level_entry = target.changes_top_level_entry });
}

bool CanonicalTraversable::notify_top_level_traversal_applied()
{
    if (!m_pending_session_history_traversal.has_value())
        return false;

    auto callback = move(m_pending_session_history_traversal->on_top_level_traversal_applied);
    if (!callback)
        return false;

    callback();
    return true;
}

WebContentHistoryStepResult CanonicalTraversable::did_traverse_the_history_to_step(i32 step, Web::HTML::HistoryStepResult result)
{
    if (!m_pending_session_history_traversal.has_value() || m_pending_session_history_traversal->target_step != step)
        return { .dump_reason = "ignored-stale-webcontent-history-step-result"sv };

    if (result != Web::HTML::HistoryStepResult::Applied) {
        m_pending_session_history_traversal.clear();
        return { .dump_reason = "webcontent-history-step-canceled"sv, .should_update_navigation_action_state = true, .should_complete_webdriver_pending_navigation = true, .should_update_webdriver_pending_navigation_to_current_url = true, .should_reset_webdriver_pending_navigation_completion = true };
    }

    if (!m_session_history.did_apply_web_content_traversal_to_step(step)) {
        m_current_web_content_session_history_matches_mirror = false;
        m_session_history.forget_web_content_state();
        m_pending_session_history_traversal.clear();
        return { .dump_reason = "webcontent-history-step-applied-without-ui-target"sv, .should_update_navigation_action_state = true, .should_complete_webdriver_pending_navigation = true, .should_update_webdriver_pending_navigation_to_current_url = true, .should_reset_webdriver_pending_navigation_completion = true };
    }

    m_current_web_content_session_history_matches_mirror = true;
    auto did_notify_top_level_traversal_applied = notify_top_level_traversal_applied();
    auto should_complete_webdriver_pending_navigation = !did_notify_top_level_traversal_applied
        && !m_pending_session_history_traversal->will_change_top_level_entry;
    Optional<URL::URL> current_url;
    if (auto const* current_entry = m_session_history.current_entry())
        current_url = current_entry->url;
    m_pending_session_history_traversal.clear();
    return { .dump_reason = "webcontent-history-step-applied"sv, .should_update_navigation_action_state = true, .current_url = move(current_url), .should_complete_webdriver_pending_navigation = should_complete_webdriver_pending_navigation };
}

HistoryStepCancelationCheckResult CanonicalTraversable::did_check_if_traverse_history_step_is_canceled(u64 request_id, i32 step, Web::HTML::HistoryStepResult result)
{
    if (!m_pending_session_history_traversal.has_value()
        || m_pending_session_history_traversal->stage != PendingSessionHistoryTraversal::Stage::CheckingCancelation
        || m_pending_session_history_traversal->cancelation_check_request_id != request_id
        || m_pending_session_history_traversal->target_step != step)
        return { .dump_reason = "ignored-stale-history-step-cancelation-check-result"sv };

    if (result == Web::HTML::HistoryStepResult::CanceledPendingNavigation) {
        auto target = m_session_history.traversal_target_for_step(step);
        auto const* previous_current_entry = m_pending_session_history_navigation.has_value()
            ? m_pending_session_history_navigation->previous_session_history.current_entry()
            : nullptr;
        // INTEROP: WebContent handled this browser UI traversal as stop loading rather than applying a history
        //          step. If it preserved the active document, discard the UI process's uncommitted speculative
        //          entry so both processes continue to expose that document as the current history entry.
        if (target.has_value()
            && previous_current_entry
            && m_pending_session_history_navigation->web_content_restore_mode == PendingSessionHistoryNavigation::WebContentRestoreMode::PreserveCurrentProcessState
            && target->target_top_level_entry->document_state.id == previous_current_entry->document_state.id) {
            auto on_cancelation_check_complete = move(m_pending_session_history_traversal->on_cancelation_check_complete);
            m_pending_session_history_traversal.clear();
            return {
                .dump_reason = "traverse-canceled-pending-navigation"sv,
                .on_cancelation_check_complete = move(on_cancelation_check_complete),
                .outcome = { .status = HistoryTraversalStatus::Started },
                .should_restore_pending_navigation = true,
            };
        }
        result = Web::HTML::HistoryStepResult::Applied;
    }

    if (result != Web::HTML::HistoryStepResult::Applied) {
        auto on_cancelation_check_complete = move(m_pending_session_history_traversal->on_cancelation_check_complete);
        m_pending_session_history_traversal.clear();
        return { .dump_reason = "traverse-fallback-canceled-by-webcontent"sv, .on_cancelation_check_complete = move(on_cancelation_check_complete), .outcome = { .status = HistoryTraversalStatus::Canceled }, .should_update_navigation_action_state = true, .should_complete_webdriver_pending_navigation = true, .should_update_webdriver_pending_navigation_to_current_url = true, .should_reset_webdriver_pending_navigation_completion = true };
    }

    auto target = m_session_history.traversal_target_for_step(step);
    if (!target.has_value()) {
        auto on_cancelation_check_complete = move(m_pending_session_history_traversal->on_cancelation_check_complete);
        m_current_web_content_session_history_matches_mirror = false;
        m_session_history.forget_web_content_state();
        m_pending_session_history_traversal.clear();
        return { .dump_reason = "traverse-fallback-cancelation-check-without-ui-target"sv, .on_cancelation_check_complete = move(on_cancelation_check_complete), .outcome = { .status = HistoryTraversalStatus::NoEntry }, .should_update_navigation_action_state = true };
    }

    auto on_cancelation_check_complete = move(m_pending_session_history_traversal->on_cancelation_check_complete);
    return {
        .dump_reason = "traverse-reconstruct-after-cancelation-check"sv,
        .on_cancelation_check_complete = move(on_cancelation_check_complete),
        .outcome = { .status = HistoryTraversalStatus::Started, .will_replace_web_content_process = m_pending_session_history_traversal->will_replace_web_content_process, .will_change_top_level_entry = m_pending_session_history_traversal->will_change_top_level_entry },
        .target = *target,
    };
}

Optional<Web::HistoryLoad> CanonicalTraversable::prepare_history_load()
{
    auto current_top_level_entry_index = m_session_history.current_top_level_entry_index();
    auto current_step = m_session_history.current_step();
    if (!current_top_level_entry_index.has_value() || !current_step.has_value())
        return {};

    auto entries = m_session_history.entries();
    if (entries.is_empty())
        return {};

    auto history_object_length_and_index = m_session_history.get_the_history_object_length_and_index(*current_step);
    auto entries_for_navigation_api = m_session_history.get_session_history_entries_for_the_navigation_api(*this, *current_step);
    if (!history_object_length_and_index.has_value() || !entries_for_navigation_api.has_value())
        return {};

    auto target_entry = entries[*current_top_level_entry_index];
    m_session_history.did_install_web_content_history_projection(*current_top_level_entry_index, *current_step);

    return Web::HistoryLoad {
        .load_id = m_next_history_load_id++,
        .navigable_id = id(),
        .target_entry = move(target_entry),
        .global_history_length = history_object_length_and_index->script_history_length,
        .global_history_index = history_object_length_and_index->script_history_index,
        .entries_for_navigation_api = entries_for_navigation_api.release_value(),
        .transitional_top_level_entries = move(entries),
        .transitional_current_top_level_entry_index = *current_top_level_entry_index,
    };
}

void CanonicalTraversable::abandon_after_web_content_process_crash()
{
    abandon_history_operations();
    m_pending_session_history_navigation.clear();
    m_pending_session_history_traversal.clear();
    m_current_web_content_session_history_matches_mirror = false;
    m_session_history.forget_web_content_state();
}

void CanonicalTraversable::reset_session_history_for_testing()
{
    abandon_history_operations();
    m_session_history.clear();
    m_current_web_content_session_history_matches_mirror = false;
    m_pending_session_history_navigation.clear();
    m_pending_session_history_traversal.clear();
}

void CanonicalTraversable::mark_web_content_session_history_stale_for_testing()
{
    m_current_web_content_session_history_matches_mirror = false;
}

StringView CanonicalTraversable::pending_session_history_navigation_web_content_restore_mode_to_string(PendingSessionHistoryNavigation::WebContentRestoreMode mode)
{
    switch (mode) {
    case PendingSessionHistoryNavigation::WebContentRestoreMode::PreserveCurrentProcessState:
        return "preserve-current-process-state"sv;
    case PendingSessionHistoryNavigation::WebContentRestoreMode::RestoreFromUIProcess:
        return "restore-from-ui-process"sv;
    }
    VERIFY_NOT_REACHED();
}

StringView CanonicalTraversable::pending_session_history_traversal_stage_to_string(PendingSessionHistoryTraversal::Stage stage)
{
    switch (stage) {
    case PendingSessionHistoryTraversal::Stage::ApplyingInWebContent:
        return "applying-in-webcontent"sv;
    case PendingSessionHistoryTraversal::Stage::CheckingCancelation:
        return "checking-cancelation"sv;
    }
    VERIFY_NOT_REACHED();
}

struct CanonicalTraversable::HistoryOperation {
    using Parameters = Variant<Web::HistoryOperationParameters, BrowserHistoryTraversalOperation, HistoryStepCancelationCheckOperation>;

    HistoryOperation(u64 operation_id, Parameters parameters, Optional<u64> initiation_id, RefPtr<WebContentClient> initiating_client, u64 initiating_page_id, Optional<i32> resolved_step, OnHistoryOperationComplete on_complete)
        : operation_id(operation_id)
        , initiation_id(initiation_id)
        , parameters(move(parameters))
        , on_complete(move(on_complete))
        , initiating_client(move(initiating_client))
        , initiating_page_id(initiating_page_id)
        , resolved_step(resolved_step)
    {
    }

    u64 operation_id { 0 };
    Optional<u64> initiation_id;
    Parameters parameters;
    OnHistoryOperationComplete on_complete;
    // The initiating endpoint owns the state parked under initiation_id and must remain stable across process
    // replacement. Jobs for individual navigables resolve their endpoints when they are dispatched instead.
    RefPtr<WebContentClient> initiating_client;
    u64 initiating_page_id { 0 };
    struct CompletionEndpoint {
        RefPtr<WebContentClient> client;
        u64 page_id { 0 };
        Optional<u64> initiation_id;
    };
    Vector<CompletionEndpoint> completion_endpoints;
    // Delta and Navigation API traversals resolve their canonical target when their queue position is reached.
    Optional<i32> resolved_step;
    // Push and replace receive their canonical target before their process-local finalization steps run.
    Optional<i32> assigned_target_step;
    // A same-document push claims its new entry's step before apply-the-history-step commits it. Keeping that claim
    // on the operation makes its lifetime match the operation automatically, including cancellation and abandonment.
    Optional<i32> claimed_step;
    bool reconstructs_web_content_history { false };
    bool requires_process_replacement { false };
    Optional<i32> active_step;
    Web::HTML::UserNavigationInvolvement user_involvement { Web::HTML::UserNavigationInvolvement::None };
    Function<void(ApplyHistoryStepJobs::InitiatorSandboxingCheckResult)> pending_sandboxing_check;
    Function<void(Web::HTML::HistoryStepResult)> pending_unload_cancelation;
    HashMap<Web::HTML::CrossProcessId, Function<void(Web::HTML::ChangingNavigableHistoryStepJobDisposition)>> pending_changing_jobs;
    HashMap<Web::HTML::CrossProcessId, Function<void()>> pending_continuations;
    HashMap<Web::HTML::CrossProcessId, Function<void()>> pending_nonchanging_updates;
    OwnPtr<ApplyHistoryStep> algorithm;
    RefPtr<Core::Promise<Empty>> queue_promise;
};

CanonicalTraversable::~CanonicalTraversable() = default;

void CanonicalTraversable::recover_from_web_content_process_crash(OnHistoryOperationComplete on_complete)
{
    m_pending_session_history_navigation.clear();
    m_current_web_content_session_history_matches_mirror = false;
    m_session_history.forget_web_content_state();

    for (auto& operation : m_history_operations) {
        if (!operation.value->parameters.has<BrowserHistoryTraversalOperation>())
            continue;

        auto endpoint = history_job_endpoint_for(*this);
        VERIFY(endpoint.client);
        operation.value->pending_sandboxing_check = nullptr;
        operation.value->pending_unload_cancelation = nullptr;
        operation.value->pending_changing_jobs.clear();
        operation.value->pending_continuations.clear();
        operation.value->pending_nonchanging_updates.clear();
        operation.value->algorithm = nullptr;
        operation.value->initiating_client = endpoint.client;
        operation.value->initiating_page_id = endpoint.page_id;
        operation.value->completion_endpoints.clear();
        auto& parameters = operation.value->parameters.get<BrowserHistoryTraversalOperation>();
        parameters.check_for_cancelation = false;
        parameters.reconstructs_web_content_history = true;
        parameters.restores_replacement_process = true;
        VERIFY(operation.value->queue_promise);
        start_history_operation(*operation.value, *operation.value->queue_promise);
        return;
    }

    abandon_history_operations();

    auto current_step = m_session_history.current_step();
    if (!current_step.has_value()) {
        if (on_complete)
            on_complete(Web::HTML::HistoryStepResult::CanceledByMissingPage, {});
        return;
    }
    enqueue_history_operation(
        BrowserHistoryTraversalOperation {
            .target_step = *current_step,
            .check_for_cancelation = false,
            .reconstructs_web_content_history = true,
            .restores_replacement_process = true,
        },
        move(on_complete));
}

CanonicalTraversable::HistoryJobEndpoint CanonicalTraversable::history_job_endpoint_for(CanonicalNavigable const& navigable) const
{
    if (auto* client = navigable.reporting_client_if_any())
        return { client, navigable.reporting_page_id() };

    // NB: The traversable is the view's root navigable; the process hosting its documents is the view's client
    //     rather than a reporting client recorded in the tree.
    if (&navigable == this) {
        if (auto view = ViewImplementation::find_view_for_traversable(*this); view.has_value())
            return { &view->client(), view->page_id() };
    }
    return {};
}

CanonicalTraversable::HistoryOperation* CanonicalTraversable::find_history_operation(u64 operation_id)
{
    auto operation = m_history_operations.find(operation_id);
    if (operation == m_history_operations.end())
        return nullptr;
    return operation->value.ptr();
}

void CanonicalTraversable::add_history_operation_completion_endpoint(HistoryOperation& operation, HistoryJobEndpoint endpoint, Optional<u64> initiation_id)
{
    VERIFY(endpoint.client);
    for (auto& existing : operation.completion_endpoints) {
        if (existing.client.ptr() != endpoint.client || existing.page_id != endpoint.page_id)
            continue;
        if (initiation_id.has_value())
            existing.initiation_id = initiation_id;
        return;
    }
    operation.completion_endpoints.append({ endpoint.client, endpoint.page_id, initiation_id });
}

bool CanonicalTraversable::is_history_traversal_operation(HistoryOperation const& operation) const
{
    return operation.parameters.visit(
        [](Web::HistoryOperationParameters const& parameters) {
            return parameters.visit(
                [](Web::TraverseByDeltaHistoryOperationParameters const&) { return true; },
                [](Web::TraverseToStepHistoryOperationParameters const&) { return true; },
                [](Web::NavigationAPITraverseHistoryOperationParameters const&) { return true; },
                [](Web::ResumeTraverseHistoryOperationParameters const&) { return true; },
                [](auto const&) { return false; });
        },
        [](BrowserHistoryTraversalOperation const&) { return true; },
        [](HistoryStepCancelationCheckOperation const&) { return false; });
}

Optional<i32> CanonicalTraversable::maximum_claimed_session_history_step() const
{
    Optional<i32> maximum_claimed_step;
    for (auto const& operation : m_history_operations) {
        if (!operation.value->claimed_step.has_value())
            continue;
        if (!maximum_claimed_step.has_value() || *operation.value->claimed_step > *maximum_claimed_step)
            maximum_claimed_step = operation.value->claimed_step;
    }
    return maximum_claimed_step;
}

void CanonicalTraversable::claim_step_for_pending_same_document_history_operation(Web::HTML::CrossProcessId navigable_id, i32 claimed_step)
{
    for (auto& operation : m_history_operations) {
        if (!operation.value->parameters.has<Web::HistoryOperationParameters>() || operation.value->algorithm)
            continue;
        auto const& request = operation.value->parameters.get<Web::HistoryOperationParameters>();
        if (!request.has<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>())
            continue;
        auto const& parameters = request.get<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>();
        if (parameters.navigable_id != navigable_id || parameters.history_handling != Web::HTML::HistoryHandlingBehavior::Push)
            continue;

        operation.value->claimed_step = claimed_step;
        return;
    }
}

ApplyHistoryStepJobs CanonicalTraversable::create_apply_history_step_jobs(u64 operation_id)
{
    return {
        .run_initiator_sandboxing_check_job = [this, operation_id](Web::HTML::CrossProcessId initiator_to_check, Vector<Web::HTML::CrossProcessId> navigables, Function<void(ApplyHistoryStepJobs::InitiatorSandboxingCheckResult)> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;

            // sourceSnapshotParams remains parked under this operation's initiation ID in the initiating process.
            VERIFY(operation->initiating_client);
            VERIFY(operation->initiation_id.has_value());
            operation->pending_sandboxing_check = move(on_complete);
            operation->initiating_client->async_run_initiator_sandboxing_check_job(operation->initiating_page_id, operation_id, initiator_to_check, move(navigables), *operation->initiation_id); },
        .run_unload_cancelation_job = [this, operation_id](i32 target_step, Vector<Web::HTML::CrossProcessId> navigables_crossing_documents, Web::HTML::UserNavigationInvolvement user_involvement, Function<void(Web::HTML::HistoryStepResult)> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            VERIFY(operation->initiating_client);
            operation->pending_unload_cancelation = move(on_complete);
            operation->initiating_client->async_run_history_step_unload_cancelation_job(operation->initiating_page_id, operation_id, target_step, move(navigables_crossing_documents), user_involvement); },
        .run_changing_navigable_history_step_job = [this, operation_id](ApplyHistoryStepJobs::ChangingNavigableHistoryStepJob job, Function<void(Web::HTML::ChangingNavigableHistoryStepJobDisposition)> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            if (job.navigable_id == id() && job.navigation_type == Web::Bindings::NavigationType::Traverse) {
                auto reconstructs_web_content_history = operation->reconstructs_web_content_history;
                auto requires_process_replacement = operation->requires_process_replacement
                    || (operation->parameters.has<BrowserHistoryTraversalOperation>()
                        && operation->parameters.get<BrowserHistoryTraversalOperation>().requires_process_replacement);
                auto restores_replacement_process = operation->parameters.has<BrowserHistoryTraversalOperation>()
                    && operation->parameters.get<BrowserHistoryTraversalOperation>().restores_replacement_process;
                if (!restores_replacement_process) {
                    auto view = ViewImplementation::find_view_for_traversable(*this);
                    if (requires_process_replacement
                        || (view.has_value() && SiteIsolationManager::the().navigation_requires_process_swap(view->url(), job.target_entry.url))) {
                        VERIFY(view.has_value());
                        view->replace_web_content_process_for_history_traversal();
                        restores_replacement_process = true;
                        reconstructs_web_content_history = true;
                        operation->reconstructs_web_content_history = true;
                        if (!operation->initiation_id.has_value())
                            operation->completion_endpoints.clear();
                    }
                }
                if (reconstructs_web_content_history) {
                    job.replacement_top_level_entries = m_session_history.entries();
                    auto target = m_session_history.traversal_target_for_step(job.replacement_current_step);
                    VERIFY(target.has_value());
                    job.replacement_current_top_level_entry_index = target->target_top_level_entry_index;
                    m_session_history.did_install_web_content_history_projection(target->target_top_level_entry_index, job.replacement_current_step);
                }
            }
            auto navigable = find(job.navigable_id);
            auto endpoint = navigable.has_value() ? history_job_endpoint_for(*navigable) : HistoryJobEndpoint {};
            if (!endpoint.client) {
                on_complete(Web::HTML::ChangingNavigableHistoryStepJobDisposition::Skipped);
                return;
            }
            add_history_operation_completion_endpoint(*operation, endpoint);
            auto navigable_id = job.navigable_id;
            auto initiation_id = endpoint.client == operation->initiating_client.ptr() && endpoint.page_id == operation->initiating_page_id
                ? operation->initiation_id
                : Optional<u64> {};
            operation->pending_changing_jobs.set(navigable_id, move(on_complete));
            endpoint.client->async_run_changing_navigable_history_job(endpoint.page_id, operation_id, navigable_id, move(job.target_entry), job.user_involvement, job.navigation_type, job.synchronous_navigation == Web::HTML::SynchronousNavigation::Yes, job.navigation_api_abort_behavior, initiation_id, move(job.replacement_top_level_entries), job.replacement_current_top_level_entry_index, job.replacement_current_step); },
        .apply_changing_navigable_history_step_continuation = [this, operation_id](ApplyHistoryStepJobs::ApplyChangingNavigableHistoryStepContinuation continuation, Function<void()> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            auto navigable = find(continuation.navigable_id);
            auto endpoint = navigable.has_value() ? history_job_endpoint_for(*navigable) : HistoryJobEndpoint {};
            if (!endpoint.client) {
                on_complete();
                return;
            }
            add_history_operation_completion_endpoint(*operation, endpoint);
            auto navigable_id = continuation.navigable_id;
            operation->pending_continuations.set(navigable_id, move(on_complete));
            endpoint.client->async_apply_changing_navigable_continuation(endpoint.page_id, operation_id, navigable_id,
                continuation.history_object_length_and_index.script_history_length, continuation.history_object_length_and_index.script_history_index,
                move(continuation.entries_for_navigation_api)); },
        .update_nonchanging_navigable_history_step_state = [this, operation_id](Web::HTML::CrossProcessId navigable_id, Web::HTML::HistoryObjectLengthAndIndex history_object_length_and_index, Function<void()> on_complete) {
            auto* operation = find_history_operation(operation_id);
            if (!operation)
                return;
            auto navigable = find(navigable_id);
            auto endpoint = navigable.has_value() ? history_job_endpoint_for(*navigable) : HistoryJobEndpoint {};
            if (!endpoint.client) {
                on_complete();
                return;
            }
            add_history_operation_completion_endpoint(*operation, endpoint);
            operation->pending_nonchanging_updates.set(navigable_id, move(on_complete));
            endpoint.client->async_update_nonchanging_navigable_history_state(endpoint.page_id, operation_id, navigable_id,
                history_object_length_and_index.script_history_length, history_object_length_and_index.script_history_index); },
    };
}

void CanonicalTraversable::finalize_cross_document_navigation_for_history_operation(u64 operation_id, CanonicalNavigable const& navigable, Web::HTML::SessionHistoryEntryDescriptor history_entry, Optional<Utf16String> entry_to_replace_navigation_api_key)
{
    auto* operation = find_history_operation(operation_id);
    if (!operation || operation->algorithm || !operation->parameters.has<Web::HistoryOperationParameters>())
        return;

    auto const& parameters = operation->parameters.get<Web::HistoryOperationParameters>();
    auto is_matching_push = parameters.has<Web::PushHistoryOperationParameters>()
        && parameters.get<Web::PushHistoryOperationParameters>().navigable_id == navigable.id();
    auto is_matching_replace = parameters.has<Web::ReplaceHistoryOperationParameters>()
        && parameters.get<Web::ReplaceHistoryOperationParameters>().navigable_id == navigable.id();
    if (!is_matching_push && !is_matching_replace)
        return;

    if (is_matching_push && operation->assigned_target_step != history_entry.step)
        return;
    auto maximum_claimed_step = maximum_claimed_session_history_step();

    auto did_finalize = m_session_history.finalize_cross_document_navigation(nested_history_id_for(navigable), move(history_entry), move(entry_to_replace_navigation_api_key), maximum_claimed_step);
    if (did_finalize)
        m_current_web_content_session_history_matches_mirror = false;
}

void CanonicalTraversable::run_history_operation_at_queue_position(u64 initiation_id, Web::HistoryOperationParameters request, WebContentClient& requesting_client, u64 requesting_page_id, Optional<i32> resolved_step, OnHistoryOperationComplete on_complete, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    auto operation_id = m_next_history_operation_id++;
    m_history_operations.set(operation_id, make<HistoryOperation>(operation_id, HistoryOperation::Parameters { move(request) }, initiation_id, &requesting_client, requesting_page_id, resolved_step, move(on_complete)));
    auto* operation = find_history_operation(operation_id);
    VERIFY(operation);
    operation->queue_promise = promise;
    start_history_operation(*operation, promise);
}

void CanonicalTraversable::run_history_operation_at_queue_position(u64 initiation_id, HistoryStepCancelationCheckOperation parameters, WebContentClient& requesting_client, u64 requesting_page_id, OnHistoryOperationComplete on_complete, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    auto operation_id = m_next_history_operation_id++;
    m_history_operations.set(operation_id, make<HistoryOperation>(operation_id, HistoryOperation::Parameters { move(parameters) }, initiation_id, &requesting_client, requesting_page_id, Optional<i32> {}, move(on_complete)));
    auto* operation = find_history_operation(operation_id);
    VERIFY(operation);
    operation->queue_promise = promise;
    start_history_operation(*operation, promise);
}

void CanonicalTraversable::run_ui_history_operation_at_queue_position(Variant<BrowserHistoryTraversalOperation, HistoryStepCancelationCheckOperation> parameters, OnHistoryOperationComplete on_complete, NonnullRefPtr<Core::Promise<Empty>> promise)
{
    auto operation_id = m_next_history_operation_id++;
    HistoryOperation::Parameters operation_parameters = parameters.visit(
        [](BrowserHistoryTraversalOperation& parameters) -> HistoryOperation::Parameters { return move(parameters); },
        [](HistoryStepCancelationCheckOperation& parameters) -> HistoryOperation::Parameters { return move(parameters); });
    m_history_operations.set(operation_id, make<HistoryOperation>(operation_id, move(operation_parameters), Optional<u64> {}, nullptr, 0, Optional<i32> {}, move(on_complete)));
    auto* operation = find_history_operation(operation_id);
    VERIFY(operation);
    operation->queue_promise = promise;
    start_history_operation(*operation, promise);
}

void CanonicalTraversable::append_history_queue_steps(SessionHistoryTraversalSteps steps)
{
    m_history_traversal_queue.append_session_history_traversal_steps(move(steps));
}

void CanonicalTraversable::enqueue_history_operation(u64 initiation_id, Web::HistoryOperationParameters request, WebContentClient& requesting_client, u64 requesting_page_id, OnHistoryOperationComplete on_complete)
{
    // https://html.spec.whatwg.org/multipage/document-sequences.html#destroy-a-child-navigable
    // Steps 6-7 remove the nested history before step 9 appends traversal steps. Apply the canonical counterpart while
    // admitting the destruction request, rather than deferring it to the operation's eventual queue position.
    if (request.has<Web::NavigableDestructionHistoryOperationParameters>()) {
        auto const& parameters = request.get<Web::NavigableDestructionHistoryOperationParameters>();
        if (auto parent_navigable = find(parameters.parent_navigable_id); parent_navigable.has_value())
            remove_nested_history(*parent_navigable, parameters.parent_document_state_id, parameters.navigable_id);
    }

    Optional<Web::HTML::CrossProcessId> synchronous_navigation_target;
    if (request.has<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>())
        synchronous_navigation_target = request.get<Web::FinalizeSameDocumentNavigationHistoryOperationParameters>().navigable_id;

    NonnullRefPtr<WebContentClient> requesting_client_ref { requesting_client };
    auto steps = [this, initiation_id, request = move(request), requesting_client = move(requesting_client_ref), requesting_page_id, on_complete = move(on_complete)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
        run_history_operation_at_queue_position(initiation_id, move(request), *requesting_client, requesting_page_id, {}, move(on_complete), move(promise));
    };

    if (synchronous_navigation_target.has_value())
        m_history_traversal_queue.append_session_history_synchronous_navigation_steps(*synchronous_navigation_target, move(steps));
    else
        m_history_traversal_queue.append_session_history_traversal_steps(move(steps));
}

void CanonicalTraversable::enqueue_history_operation(BrowserHistoryTraversalOperation parameters, OnHistoryOperationComplete on_complete)
{
    auto steps = [this, parameters = move(parameters), on_complete = move(on_complete)](NonnullRefPtr<Core::Promise<Empty>> promise) mutable {
        run_ui_history_operation_at_queue_position(move(parameters), move(on_complete), move(promise));
    };
    m_history_traversal_queue.append_session_history_traversal_steps(move(steps));
}

void CanonicalTraversable::apply_history_step(HistoryOperation& operation, i32 step, bool check_for_cancelation, Optional<Web::HTML::CrossProcessId> initiator_to_check, Web::HTML::UserNavigationInvolvement user_involvement, Optional<Web::Bindings::NavigationType> navigation_type, Web::HTML::SynchronousNavigation synchronous_navigation, Optional<Web::HTML::CrossProcessId> navigable_with_finalized_entry)
{
    VERIFY(!operation.algorithm);
    operation.active_step = step;
    operation.user_involvement = user_involvement;
    auto operation_id = operation.operation_id;
    operation.algorithm = make<ApplyHistoryStep>(
        m_session_history, *this, m_history_traversal_queue, m_apply_history_step_traversable_state, create_apply_history_step_jobs(operation_id),
        step, check_for_cancelation, initiator_to_check, user_involvement, navigation_type, synchronous_navigation,
        navigable_with_finalized_entry,
        [this, operation_id](Web::HTML::HistoryStepResult result) {
            auto* operation = find_history_operation(operation_id);
            auto committed_step = operation && operation->algorithm ? operation->algorithm->committed_step() : Optional<i32> {};
            finish_history_operation(operation_id, result, committed_step);
        });
    operation.algorithm->apply_the_history_step();
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#update-for-navigable-creation/destruction
void CanonicalTraversable::update_for_navigable_creation_or_destruction(HistoryOperation& operation)
{
    // 1. Let step be traversable's current session history step.
    auto step = m_session_history.current_step();
    if (!step.has_value()) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
        return;
    }

    // 2. Return the result of applying the history step step to traversable given false, null, null, "none", and null.
    apply_history_step(operation, *step, false, {}, Web::HTML::UserNavigationInvolvement::None, {}, Web::HTML::SynchronousNavigation::No, {});
}

void CanonicalTraversable::check_history_step_cancelation(HistoryOperation& operation, HistoryStepCancelationCheckOperation const& parameters)
{
    auto operation_id = operation.operation_id;
    auto run_unload_cancelation_job = [this, operation_id] {
        auto* operation = find_history_operation(operation_id);
        if (!operation)
            return;
        auto const& parameters = operation->parameters.get<HistoryStepCancelationCheckOperation>();
        auto navigables_crossing_documents = m_session_history.get_all_navigables_that_might_experience_a_cross_document_traversal(*this, parameters.target_step);
        auto jobs = create_apply_history_step_jobs(operation_id);
        jobs.run_unload_cancelation_job(parameters.target_step, move(navigables_crossing_documents), parameters.user_involvement,
            [this, operation_id](Web::HTML::HistoryStepResult result) {
                finish_history_operation(operation_id, result, {});
            });
    };

    if (!parameters.initiator_to_check.has_value()) {
        run_unload_cancelation_job();
        return;
    }

    VERIFY(operation.initiation_id.has_value());
    // AD-HOC: ApplyHistoryStep normally derives this set while preparing its changing-navigable jobs. A
    // cancelation-only operation stops before those jobs, so derive the same target set here for the spec's
    // initiator sandboxing check.
    auto navigables = m_session_history.get_all_navigables_whose_current_session_history_entry_will_change_or_reload(*this, parameters.target_step);
    auto jobs = create_apply_history_step_jobs(operation_id);
    jobs.run_initiator_sandboxing_check_job(*parameters.initiator_to_check, move(navigables),
        [this, operation_id, run_unload_cancelation_job](ApplyHistoryStepJobs::InitiatorSandboxingCheckResult result) {
            if (result == ApplyHistoryStepJobs::InitiatorSandboxingCheckResult::Disallowed) {
                finish_history_operation(operation_id, Web::HTML::HistoryStepResult::InitiatorDisallowed, {});
                return;
            }
            run_unload_cancelation_job();
        });
}

void CanonicalTraversable::start_history_operation(HistoryOperation& operation, NonnullRefPtr<Core::Promise<Empty>>)
{
    if (!operation.initiating_client) {
        auto endpoint = history_job_endpoint_for(*this);
        operation.initiating_client = endpoint.client;
        operation.initiating_page_id = endpoint.page_id;
    }

    if (!operation.initiating_client) {
        finish_history_operation(operation.operation_id, Web::HTML::HistoryStepResult::CanceledByMissingPage, {});
        return;
    }

    add_history_operation_completion_endpoint(operation, {
                                                             operation.initiating_client,
                                                             operation.initiating_page_id,
                                                         },
        operation.initiation_id);

    operation.parameters.visit(
        [&](Web::HistoryOperationParameters const& parameters) {
            VERIFY(operation.initiation_id.has_value());
            parameters.visit(
                [&](Web::PushHistoryOperationParameters const& parameters) {
                    auto current_step = m_session_history.current_step();
                    VERIFY(current_step.has_value());
                    auto current_entry = m_session_history.current_entry();
                    auto replaces_provisional_entry = parameters.navigable_id == id()
                        && current_entry
                        && current_entry->document_state.is_provisional;

                    if (replaces_provisional_entry) {
                        operation.assigned_target_step = *current_step;
                        return;
                    }

                    auto maximum_claimed_step = maximum_claimed_session_history_step();
                    auto last_reserved_step = max(*current_step, maximum_claimed_step.value_or(*current_step));
                    VERIFY(last_reserved_step < NumericLimits<i32>::max());
                    operation.assigned_target_step = last_reserved_step + 1;
                },
                [&](Web::ReplaceHistoryOperationParameters const&) {
                    auto current_step = m_session_history.current_step();
                    VERIFY(current_step.has_value());
                    operation.assigned_target_step = *current_step;
                },
                [](auto const&) {});
            operation.initiating_client->async_history_operation_started(operation.initiating_page_id, operation.operation_id, *operation.initiation_id, operation.assigned_target_step);
        },
        [&](BrowserHistoryTraversalOperation& parameters) {
            operation.reconstructs_web_content_history = parameters.reconstructs_web_content_history
                || parameters.restores_replacement_process;
            operation.requires_process_replacement = parameters.requires_process_replacement;
            apply_history_step(operation, parameters.target_step, parameters.check_for_cancelation, {}, Web::HTML::UserNavigationInvolvement::BrowserUI, Web::Bindings::NavigationType::Traverse, Web::HTML::SynchronousNavigation::No,
                operation.reconstructs_web_content_history ? Optional<Web::HTML::CrossProcessId> { id() } : Optional<Web::HTML::CrossProcessId> {});
        },
        [&](HistoryStepCancelationCheckOperation const& parameters) {
            check_history_step_cancelation(operation, parameters);
        });
}

void CanonicalTraversable::did_receive_history_operation_ready(u64 operation_id, bool proceed, Optional<i32> step_override, Optional<Web::HTML::CrossProcessId> creation_parent_document_state_id, Web::HTML::HistoryStepResult abandon_result)
{
    auto* operation = find_history_operation(operation_id);
    if (!operation || operation->algorithm)
        return;

    if (!proceed) {
        finish_history_operation(operation_id, abandon_result, {});
        return;
    }

    auto apply_current_step = [&](bool check_for_cancelation, Web::HTML::UserNavigationInvolvement user_involvement,
                                  Optional<Web::Bindings::NavigationType> navigation_type, Optional<Web::HTML::CrossProcessId> navigable_with_finalized_entry) {
        auto step = m_session_history.current_step();
        if (!step.has_value()) {
            finish_history_operation(operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
            return;
        }
        apply_history_step(*operation, *step, check_for_cancelation, {}, user_involvement, navigation_type,
            Web::HTML::SynchronousNavigation::No, navigable_with_finalized_entry);
    };

    VERIFY(operation->parameters.has<Web::HistoryOperationParameters>());
    auto const& request = operation->parameters.get<Web::HistoryOperationParameters>();
    if (!request.has<Web::NavigableCreationHistoryOperationParameters>())
        VERIFY(!creation_parent_document_state_id.has_value());
    request.visit(
        [&](Web::PushHistoryOperationParameters const& parameters) {
            VERIFY(!step_override.has_value());
            VERIFY(operation->assigned_target_step.has_value());
            apply_history_step(*operation, *operation->assigned_target_step, false, {}, parameters.user_involvement, Web::Bindings::NavigationType::Push,
                Web::HTML::SynchronousNavigation::No, parameters.navigable_id);
        },
        [&](Web::ReplaceHistoryOperationParameters const& parameters) {
            VERIFY(!step_override.has_value());
            VERIFY(operation->assigned_target_step.has_value());
            apply_history_step(*operation, *operation->assigned_target_step, false, {}, parameters.user_involvement, Web::Bindings::NavigationType::Replace,
                Web::HTML::SynchronousNavigation::No, parameters.navigable_id);
        },
        [&](Web::ReloadHistoryOperationParameters const& parameters) {
            VERIFY(!step_override.has_value());
            apply_current_step(true, parameters.user_involvement, Web::Bindings::NavigationType::Reload, parameters.navigable_id);
        },
        [&](Web::TraverseByDeltaHistoryOperationParameters const& parameters) {
            VERIFY(!step_override.has_value());
            VERIFY(operation->resolved_step.has_value());
            apply_history_step(*operation, *operation->resolved_step, true, parameters.initiator_to_check, parameters.user_involvement, Web::Bindings::NavigationType::Traverse,
                Web::HTML::SynchronousNavigation::No, {});
        },
        [&](Web::TraverseToStepHistoryOperationParameters const& parameters) {
            VERIFY(!step_override.has_value());
            apply_history_step(*operation, parameters.target_step, true, {}, parameters.user_involvement, Web::Bindings::NavigationType::Traverse,
                Web::HTML::SynchronousNavigation::No, {});
        },
        [&](Web::NavigationAPITraverseHistoryOperationParameters const& parameters) {
            VERIFY(!step_override.has_value());
            VERIFY(operation->resolved_step.has_value());
            apply_history_step(*operation, *operation->resolved_step, true, parameters.navigable_id, parameters.user_involvement, Web::Bindings::NavigationType::Traverse,
                Web::HTML::SynchronousNavigation::No, {});
        },
        [&](Web::ResumeTraverseHistoryOperationParameters const& parameters) {
            VERIFY(!step_override.has_value());
            apply_history_step(*operation, parameters.target_step, false, {}, parameters.user_involvement, Web::Bindings::NavigationType::Traverse,
                Web::HTML::SynchronousNavigation::No, {});
        },
        [&](Web::NavigableCreationHistoryOperationParameters const& parameters) {
            VERIFY(!step_override.has_value());
            VERIFY(creation_parent_document_state_id.has_value());
            auto parent_navigable = find(parameters.parent_navigable_id);
            if (!parent_navigable.has_value()
                || !append_nested_history(*parent_navigable, *creation_parent_document_state_id, parameters.navigable_id, parameters.initial_history_entry).has_value()) {
                finish_history_operation(operation_id, Web::HTML::HistoryStepResult::NoMatchingEntry, {});
                return;
            }

            // Steps 1-6 of create-a-new-child-navigable's queued work are complete. Resume at step 7.
            update_for_navigable_creation_or_destruction(*operation);
        },
        [&](Web::NavigableDestructionHistoryOperationParameters const&) {
            VERIFY(!step_override.has_value());
            update_for_navigable_creation_or_destruction(*operation);
        },
        [&](Web::FinalizeSameDocumentNavigationHistoryOperationParameters const& parameters) {
            VERIFY(step_override.has_value());
            VERIFY(parameters.history_handling == Web::HTML::HistoryHandlingBehavior::Push || parameters.history_handling == Web::HTML::HistoryHandlingBehavior::Replace);
            auto navigation_type = parameters.history_handling == Web::HTML::HistoryHandlingBehavior::Replace
                ? Web::Bindings::NavigationType::Replace
                : Web::Bindings::NavigationType::Push;
            apply_history_step(*operation, *step_override, false, {}, parameters.user_involvement, navigation_type,
                Web::HTML::SynchronousNavigation::Yes, parameters.navigable_id);
        },
        [&](Web::CloseTopLevelTraversableHistoryOperationParameters const&) {
            // Close runs entirely in the requesting process at this queue position and must complete with proceed=false.
            VERIFY_NOT_REACHED();
        },
        [&](Web::ResetSessionHistoryForTestingOperationParameters const&) {
            VERIFY_NOT_REACHED();
        },
        [&](Web::FlushSessionHistoryTraversalQueueOperationParameters const&) {
            VERIFY_NOT_REACHED();
        });
}

void CanonicalTraversable::finish_history_operation(u64 operation_id, Web::HTML::HistoryStepResult result, Optional<i32> committed_step)
{
    auto operation = m_history_operations.take(operation_id);
    if (!operation.has_value())
        return;
    auto& taken_operation = **operation;

    for (auto& endpoint : taken_operation.completion_endpoints)
        endpoint.client->async_complete_history_operation(endpoint.page_id, operation_id, result, committed_step, endpoint.initiation_id);

    // The completion installs the committed step as WebContent's current session history step; record that in the
    // mirror bookkeeping, which used to learn it from a WebContent notification.
    if (committed_step.has_value()) {
        if (m_session_history.did_set_web_content_current_session_history_step(*committed_step))
            m_current_web_content_session_history_matches_mirror = m_session_history.web_content_history_matches_mirror();
    }

    if (taken_operation.on_complete)
        taken_operation.on_complete(result, committed_step);

    // NB: Resolving the queue promise can synchronously start the next queued operation.
    if (taken_operation.queue_promise)
        taken_operation.queue_promise->resolve({});

    // The completion callback that brought us here can be running inside the algorithm object; destroy the
    // operation only once the stack has unwound.
    Core::deferred_invoke([operation = operation.release_value()] { });
}

void CanonicalTraversable::abandon_history_operations()
{
    while (!m_history_operations.is_empty()) {
        auto operation_id = m_history_operations.begin()->key;
        finish_history_operation(operation_id, Web::HTML::HistoryStepResult::CanceledByMissingPage, {});
    }
}

void CanonicalTraversable::did_receive_initiator_sandboxing_check_result(u64 operation_id, bool allowed)
{
    if (auto* operation = find_history_operation(operation_id)) {
        if (auto pending = move(operation->pending_sandboxing_check))
            pending(allowed ? ApplyHistoryStepJobs::InitiatorSandboxingCheckResult::Allowed : ApplyHistoryStepJobs::InitiatorSandboxingCheckResult::Disallowed);
    }
}

void CanonicalTraversable::did_receive_history_step_unload_cancelation_result(u64 operation_id, Web::HTML::HistoryStepResult result)
{
    if (auto* operation = find_history_operation(operation_id)) {
        if (result == Web::HTML::HistoryStepResult::NoMatchingEntry) {
            if (operation->parameters.has<HistoryStepCancelationCheckOperation>()) {
                result = Web::HTML::HistoryStepResult::Applied;
            } else if (is_history_traversal_operation(*operation)) {
                VERIFY(operation->active_step.has_value());
                auto step = *operation->active_step;
                auto user_involvement = operation->user_involvement;
                operation->reconstructs_web_content_history = true;
                operation->requires_process_replacement = true;
                VERIFY(operation->algorithm);
                operation->pending_unload_cancelation = nullptr;
                operation->algorithm = nullptr;
                Core::deferred_invoke([this, operation_id, step, user_involvement] {
                    auto* operation = find_history_operation(operation_id);
                    if (!operation)
                        return;
                    apply_history_step(*operation, step, false, {}, user_involvement,
                        Web::Bindings::NavigationType::Traverse,
                        Web::HTML::SynchronousNavigation::No, id());
                });
                return;
            }
        }
        if (auto pending = move(operation->pending_unload_cancelation))
            pending(result);
    }
}

void CanonicalTraversable::did_receive_changing_navigable_history_job_ready(u64 operation_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition disposition)
{
    if (auto* operation = find_history_operation(operation_id)) {
        if (auto pending = operation->pending_changing_jobs.take(navigable_id); pending.has_value())
            (*pending)(disposition);
    }
}

void CanonicalTraversable::did_receive_changing_navigable_continuation_applied(u64 operation_id, Web::HTML::CrossProcessId navigable_id)
{
    if (auto* operation = find_history_operation(operation_id)) {
        if (auto pending = operation->pending_continuations.take(navigable_id); pending.has_value()) {
            // A replacement document's creation operation also applies a top-level continuation, but it runs before
            // the document has accepted the UI-owned history state. Only the browser traversal itself reaches the
            // observable top-level completion point here.
            auto is_top_level_pending_browser_history_traversal = navigable_id == id()
                && operation->parameters.has<BrowserHistoryTraversalOperation>()
                && m_pending_session_history_traversal.has_value()
                && operation->parameters.get<BrowserHistoryTraversalOperation>().target_step == m_pending_session_history_traversal->target_step;
            if (is_top_level_pending_browser_history_traversal)
                notify_top_level_traversal_applied();
            (*pending)();
        }
    }
}

void CanonicalTraversable::did_receive_nonchanging_navigable_history_state_updated(u64 operation_id, Web::HTML::CrossProcessId navigable_id)
{
    if (auto* operation = find_history_operation(operation_id)) {
        if (auto pending = operation->pending_nonchanging_updates.take(navigable_id); pending.has_value())
            (*pending)();
    }
}

}
