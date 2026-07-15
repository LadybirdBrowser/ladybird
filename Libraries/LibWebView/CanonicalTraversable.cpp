/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/Application.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/SiteIsolationManager.h>
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

static Optional<size_t> current_top_level_history_entry_index_for_step(Vector<Web::HTML::SessionHistoryEntryDescriptor> const& entries, Optional<i32> current_step)
{
    if (!current_step.has_value())
        return {};

    Optional<size_t> current_entry_index;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (!entries[i].document_state.navigable_target_name.is_empty())
            continue;

        if (entries[i].step <= *current_step)
            current_entry_index = i;
        if (entries[i].step >= *current_step)
            break;
    }
    return current_entry_index;
}

void CanonicalTraversable::abandon_pending_web_content_session_history_seed()
{
    m_session_history_entry_url_loading_from_ui_process.clear();
    m_pending_web_content_session_history_seed.clear();
}

void CanonicalTraversable::prepare_to_seed_web_content_session_history_from_ui_process()
{
    m_current_web_content_session_history_matches_mirror = false;
    m_session_history.forget_web_content_state();
    m_pending_session_history_navigation.clear();
    m_pending_web_content_session_history_seed.clear();
    m_pending_web_content_session_history_seed.step_after_loading_top_level_entry = m_session_history.current_step_to_restore_after_loading_top_level_entry();
    m_pending_web_content_session_history_seed.should_send_entries = true;
    m_pending_web_content_session_history_seed.ignore_updates_until_seed = true;
}

static bool can_seed_replacement_process_before_load(TraversableSessionHistory const& session_history, Optional<URL::URL> const& session_history_entry_url_loading_from_ui_process, PendingWebContentSessionHistorySeed const& pending_web_content_session_history_seed)
{
    if (!pending_web_content_session_history_seed.should_send_entries)
        return false;
    if (session_history_entry_url_loading_from_ui_process.has_value())
        return false;
    if (session_history.current_step_to_restore_after_loading_top_level_entry().has_value())
        return false;
    return true;
}

ProcessSwapNavigationPreparation CanonicalTraversable::prepare_for_process_swap_navigation(URL::URL const& url, Web::HTML::DocumentResource document_resource, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    ProcessSwapNavigationPreparation result;

    auto ui_session_history_already_points_to_url = false;
    if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url)
        ui_session_history_already_points_to_url = true;

    if (m_pending_session_history_traversal.has_value() && m_pending_session_history_traversal->will_replace_web_content_process)
        m_pending_session_history_traversal->stage = PendingSessionHistoryTraversal::Stage::ReplacingWebContentProcess;
    if (m_pending_session_history_navigation.has_value())
        m_pending_session_history_navigation->web_content_restore_mode = PendingSessionHistoryNavigation::WebContentRestoreMode::RestoreFromUIProcess;
    m_current_web_content_session_history_matches_mirror = false;
    m_session_history.forget_web_content_state();
    m_pending_web_content_session_history_seed.waiting_for_ack = false;
    m_pending_web_content_session_history_seed.should_send_entries = true;
    m_pending_web_content_session_history_seed.ignore_updates_until_seed = true;

    if (!ui_session_history_already_points_to_url && !m_session_history_entry_url_loading_from_ui_process.has_value()) {
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

    if (!m_session_history_entry_url_loading_from_ui_process.has_value())
        m_pending_web_content_session_history_seed.step_after_loading_top_level_entry = m_session_history.current_step_to_restore_after_loading_top_level_entry();

    result.should_seed_web_content_before_load = can_seed_replacement_process_before_load(m_session_history, m_session_history_entry_url_loading_from_ui_process, m_pending_web_content_session_history_seed);
    return result;
}

PageLoadPreparation CanonicalTraversable::prepare_for_page_load(URL::URL const& url, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    PageLoadPreparation result;

    if (m_session_history_entry_url_loading_from_ui_process.has_value())
        return result;

    abandon_pending_web_content_session_history_seed();
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
    abandon_pending_web_content_session_history_seed();
    m_current_web_content_session_history_matches_mirror = false;
    m_session_history.forget_web_content_state();
}

void CanonicalTraversable::prepare_for_reload()
{
    abandon_pending_web_content_session_history_seed();
    m_session_history.mark_current_entry_reload_pending();
    m_current_web_content_session_history_matches_mirror = false;
}

WebContentSessionHistoryUpdateDecision CanonicalTraversable::did_receive_web_content_session_history_update(Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index, URL::URL const& current_url)
{
    if (m_pending_web_content_session_history_seed.waiting_for_ack)
        return { .ignore_reason = "ignored-session-history-before-ui-seed-ack"sv };

    auto pending_step_after_fallback_load_was_restored = false;
    if (m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.has_value()) {
        if (current_used_step_index >= used_steps.size() || used_steps[current_used_step_index] != *m_pending_web_content_session_history_seed.step_after_loading_top_level_entry)
            return { .ignore_reason = "ignored-partial-session-history-before-fallback-seed"sv };
        pending_step_after_fallback_load_was_restored = true;
    }

    if (m_pending_web_content_session_history_seed.ignore_updates_until_seed)
        return { .ignore_reason = "ignored-session-history-before-ui-seed"sv };

    return {
        .update = update_session_history_from_web_content(move(entries), move(used_steps), current_used_step_index, pending_step_after_fallback_load_was_restored, true, current_url),
    };
}

WebContentSessionHistoryUpdateDecision CanonicalTraversable::did_receive_web_content_session_history_update_for_testing(Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index, URL::URL const& current_url)
{
    // NB: dumpUIProcessSessionHistory() first sends WebContent's current snapshot to the UI process, then returns
    //     the UI mirror. If a stale seed ack is still pending, normal async snapshots are intentionally ignored, so
    //     use the same convergence path as a rejected seed ack to make this testing hook deterministic.
    if (m_pending_web_content_session_history_seed.waiting_for_ack) {
        auto update = adopt_web_content_session_history_after_rejected_seed(move(entries), move(used_steps), current_used_step_index, current_url);
        if (update.update_result == TraversableSessionHistory::UpdateResult::InvalidSnapshot)
            return { .ignore_reason = "ignored-session-history-for-testing-before-ui-seed-ack"sv };
        return { .update = move(update) };
    }

    return {
        .update = update_session_history_from_web_content(move(entries), move(used_steps), current_used_step_index, false, true, current_url),
    };
}

bool CanonicalTraversable::update_session_history_entry_navigation_api_state(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, Web::HTML::StorageSerializationRecord navigation_api_state)
{
    VERIFY(&navigable.top_level_traversable() == this);

    if (&navigable == this)
        return m_session_history.update_top_level_navigation_api_state(navigation_api_key, move(navigation_api_state));
    return m_session_history.update_nested_navigation_api_state(navigable.id(), navigation_api_key, move(navigation_api_state));
}

bool CanonicalTraversable::update_session_history_entry_scroll_restoration_mode(CanonicalNavigable const& navigable, Utf16String const& navigation_api_key, Web::HTML::ScrollRestorationMode scroll_restoration_mode)
{
    VERIFY(&navigable.top_level_traversable() == this);

    if (&navigable == this)
        return m_session_history.update_top_level_scroll_restoration_mode(navigation_api_key, scroll_restoration_mode);
    return m_session_history.update_nested_scroll_restoration_mode(navigable.id(), navigation_api_key, scroll_restoration_mode);
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

WebContentSessionHistoryUpdateResult CanonicalTraversable::update_session_history_from_web_content(Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index, bool pending_step_after_fallback_load_was_restored, bool seed_web_content_on_invalid_snapshot, URL::URL const& current_url)
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
        if (pending_step_after_fallback_load_was_restored)
            m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.clear();
    } else if (seed_web_content_on_invalid_snapshot) {
        if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == current_url) {
            prepare_to_seed_web_content_session_history_from_ui_process();
            result.should_seed_web_content = true;
        }
    }

    return result;
}

WebContentSessionHistoryUpdateResult CanonicalTraversable::adopt_web_content_session_history_after_rejected_seed(Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index, URL::URL const& current_url)
{
    if (entries.is_empty())
        return {};

    auto entries_from_web_content = entries;
    auto used_steps_from_web_content = used_steps;
    auto update = update_session_history_from_web_content(move(entries), move(used_steps), current_used_step_index, false, false, current_url);
    if (update.update_result == TraversableSessionHistory::UpdateResult::InvalidSnapshot && current_used_step_index < used_steps_from_web_content.size()) {
        auto current_top_level_entry_index = current_top_level_history_entry_index_for_step(entries_from_web_content, used_steps_from_web_content[current_used_step_index]);
        if (current_top_level_entry_index.has_value() && entries_from_web_content[*current_top_level_entry_index].url == current_url) {
            m_session_history.clear();
            update = update_session_history_from_web_content(move(entries_from_web_content), move(used_steps_from_web_content), current_used_step_index, false, false, current_url);
        }
    }
    if (update.update_result == TraversableSessionHistory::UpdateResult::InvalidSnapshot)
        return update;

    m_pending_web_content_session_history_seed.clear();
    m_pending_session_history_traversal.clear();
    return update;
}

WebContentSessionHistorySeedAckResult CanonicalTraversable::did_receive_web_content_session_history_seed_ack(bool accepted, Vector<Web::HTML::SessionHistoryEntryDescriptor> entries, Vector<i32> used_steps, size_t current_used_step_index, URL::URL const& current_url)
{
    if (!m_pending_web_content_session_history_seed.waiting_for_ack)
        return { .ignored = true, .dump_reason = "ignored-webcontent-session-history-seed-ack"sv };

    WebContentSessionHistorySeedAckResult result;
    result.should_update_navigation_action_state = true;

    if (!accepted) {
        auto update = adopt_web_content_session_history_after_rejected_seed(move(entries), move(used_steps), current_used_step_index, current_url);
        if (update.update_result != TraversableSessionHistory::UpdateResult::InvalidSnapshot) {
            result.dump_reason = "webcontent-session-history-seed-rejected-with-current-snapshot"sv;
            result.current_url = move(update.current_url);
            // NB: Applying the adopted snapshot's current URL already refreshes the navigation actions.
            result.should_update_navigation_action_state = false;
            return result;
        }

        abandon_pending_web_content_session_history_seed();
        m_current_web_content_session_history_matches_mirror = false;
        m_session_history.forget_web_content_state();
        m_pending_session_history_traversal.clear();
        result.dump_reason = "webcontent-session-history-seed-rejected"sv;
        return result;
    }

    if (!m_session_history.did_seed_web_content_from_ui_process(move(entries), move(used_steps), current_used_step_index)) {
        if (m_pending_web_content_session_history_seed.should_reseed_after_current_history_load) {
            m_pending_web_content_session_history_seed.waiting_for_ack = false;
            m_pending_web_content_session_history_seed.should_send_entries = true;
            m_pending_web_content_session_history_seed.ignore_updates_until_seed = true;
            m_current_web_content_session_history_matches_mirror = false;
            result.dump_reason = "webcontent-session-history-preload-seed-ack-mismatch"sv;
            return result;
        }

        abandon_pending_web_content_session_history_seed();
        m_current_web_content_session_history_matches_mirror = false;
        m_session_history.forget_web_content_state();
        m_pending_session_history_traversal.clear();
        result.dump_reason = "webcontent-session-history-seed-ack-mismatch"sv;
        return result;
    }

    m_pending_web_content_session_history_seed.waiting_for_ack = false;
    if (m_pending_web_content_session_history_seed.should_reseed_after_current_history_load) {
        m_pending_web_content_session_history_seed.should_send_entries = true;
        m_pending_web_content_session_history_seed.ignore_updates_until_seed = true;
        m_current_web_content_session_history_matches_mirror = false;
        result.dump_reason = "webcontent-session-history-preload-seed-ack"sv;
        return result;
    }

    m_pending_web_content_session_history_seed.ignore_updates_until_seed = false;
    m_current_web_content_session_history_matches_mirror = !m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.has_value()
        && !m_pending_session_history_navigation.has_value();
    if (m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.has_value()) {
        if (m_pending_session_history_traversal.has_value())
            m_pending_session_history_traversal->stage = PendingSessionHistoryTraversal::Stage::RestoringNestedStepAfterSeed;
        result.step_to_traverse = *m_pending_web_content_session_history_seed.step_after_loading_top_level_entry;
    } else {
        auto is_waiting_for_history_step_cancelation_check = m_pending_session_history_traversal.has_value()
            && m_pending_session_history_traversal->stage == PendingSessionHistoryTraversal::Stage::CheckingCancelation;
        if (!is_waiting_for_history_step_cancelation_check) {
            m_pending_session_history_traversal.clear();
            result.should_complete_webdriver_pending_navigation = !m_pending_session_history_navigation.has_value();
        }
    }

    result.dump_reason = "webcontent-session-history-seed-ack"sv;
    return result;
}

NavigationStartResult CanonicalTraversable::did_start_navigation(URL::URL const& url, Web::HTML::DocumentResource document_resource, bool is_redirect, Web::Bindings::NavigationHistoryBehavior history_handling, bool is_showing_crash_page)
{
    if (m_session_history_entry_url_loading_from_ui_process.has_value()) {
        if (*m_session_history_entry_url_loading_from_ui_process != url)
            return { .dump_reason = "ignored-stale-ui-history-load-start"sv };

        auto should_keep_preseeded_web_content_history = m_pending_web_content_session_history_seed.waiting_for_ack || m_session_history.web_content_uses_ui_step_coordinates();
        m_session_history_entry_url_loading_from_ui_process.clear();
        if (!should_keep_preseeded_web_content_history) {
            m_current_web_content_session_history_matches_mirror = false;
            m_session_history.forget_web_content_state();
        }
        return { .dump_reason = "did-start-navigation-from-ui-history-load"sv };
    }

    if (m_pending_web_content_session_history_seed.should_send_entries || m_pending_web_content_session_history_seed.ignore_updates_until_seed || m_pending_web_content_session_history_seed.waiting_for_ack) {
        if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url != url)
            return { .dump_reason = "ignored-navigation-start-before-ui-history-seed"sv };
    }

    if (is_showing_crash_page) {
        if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url) {
            prepare_to_seed_web_content_session_history_from_ui_process();
            return { .dump_reason = "did-start-navigation-from-crash-page"sv, .did_clear_crash_page = true };
        }
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

    if (m_session_history_entry_url_loading_from_ui_process.has_value() && *m_session_history_entry_url_loading_from_ui_process == url) {
        m_session_history_entry_url_loading_from_ui_process.clear();
        abandon_pending_web_content_session_history_seed();
        m_current_web_content_session_history_matches_mirror = false;
        m_session_history.forget_web_content_state();
        return { .status = NavigationCancelStatus::CanceledUIHistoryLoad };
    }

    if (has_webdriver_pending_navigation) {
        m_session_history.clear_current_entry_reload_pending();
        return { .status = NavigationCancelStatus::CompleteWebdriverPendingNavigation };
    }

    return {};
}

NavigationFinishResult CanonicalTraversable::did_finish_navigation(URL::URL const& url)
{
    if (m_pending_session_history_navigation.has_value() && m_pending_session_history_navigation->url == url)
        m_pending_session_history_navigation.clear();

    if (!m_pending_web_content_session_history_seed.should_send_entries)
        return {};

    if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url) {
        m_session_history.clear_current_entry_reload_pending();
        auto allow_current_entry_reconstruction = m_pending_web_content_session_history_seed.should_reseed_after_current_history_load;
        m_pending_web_content_session_history_seed.should_reseed_after_current_history_load = false;
        return { .should_seed_web_content = true, .allow_current_entry_reconstruction = allow_current_entry_reconstruction };
    }

    // NB: The first finish notification from a fresh WebContent process can still report about:blank before the
    //     traversed-to entry is ready. Keep the pending seed state intact so partial snapshots remain ignored
    //     until we can seed the full UI-owned history.
    return { .dump_reason = "skip-seed-webcontent-session-history"sv };
}

RestorePendingSessionHistoryNavigationResult CanonicalTraversable::restore_pending_session_history_navigation()
{
    if (!m_pending_session_history_navigation.has_value())
        return {};

    auto web_content_restore_mode = m_pending_session_history_navigation->web_content_restore_mode;
    m_session_history = move(m_pending_session_history_navigation->previous_session_history);
    m_pending_session_history_navigation.clear();
    m_pending_session_history_traversal.clear();

    RestorePendingSessionHistoryNavigationResult result { .restored = true, .web_content_restore_mode = web_content_restore_mode };
    if (auto* current_entry = m_session_history.current_entry()) {
        result.current_url = current_entry->url;
        if (web_content_restore_mode == PendingSessionHistoryNavigation::WebContentRestoreMode::PreserveCurrentProcessState) {
            m_session_history_entry_url_loading_from_ui_process.clear();
            abandon_pending_web_content_session_history_seed();
            m_current_web_content_session_history_matches_mirror = m_session_history.web_content_history_matches_mirror();
        }
    } else {
        m_current_web_content_session_history_matches_mirror = false;
    }
    return result;
}

HistoryTraversalDecision CanonicalTraversable::traverse_the_history_by_delta(int delta, CheckForCancelation check_for_cancelation, URL::URL const& current_url, Function<void(HistoryTraversalOutcome)> on_cancelation_check_complete)
{
    auto target = m_session_history.traversal_target_for_delta(delta);
    if (!target.has_value())
        return { .outcome = { .status = HistoryTraversalStatus::NoEntry } };

    return traverse_the_history(*target, check_for_cancelation, current_url, move(on_cancelation_check_complete));
}

HistoryTraversalDecision CanonicalTraversable::traverse_the_history_to_step(i32 step, CheckForCancelation check_for_cancelation, URL::URL const& current_url, Function<void(HistoryTraversalOutcome)> on_cancelation_check_complete)
{
    auto target = m_session_history.traversal_target_for_step(step);
    if (!target.has_value())
        return { .outcome = { .status = HistoryTraversalStatus::NoEntry } };

    return traverse_the_history(*target, check_for_cancelation, current_url, move(on_cancelation_check_complete));
}

HistoryTraversalDecision CanonicalTraversable::traverse_the_history(TraversableSessionHistory::TraversalTarget const& target, CheckForCancelation check_for_cancelation, URL::URL const& current_url, Function<void(HistoryTraversalOutcome)> on_cancelation_check_complete)
{
    // FIXME: This pre-flight prediction exists only because WebContent applies the history step itself, so the UI must
    //        choose between delegating the traversal to the current process and driving a cross-process load before
    //        sending anything. Once the UI process owns apply-the-history-step and issues per-navigable load commands,
    //        placement is decided per command and this prediction goes away.
    auto will_replace_web_content_process = SiteIsolationManager::the().navigation_requires_process_swap(current_url, target.target_top_level_entry->url);
    auto pending_traversal = PendingSessionHistoryTraversal {
        .target_step = target.target_step,
        .target_step_index = target.target_step_index,
        .will_change_top_level_entry = target.changes_top_level_entry,
        .will_replace_web_content_process = will_replace_web_content_process,
        .on_cancelation_check_complete = nullptr,
    };

    auto web_content_can_apply_traversal = !m_pending_web_content_session_history_seed.should_send_entries
        && !m_pending_web_content_session_history_seed.ignore_updates_until_seed
        && !m_pending_web_content_session_history_seed.waiting_for_ack
        && !m_session_history_entry_url_loading_from_ui_process.has_value()
        && !m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.has_value()
        && m_session_history.web_content_can_traverse_to(target);

    if (web_content_can_apply_traversal && !will_replace_web_content_process) {
        m_pending_session_history_traversal = move(pending_traversal);
        auto webdriver_pending_navigation_completes_with_session_history_update = false;
        if (auto const* current_entry = m_session_history.current_entry()) {
            webdriver_pending_navigation_completes_with_session_history_update = current_entry->document_state.id == target.target_top_level_entry->document_state.id;
        }
        return {
            .outcome = { .status = HistoryTraversalStatus::Started, .will_replace_web_content_process = will_replace_web_content_process, .will_change_top_level_entry = target.changes_top_level_entry },
            .action = HistoryTraversalAction::TraverseInWebContent,
            .target_step = target.target_step,
            .webdriver_pending_navigation_url = target.target_top_level_entry->url,
            .webdriver_pending_navigation_completes_with_session_history_update = webdriver_pending_navigation_completes_with_session_history_update,
        };
    }

    auto needs_cancelation_check = check_for_cancelation == CheckForCancelation::Yes
        || (check_for_cancelation == CheckForCancelation::IfWebContentCannotTraverseTarget && !web_content_can_apply_traversal);
    if (needs_cancelation_check) {
        pending_traversal.stage = PendingSessionHistoryTraversal::Stage::CheckingCancelation;
        pending_traversal.cancelation_check_request_id = m_next_traverse_history_step_cancelation_check_request_id++;
        pending_traversal.on_cancelation_check_complete = move(on_cancelation_check_complete);
        auto request_id = pending_traversal.cancelation_check_request_id;
        m_pending_session_history_traversal = move(pending_traversal);
        return {
            .outcome = { .status = HistoryTraversalStatus::Started, .will_replace_web_content_process = will_replace_web_content_process, .will_change_top_level_entry = target.changes_top_level_entry, .waiting_for_cancelation_check = true },
            .action = HistoryTraversalAction::CheckForCancelation,
            .target_step = target.target_step,
            .cancelation_check_request_id = request_id,
        };
    }

    pending_traversal.stage = PendingSessionHistoryTraversal::Stage::LoadingEntryFromUIProcess;
    m_pending_session_history_traversal = move(pending_traversal);
    prepare_to_load_session_history_traversal_target_from_ui_process(target, current_url);
    return {
        .outcome = { .status = HistoryTraversalStatus::Started, .will_replace_web_content_process = will_replace_web_content_process, .will_change_top_level_entry = target.changes_top_level_entry },
        .action = HistoryTraversalAction::LoadCurrentEntryFromUIProcess,
        .webdriver_pending_navigation_url = target.target_top_level_entry->url,
        .webdriver_pending_navigation_completes_with_session_history_update = true,
    };
}

URL::URL CanonicalTraversable::prepare_to_load_session_history_traversal_target_from_ui_process(TraversableSessionHistory::TraversalTarget const& target, URL::URL const& current_url)
{
    if (!m_pending_session_history_traversal.has_value() || m_pending_session_history_traversal->target_step != target.target_step) {
        m_pending_session_history_traversal = PendingSessionHistoryTraversal {
            .target_step = target.target_step,
            .target_step_index = target.target_step_index,
            .will_change_top_level_entry = target.changes_top_level_entry,
            .will_replace_web_content_process = SiteIsolationManager::the().navigation_requires_process_swap(current_url, target.target_top_level_entry->url),
            .stage = PendingSessionHistoryTraversal::Stage::LoadingEntryFromUIProcess,
            .on_cancelation_check_complete = nullptr,
        };
    } else {
        m_pending_session_history_traversal->stage = PendingSessionHistoryTraversal::Stage::LoadingEntryFromUIProcess;
    }

    auto target_url = target.target_top_level_entry->url;
    auto previous_session_history = m_session_history;
    m_session_history.traverse_to(target.target_step_index);
    prepare_to_seed_web_content_session_history_from_ui_process();
    m_pending_session_history_navigation = PendingSessionHistoryNavigation { target_url, move(previous_session_history) };
    return target_url;
}

WebContentHistoryStepResult CanonicalTraversable::did_traverse_the_history_to_step(i32 step, bool step_was_available, Web::HTML::HistoryStepResult result)
{
    if (!m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.has_value()) {
        if (!m_pending_session_history_traversal.has_value() || m_pending_session_history_traversal->target_step != step)
            return { .dump_reason = "ignored-stale-webcontent-history-step-result"sv };

        if (!step_was_available) {
            auto target = m_session_history.traversal_target_for_step(step);
            if (target.has_value())
                return { .dump_reason = "webcontent-history-step-unavailable-fallback-load"sv, .fallback_target = *target };
            m_current_web_content_session_history_matches_mirror = false;
            m_session_history.forget_web_content_state();
            m_pending_session_history_traversal.clear();
            return { .dump_reason = "webcontent-history-step-unavailable"sv, .should_update_navigation_action_state = true };
        }

        if (result != Web::HTML::HistoryStepResult::Applied) {
            m_pending_session_history_traversal.clear();
            return { .dump_reason = "webcontent-history-step-canceled"sv, .should_update_navigation_action_state = true, .should_complete_webdriver_pending_navigation = true, .should_update_webdriver_pending_navigation_to_current_url = true, .should_reset_webdriver_pending_navigation_completion = true };
        }

        if (!m_session_history.did_apply_web_content_traversal_to_step(step)) {
            if (auto target = m_session_history.traversal_target_for_step(step); target.has_value())
                return { .dump_reason = "webcontent-history-step-applied-with-stale-mirror-fallback-load"sv, .fallback_target = *target };
            m_current_web_content_session_history_matches_mirror = false;
            m_session_history.forget_web_content_state();
            m_pending_session_history_traversal.clear();
            return { .dump_reason = "webcontent-history-step-applied-without-ui-target"sv, .should_update_navigation_action_state = true };
        }

        m_current_web_content_session_history_matches_mirror = true;
        auto should_complete_webdriver_pending_navigation = !m_pending_session_history_traversal->will_change_top_level_entry;
        Optional<URL::URL> current_url;
        if (auto const* current_entry = m_session_history.current_entry())
            current_url = current_entry->url;
        m_pending_session_history_traversal.clear();
        return { .dump_reason = "webcontent-history-step-applied"sv, .should_update_navigation_action_state = true, .current_url = move(current_url), .should_complete_webdriver_pending_navigation = should_complete_webdriver_pending_navigation };
    }

    if (*m_pending_web_content_session_history_seed.step_after_loading_top_level_entry != step)
        return { .dump_reason = "ignored-stale-webcontent-history-step-result"sv };

    if (step_was_available && result == Web::HTML::HistoryStepResult::Applied) {
        m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.clear();
        m_current_web_content_session_history_matches_mirror = m_session_history.did_restore_web_content_to_current_step(step);
        m_pending_session_history_traversal.clear();
        return { .dump_reason = "webcontent-history-step-restored"sv, .should_update_navigation_action_state = true, .should_complete_webdriver_pending_navigation = true };
    }

    auto pending_step_dump_reason = step_was_available ? "webcontent-pending-history-step-canceled"sv : "webcontent-history-step-unavailable"sv;
    if (m_pending_session_history_navigation.has_value())
        return { .dump_reason = pending_step_dump_reason, .should_restore_pending_navigation = true };

    m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.clear();
    m_current_web_content_session_history_matches_mirror = false;
    m_session_history.forget_web_content_state();
    m_pending_session_history_traversal.clear();
    return { .dump_reason = pending_step_dump_reason, .should_update_navigation_action_state = true };
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
        .dump_reason = "traverse-fallback-load-after-cancelation-check"sv,
        .on_cancelation_check_complete = move(on_cancelation_check_complete),
        .outcome = { .status = HistoryTraversalStatus::Started, .will_replace_web_content_process = m_pending_session_history_traversal->will_replace_web_content_process, .will_change_top_level_entry = m_pending_session_history_traversal->will_change_top_level_entry },
        .target = *target,
    };
}

Optional<WebContentSessionHistorySeed> CanonicalTraversable::prepare_web_content_session_history_seed(bool allow_current_entry_reconstruction)
{
    auto current_top_level_entry_index = m_session_history.current_top_level_entry_index();
    if (!current_top_level_entry_index.has_value()) {
        abandon_pending_web_content_session_history_seed();
        m_current_web_content_session_history_matches_mirror = false;
        m_session_history.forget_web_content_state();
        return {};
    }

    auto entries = m_session_history.entries();
    if (entries.is_empty()) {
        abandon_pending_web_content_session_history_seed();
        m_current_web_content_session_history_matches_mirror = false;
        m_session_history.forget_web_content_state();
        return {};
    }

    auto is_restoring_traversal_target = m_pending_session_history_traversal.has_value()
        && (m_pending_session_history_traversal->stage == PendingSessionHistoryTraversal::Stage::LoadingEntryFromUIProcess
            || m_pending_session_history_traversal->stage == PendingSessionHistoryTraversal::Stage::ReplacingWebContentProcess
            || m_pending_session_history_traversal->stage == PendingSessionHistoryTraversal::Stage::RestoringNestedStepAfterSeed);
    auto allow_reconstructing_current_entry = is_restoring_traversal_target
        || m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.has_value()
        || allow_current_entry_reconstruction;

    return WebContentSessionHistorySeed {
        .entries = move(entries),
        .current_top_level_entry_index = *current_top_level_entry_index,
        .allow_current_entry_reconstruction = allow_reconstructing_current_entry,
    };
}

void CanonicalTraversable::did_send_web_content_session_history_seed()
{
    m_pending_web_content_session_history_seed.waiting_for_ack = true;
    m_pending_web_content_session_history_seed.should_send_entries = false;
}

bool CanonicalTraversable::prepare_to_restore_current_session_history_entry_from_ui_process()
{
    auto should_seed = !m_pending_web_content_session_history_seed.step_after_loading_top_level_entry.has_value();
    if (should_seed)
        m_pending_web_content_session_history_seed.should_reseed_after_current_history_load = true;
    return should_seed;
}

CurrentSessionHistoryEntryLoad CanonicalTraversable::prepare_current_session_history_entry_load(URL::URL const& current_url)
{
    auto const* current_entry = m_session_history.current_entry();
    if (!current_entry) {
        m_session_history_entry_url_loading_from_ui_process = current_url;
        return { .url = current_url, .document_resource = Empty {}, .history_handling = Web::Bindings::NavigationHistoryBehavior::Auto };
    }

    m_session_history_entry_url_loading_from_ui_process = current_entry->url;
    auto history_handling = m_pending_web_content_session_history_seed.waiting_for_ack || m_session_history.web_content_uses_ui_step_coordinates()
        ? Web::Bindings::NavigationHistoryBehavior::Replace
        : Web::Bindings::NavigationHistoryBehavior::Auto;
    return { .url = current_entry->url, .document_resource = current_entry->document_state.resource, .history_handling = history_handling };
}

void CanonicalTraversable::did_crash_requiring_web_content_session_history_seed()
{
    m_session_history_entry_url_loading_from_ui_process.clear();
    prepare_to_seed_web_content_session_history_from_ui_process();
}

void CanonicalTraversable::reset_session_history_for_testing()
{
    m_session_history.clear();
    m_current_web_content_session_history_matches_mirror = false;
    m_pending_session_history_navigation.clear();
    m_pending_session_history_traversal.clear();
    m_session_history_entry_url_loading_from_ui_process.clear();
    abandon_pending_web_content_session_history_seed();
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
    case PendingSessionHistoryTraversal::Stage::LoadingEntryFromUIProcess:
        return "loading-entry-from-ui-process"sv;
    case PendingSessionHistoryTraversal::Stage::ReplacingWebContentProcess:
        return "replacing-webcontent-process"sv;
    case PendingSessionHistoryTraversal::Stage::RestoringNestedStepAfterSeed:
        return "restoring-nested-step-after-seed"sv;
    }
    VERIFY_NOT_REACHED();
}

}
