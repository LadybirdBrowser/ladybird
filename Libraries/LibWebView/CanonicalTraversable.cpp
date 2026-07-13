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

Optional<CanonicalNavigable&> CanonicalTraversable::find(Web::HTML::CrossProcessId frame_id)
{
    auto navigable = m_navigable_index.get(frame_id);
    if (!navigable.has_value() || !navigable.value())
        return {};

    return *navigable.value();
}

Optional<CanonicalNavigable const&> CanonicalTraversable::find(Web::HTML::CrossProcessId frame_id) const
{
    auto navigable = m_navigable_index.get(frame_id);
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

Web::HTML::SessionHistoryEpoch CanonicalTraversable::begin_new_web_content_session_history_epoch()
{
    VERIFY(m_web_content_session_history_epoch + 1 != 0);
    ++m_web_content_session_history_epoch;
    m_last_applied_web_content_session_history_mutation_id = 0;
    m_last_handled_web_content_session_history_mutation_id = 0;
    return m_web_content_session_history_epoch;
}

void CanonicalTraversable::abandon_pending_web_content_session_history_state_install()
{
    m_session_history_entry_url_loading_from_ui_process.clear();
    m_pending_web_content_session_history_state_install.clear();
}

void CanonicalTraversable::prepare_to_install_web_content_session_history_state()
{
    begin_new_web_content_session_history_epoch();
    m_session_history.forget_web_content_state();
    m_pending_session_history_navigation.clear();
    m_pending_web_content_session_history_state_install.clear();
    m_pending_session_history_reload_step.clear();
    if (auto step_to_restore = m_session_history.current_step_to_restore_after_loading_top_level_entry(); step_to_restore.has_value())
        m_pending_web_content_session_history_state_install.restore_command_after_loading_top_level_entry = create_apply_session_history_step_command(*step_to_restore, Web::HTML::ApplySessionHistoryStepKind::RestoreCurrentStepAfterTopLevelLoad);
    m_pending_web_content_session_history_state_install.should_install_state = true;
}

static bool can_install_state_in_replacement_process_before_load(TraversableSessionHistory const& session_history, Optional<URL::URL> const& session_history_entry_url_loading_from_ui_process, PendingWebContentSessionHistoryStateInstall const& pending_web_content_session_history_state_install)
{
    if (!pending_web_content_session_history_state_install.should_install_state)
        return false;
    if (session_history_entry_url_loading_from_ui_process.has_value())
        return false;
    (void)session_history;
    if (pending_web_content_session_history_state_install.restore_command_after_loading_top_level_entry.has_value())
        return false;
    return true;
}

static bool web_content_session_history_state_install_is_pending(PendingWebContentSessionHistoryStateInstall const& pending_web_content_session_history_state_install)
{
    return pending_web_content_session_history_state_install.should_install_state
        || pending_web_content_session_history_state_install.waiting_for_state_install_ack
        || pending_web_content_session_history_state_install.should_install_after_current_history_load
        || pending_web_content_session_history_state_install.restore_command_after_loading_top_level_entry.has_value();
}

static bool has_current_ui_session_history_entry(TraversableSessionHistory const& session_history)
{
    return session_history.current_entry() != nullptr;
}

ProcessSwapNavigationPreparation CanonicalTraversable::prepare_for_process_swap_navigation(URL::URL const& url, Web::HTML::DocumentResource document_resource, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    ProcessSwapNavigationPreparation result;
    if (!m_session_history_entry_url_loading_from_ui_process.has_value())
        begin_new_web_content_session_history_epoch();

    auto ui_session_history_already_points_to_url = false;
    if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url)
        ui_session_history_already_points_to_url = true;

    if (m_pending_session_history_traversal.has_value() && m_pending_session_history_traversal->will_replace_web_content_process)
        m_pending_session_history_traversal->stage = PendingSessionHistoryTraversal::Stage::ReplacingWebContentProcess;
    if (m_pending_session_history_navigation.has_value())
        m_pending_session_history_navigation->web_content_restore_mode = PendingSessionHistoryNavigation::WebContentRestoreMode::RestoreFromUIProcess;
    m_pending_session_history_reload_step.clear();
    m_session_history.forget_web_content_state();
    m_pending_web_content_session_history_state_install.waiting_for_state_install_ack = false;
    m_pending_web_content_session_history_state_install.expected_current_step.clear();
    m_pending_web_content_session_history_state_install.expected_state_install_id.clear();
    m_pending_web_content_session_history_state_install.should_install_state = true;

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
        m_session_history.mark_web_content_history_match_unproven();
        result.should_update_navigation_action_state = true;
    }

    if (!m_session_history_entry_url_loading_from_ui_process.has_value()) {
        if (auto step_to_restore = m_session_history.current_step_to_restore_after_loading_top_level_entry(); step_to_restore.has_value())
            m_pending_web_content_session_history_state_install.restore_command_after_loading_top_level_entry = create_apply_session_history_step_command(*step_to_restore, Web::HTML::ApplySessionHistoryStepKind::RestoreCurrentStepAfterTopLevelLoad);
        else
            m_pending_web_content_session_history_state_install.restore_command_after_loading_top_level_entry.clear();
    }

    result.should_install_web_content_history_state_before_load = can_install_state_in_replacement_process_before_load(m_session_history, m_session_history_entry_url_loading_from_ui_process, m_pending_web_content_session_history_state_install);
    return result;
}

PageLoadPreparation CanonicalTraversable::prepare_for_page_load(URL::URL const& url, Web::Bindings::NavigationHistoryBehavior history_handling)
{
    PageLoadPreparation result;

    if (m_session_history_entry_url_loading_from_ui_process.has_value())
        return result;

    abandon_pending_web_content_session_history_state_install();
    m_pending_session_history_traversal.clear();
    m_pending_session_history_reload_step.clear();
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
    m_session_history.mark_web_content_history_match_unproven();
    result.should_update_navigation_action_state = true;
    return result;
}

void CanonicalTraversable::prepare_for_non_history_page_load()
{
    begin_new_web_content_session_history_epoch();
    abandon_pending_web_content_session_history_state_install();
    m_session_history.forget_web_content_state();
    m_pending_session_history_reload_step.clear();
}

void CanonicalTraversable::prepare_for_reload()
{
    abandon_pending_web_content_session_history_state_install();
    m_pending_session_history_reload_step.clear();
    if (m_session_history.web_content_history_is_synchronized()) {
        if (auto const* current_entry = m_session_history.current_entry())
            m_pending_session_history_reload_step = current_entry->step;
    }
    m_session_history.mark_current_entry_reload_pending();
}

void CanonicalTraversable::did_replace_web_content_process()
{
    m_last_applied_web_content_session_history_mutation_id = 0;
    m_last_handled_web_content_session_history_mutation_id = 0;
}

WebContentSessionHistoryMutationResult CanonicalTraversable::did_receive_web_content_session_history_mutation(Web::HTML::WebContentSessionHistoryMutation mutation)
{
    if (mutation.epoch != m_web_content_session_history_epoch)
        return { .dump_reason = "ignored-stale-session-history-mutation-epoch"sv };

    auto mutation_operation_id = mutation.operation_id;
    if (mutation_operation_id != 0 && mutation_operation_id <= m_last_handled_web_content_session_history_mutation_id)
        return { .dump_reason = "ignored-stale-session-history-mutation"sv };

    if (mutation_operation_id != 0)
        m_last_handled_web_content_session_history_mutation_id = mutation_operation_id;

    // WebContent still creates the concrete SessionHistoryEntry for a cross-document commit. Until that is split
    // apart from the UI-owned history commit, the post-load state install is the step that grafts the UI-selected
    // entry state back onto the freshly loaded document. Do not let the interim load-commit mutation overwrite the
    // UI's authoritative entry before that install runs.
    if (m_pending_web_content_session_history_state_install.should_install_after_current_history_load)
        return { .dump_reason = "ignored-session-history-mutation-before-post-load-state-install"sv };

    if (m_pending_web_content_session_history_state_install.restore_command_after_loading_top_level_entry.has_value())
        return { .dump_reason = "ignored-session-history-mutation-before-restore-command"sv };

    if (!has_current_ui_session_history_entry(m_session_history)) {
        m_session_history.forget_web_content_state();
        return {
            .dump_reason = "rejected-session-history-mutation-without-current-ui-entry"sv,
            .should_update_navigation_action_state = true,
        };
    }

    if (mutation.mutation.has<Web::HTML::CurrentSessionHistoryEntryUpdate>()) {
        auto current_entry_update = move(mutation.mutation.get<Web::HTML::CurrentSessionHistoryEntryUpdate>());
        auto reload_pending_clear_synchronizes_state = current_entry_update.update_kind == Web::HTML::SessionHistoryEntryUpdateKind::DocumentStateReloadPending
            && m_pending_session_history_reload_step.has_value()
            && current_entry_update.entry.step == *m_pending_session_history_reload_step
            && !current_entry_update.entry.document_state.reload_pending;
        auto mutation_result = m_session_history.apply_web_content_mutation(
            TraversableSessionHistory::WebContentMutation::current_entry_update(current_entry_update.update_kind, move(current_entry_update.entry)));
        if (!mutation_result.accepted) {
            m_session_history.mark_web_content_history_match_unproven();
            if (current_entry_update.update_kind == Web::HTML::SessionHistoryEntryUpdateKind::DocumentStateReloadPending)
                m_pending_session_history_reload_step.clear();
            return {
                .dump_reason = "rejected-current-entry-update"sv,
                .should_update_navigation_action_state = true,
            };
        }

        if (reload_pending_clear_synchronizes_state) {
            m_session_history.record_web_content_history_synchronized();
            m_pending_session_history_reload_step.clear();
        }

        if (mutation_operation_id != 0)
            m_last_applied_web_content_session_history_mutation_id = mutation_operation_id;
        return {
            .accepted = true,
            .dump_reason = "did-update-current-entry"sv,
        };
    }

    if (mutation.mutation.has<Web::HTML::ChildNavigableSessionHistoryCreated>()) {
        auto child_navigable_created = move(mutation.mutation.get<Web::HTML::ChildNavigableSessionHistoryCreated>());
        auto mutation_result = m_session_history.apply_web_content_mutation(
            TraversableSessionHistory::WebContentMutation::child_navigable_created(child_navigable_created.parent_document_state_id, child_navigable_created.navigable_id, move(child_navigable_created.initial_entry), child_navigable_created.current_step));
        if (!mutation_result.accepted) {
            m_session_history.forget_web_content_state();
            return {
                .dump_reason = "rejected-child-navigable-session-history-creation"sv,
                .should_update_navigation_action_state = true,
            };
        }

        if (mutation_operation_id != 0)
            m_last_applied_web_content_session_history_mutation_id = mutation_operation_id;
        return {
            .accepted = true,
            .dump_reason = "did-create-child-navigable-session-history"sv,
            .should_update_navigation_action_state = true,
        };
    }

    if (mutation.mutation.has<Web::HTML::ChildNavigableSessionHistoryDestroyed>()) {
        auto child_navigable_destroyed = mutation.mutation.get<Web::HTML::ChildNavigableSessionHistoryDestroyed>();
        auto mutation_result = m_session_history.apply_web_content_mutation(
            TraversableSessionHistory::WebContentMutation::child_navigable_destroyed(child_navigable_destroyed.parent_document_state_id, child_navigable_destroyed.navigable_id, child_navigable_destroyed.current_step));
        if (!mutation_result.accepted) {
            m_session_history.forget_web_content_state();
            return {
                .dump_reason = "rejected-child-navigable-session-history-destruction"sv,
                .should_update_navigation_action_state = true,
            };
        }

        if (mutation_operation_id != 0)
            m_last_applied_web_content_session_history_mutation_id = mutation_operation_id;
        return {
            .accepted = true,
            .dump_reason = "did-destroy-child-navigable-session-history"sv,
            .should_update_navigation_action_state = true,
        };
    }

    if (mutation.mutation.has<Web::HTML::NestedSameDocumentSessionHistoryNavigation>()) {
        auto nested_navigation = move(mutation.mutation.get<Web::HTML::NestedSameDocumentSessionHistoryNavigation>());
        if (!m_session_history.apply_nested_same_document_navigation(move(nested_navigation))) {
            m_session_history.forget_web_content_state();
            return {
                .dump_reason = "rejected-nested-same-document-navigation"sv,
                .should_update_navigation_action_state = true,
            };
        }

        if (mutation_operation_id != 0)
            m_last_applied_web_content_session_history_mutation_id = mutation_operation_id;
        return {
            .accepted = true,
            .dump_reason = "did-apply-nested-same-document-navigation"sv,
            .should_update_navigation_action_state = true,
        };
    }

    if (mutation.mutation.has<Web::HTML::NestedCrossDocumentSessionHistoryNavigation>()) {
        auto nested_navigation = move(mutation.mutation.get<Web::HTML::NestedCrossDocumentSessionHistoryNavigation>());
        if (!m_session_history.apply_nested_cross_document_navigation_commit(move(nested_navigation))) {
            m_session_history.forget_web_content_state();
            return {
                .dump_reason = "rejected-nested-cross-document-navigation"sv,
                .should_update_navigation_action_state = true,
            };
        }

        if (mutation_operation_id != 0)
            m_last_applied_web_content_session_history_mutation_id = mutation_operation_id;
        return {
            .accepted = true,
            .dump_reason = "did-apply-nested-cross-document-navigation"sv,
            .should_update_navigation_action_state = true,
        };
    }

    if (mutation.mutation.has<Web::HTML::TopLevelCrossDocumentSessionHistoryNavigation>()) {
        auto cross_document_navigation = move(mutation.mutation.get<Web::HTML::TopLevelCrossDocumentSessionHistoryNavigation>());
        auto navigation_url = cross_document_navigation.url;
        auto pending_navigation_started_from_synchronized_state = m_pending_session_history_navigation.has_value()
            && m_pending_session_history_navigation->previous_session_history.web_content_history_is_synchronized();
        auto pending_navigation_completed_from_accepted_state_install = m_pending_session_history_navigation.has_value()
            && m_pending_session_history_navigation->web_content_restore_mode == PendingSessionHistoryNavigation::WebContentRestoreMode::RestoreFromUIProcess
            && m_pending_session_history_navigation->web_content_state_install_was_accepted
            && m_pending_session_history_navigation->url == navigation_url;
        auto initial_top_level_navigation_started_with_single_entry = !m_pending_session_history_navigation.has_value()
            && !m_session_history.web_content_history_is_synchronized()
            && m_session_history.size() == 1
            && m_session_history.used_step_count() == 1
            && m_session_history.current_used_step_index().has_value()
            && *m_session_history.current_used_step_index() == 0;
        if (!m_session_history.apply_top_level_cross_document_navigation_commit(move(cross_document_navigation))) {
            m_session_history.forget_web_content_state();
            return {
                .dump_reason = "rejected-top-level-cross-document-navigation"sv,
                .should_update_navigation_action_state = true,
            };
        }

        auto current_used_step_index = m_session_history.current_used_step_index();
        // The initial top-level navigation has no prior WebContent history to preserve. The UI predicted exactly one
        // placeholder current entry, and WebContent has now committed that same step into the single-entry list.
        auto initial_single_entry_commit_synchronizes_state = initial_top_level_navigation_started_with_single_entry
            && m_session_history.size() == 1
            && m_session_history.used_step_count() == 1
            && current_used_step_index.has_value()
            && *current_used_step_index == 0;
        if (pending_navigation_started_from_synchronized_state || pending_navigation_completed_from_accepted_state_install || initial_single_entry_commit_synchronizes_state)
            m_session_history.record_web_content_history_synchronized();
        if ((pending_navigation_started_from_synchronized_state || pending_navigation_completed_from_accepted_state_install || initial_single_entry_commit_synchronizes_state || m_session_history.web_content_history_is_synchronized()) && m_pending_session_history_navigation.has_value())
            m_pending_session_history_navigation.clear();

        if (mutation_operation_id != 0)
            m_last_applied_web_content_session_history_mutation_id = mutation_operation_id;
        return {
            .accepted = true,
            .dump_reason = "did-apply-top-level-cross-document-navigation"sv,
            .should_update_navigation_action_state = true,
            .current_url = move(navigation_url),
        };
    }

    auto same_document_navigation = move(mutation.mutation.get<Web::HTML::SameDocumentSessionHistoryNavigation>());
    if (!m_session_history.apply_top_level_same_document_navigation(move(same_document_navigation))) {
        m_session_history.forget_web_content_state();
        return {
            .dump_reason = "rejected-same-document-navigation"sv,
            .should_update_navigation_action_state = true,
        };
    }

    if (mutation_operation_id != 0)
        m_last_applied_web_content_session_history_mutation_id = mutation_operation_id;
    return {
        .accepted = true,
        .dump_reason = "did-apply-same-document-navigation"sv,
        .should_update_navigation_action_state = true,
    };
}

WebContentSessionHistoryMutationResult CanonicalTraversable::did_receive_web_content_session_history_mutation_batch(Web::HTML::WebContentSessionHistoryMutationBatch batch)
{
    if (batch.epoch != m_web_content_session_history_epoch)
        return { .dump_reason = "ignored-stale-session-history-mutation-batch-epoch"sv };

    auto batch_operation_id = batch.operation_id;
    if (batch_operation_id != 0 && batch_operation_id <= m_last_handled_web_content_session_history_mutation_id)
        return { .dump_reason = "ignored-stale-session-history-mutation"sv };

    // See the single-mutation case above. WebContent commonly reports the load commit as a mutation batch.
    if (m_pending_web_content_session_history_state_install.should_install_after_current_history_load) {
        if (batch_operation_id != 0)
            m_last_handled_web_content_session_history_mutation_id = batch_operation_id;
        return { .dump_reason = "ignored-session-history-mutation-batch-before-post-load-state-install"sv };
    }

    if (m_pending_web_content_session_history_state_install.restore_command_after_loading_top_level_entry.has_value()) {
        if (batch_operation_id != 0)
            m_last_handled_web_content_session_history_mutation_id = batch_operation_id;
        return { .dump_reason = "ignored-session-history-mutation-before-restore-command"sv };
    }

    WebContentSessionHistoryMutationResult batch_result {
        .accepted = true,
        .dump_reason = "did-apply-session-history-mutation-batch"sv,
    };

    for (auto& mutation : batch.mutations) {
        auto mutation_result = did_receive_web_content_session_history_mutation(move(mutation));
        if (!mutation_result.accepted) {
            if (batch_operation_id != 0)
                m_last_handled_web_content_session_history_mutation_id = batch_operation_id;
            return mutation_result;
        }

        batch_result.should_update_navigation_action_state |= mutation_result.should_update_navigation_action_state;
        batch_result.should_complete_webdriver_pending_navigation |= mutation_result.should_complete_webdriver_pending_navigation;
        if (mutation_result.current_url.has_value())
            batch_result.current_url = move(mutation_result.current_url);
    }

    return batch_result;
}

WebContentSessionHistoryStateInstallAckResult CanonicalTraversable::did_receive_web_content_session_history_state_install_ack(u64 state_install_id, bool accepted, i32 current_step, Web::HTML::SessionHistoryEpoch epoch)
{
    if (epoch != m_web_content_session_history_epoch)
        return { .ignored = true, .dump_reason = "ignored-stale-webcontent-session-history-state-install-ack-epoch"sv };

    if (!m_pending_web_content_session_history_state_install.waiting_for_state_install_ack)
        return { .ignored = true, .dump_reason = "ignored-webcontent-session-history-state-install-ack"sv };

    if (!m_pending_web_content_session_history_state_install.expected_state_install_id.has_value()
        || state_install_id != *m_pending_web_content_session_history_state_install.expected_state_install_id) {
        return { .ignored = true, .dump_reason = "ignored-stale-webcontent-session-history-state-install-ack"sv };
    }

    WebContentSessionHistoryStateInstallAckResult result;
    result.should_update_navigation_action_state = true;

    if (!accepted) {
        abandon_pending_web_content_session_history_state_install();
        m_session_history.forget_web_content_state();
        m_pending_session_history_traversal.clear();
        result.dump_reason = "webcontent-session-history-state-install-rejected"sv;
        return result;
    }

    auto expected_current_step = m_pending_web_content_session_history_state_install.expected_current_step;
    if (!expected_current_step.has_value() || current_step != *expected_current_step) {
        if (m_pending_web_content_session_history_state_install.should_install_after_current_history_load) {
            m_pending_web_content_session_history_state_install.waiting_for_state_install_ack = false;
            m_pending_web_content_session_history_state_install.should_install_state = true;
            m_pending_web_content_session_history_state_install.expected_current_step.clear();
            m_pending_web_content_session_history_state_install.expected_state_install_id.clear();
            m_session_history.mark_web_content_history_match_unproven();
            result.dump_reason = "webcontent-session-history-preload-state-install-ack-mismatch"sv;
            return result;
        }

        abandon_pending_web_content_session_history_state_install();
        m_session_history.forget_web_content_state();
        m_pending_session_history_traversal.clear();
        result.dump_reason = "webcontent-session-history-state-install-ack-mismatch"sv;
        return result;
    }

    m_session_history.record_web_content_session_history_state_installed(*expected_current_step);
    result.should_send_session_history_state = true;
    if (m_pending_session_history_navigation.has_value()
        && m_pending_session_history_navigation->web_content_restore_mode == PendingSessionHistoryNavigation::WebContentRestoreMode::RestoreFromUIProcess)
        m_pending_session_history_navigation->web_content_state_install_was_accepted = true;

    m_pending_web_content_session_history_state_install.waiting_for_state_install_ack = false;
    m_pending_web_content_session_history_state_install.expected_current_step.clear();
    m_pending_web_content_session_history_state_install.expected_state_install_id.clear();
    if (m_pending_web_content_session_history_state_install.should_install_after_current_history_load) {
        m_pending_web_content_session_history_state_install.should_install_state = true;
        m_session_history.mark_web_content_history_match_unproven();
        result.dump_reason = "webcontent-session-history-preload-state-install-ack"sv;
        return result;
    }

    auto pending_navigation_needs_synchronization = m_pending_session_history_navigation.has_value()
        && m_pending_session_history_navigation->web_content_restore_mode == PendingSessionHistoryNavigation::WebContentRestoreMode::PreserveCurrentProcessState;
    if (m_pending_web_content_session_history_state_install.restore_command_after_loading_top_level_entry.has_value()
        || pending_navigation_needs_synchronization) {
        m_session_history.mark_web_content_history_match_unproven();
    }
    if (m_pending_web_content_session_history_state_install.restore_command_after_loading_top_level_entry.has_value()) {
        auto command = *m_pending_web_content_session_history_state_install.restore_command_after_loading_top_level_entry;

        if (!m_pending_session_history_traversal.has_value()) {
            m_pending_session_history_traversal = PendingSessionHistoryTraversal {
                .command_id = command.command_id,
                .command_kind = command.kind,
                .target_step = command.target_step,
                .target_step_index = command.target_step_index,
                .will_change_top_level_entry = command.changes_top_level_entry,
                .will_replace_web_content_process = false,
                .webdriver_pending_navigation_completes_with_session_history_update = true,
                .stage = PendingSessionHistoryTraversal::Stage::RestoringCurrentStepAfterStateInstall,
                .on_cancelation_check_complete = nullptr,
            };
        } else {
            m_pending_session_history_traversal->command_id = command.command_id;
            m_pending_session_history_traversal->command_kind = command.kind;
            m_pending_session_history_traversal->stage = PendingSessionHistoryTraversal::Stage::RestoringCurrentStepAfterStateInstall;
        }
        result.step_to_traverse = command.target_step;
        result.command_to_apply = move(command);
    } else {
        m_pending_session_history_traversal.clear();
        result.should_complete_webdriver_pending_navigation = !m_pending_session_history_navigation.has_value();
    }

    result.dump_reason = "webcontent-session-history-state-install-ack"sv;
    return result;
}

NavigationStartResult CanonicalTraversable::did_start_navigation(URL::URL const& url, Web::HTML::DocumentResource document_resource, Web::HTML::CrossProcessId document_state_id, bool is_redirect, Web::Bindings::NavigationHistoryBehavior history_handling, bool is_showing_crash_page)
{
    if (m_session_history_entry_url_loading_from_ui_process.has_value()) {
        if (*m_session_history_entry_url_loading_from_ui_process != url)
            return { .dump_reason = "ignored-stale-ui-history-load-start"sv };

        auto should_keep_existing_web_content_history = m_pending_web_content_session_history_state_install.waiting_for_state_install_ack || m_session_history.web_content_history_is_synchronized();
        m_session_history_entry_url_loading_from_ui_process.clear();
        if (!should_keep_existing_web_content_history) {
            m_session_history.forget_web_content_state();
        }
        return { .dump_reason = "did-start-navigation-from-ui-history-load"sv };
    }

    if (m_pending_web_content_session_history_state_install.should_install_state || m_pending_web_content_session_history_state_install.waiting_for_state_install_ack) {
        if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url != url)
            return { .dump_reason = "ignored-navigation-start-before-ui-history-state install"sv };
    }

    if (is_showing_crash_page) {
        if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url) {
            prepare_to_install_web_content_session_history_state();
            return { .dump_reason = "did-start-navigation-from-crash-page"sv, .did_clear_crash_page = true };
        }
    }

    if (is_redirect) {
        m_session_history.replace_current_entry_url(url, document_state_id);
        if (m_pending_session_history_navigation.has_value())
            m_pending_session_history_navigation->url = url;
        m_session_history.mark_web_content_history_match_unproven();
        return { .dump_reason = "did-start-navigation-redirect"sv, .should_update_navigation_action_state = true, .should_update_webdriver_pending_navigation_url = true, .did_clear_crash_page = is_showing_crash_page };
    }

    if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url) {
        if (m_pending_session_history_navigation.has_value() && m_pending_session_history_navigation->url == url)
            return { .did_clear_crash_page = is_showing_crash_page };

        if (history_handling == Web::Bindings::NavigationHistoryBehavior::Push && m_session_history.web_content_history_is_synchronized())
            m_pending_session_history_navigation = PendingSessionHistoryNavigation { url, m_session_history };
        else
            m_pending_session_history_navigation.clear();

        if (history_handling == Web::Bindings::NavigationHistoryBehavior::Replace) {
            m_session_history.replace_current_entry(url, document_state_id, move(document_resource));
            m_session_history.mark_web_content_history_match_unproven();
            return { .dump_reason = "did-start-navigation-replace-current-url"sv, .should_update_navigation_action_state = true, .did_clear_crash_page = is_showing_crash_page };
        }
        if (history_handling == Web::Bindings::NavigationHistoryBehavior::Push) {
            m_session_history.navigate(url, document_state_id, move(document_resource));
            m_session_history.mark_web_content_history_match_unproven();
            return { .dump_reason = "did-start-navigation-push-current-url"sv, .should_update_navigation_action_state = true, .did_clear_crash_page = is_showing_crash_page };
        }
        return { .did_clear_crash_page = is_showing_crash_page };
    }

    if (m_session_history.current_entry())
        m_pending_session_history_navigation = PendingSessionHistoryNavigation { url, m_session_history };
    else
        m_pending_session_history_navigation.clear();
    if (history_handling == Web::Bindings::NavigationHistoryBehavior::Replace)
        m_session_history.replace_current_entry(url, document_state_id, move(document_resource));
    else
        m_session_history.navigate(url, document_state_id, move(document_resource));
    m_session_history.mark_web_content_history_match_unproven();
    return { .dump_reason = "did-start-navigation"sv, .should_update_navigation_action_state = true, .did_clear_crash_page = is_showing_crash_page };
}

NavigationCancelResult CanonicalTraversable::did_cancel_navigation(URL::URL const& url, bool has_webdriver_pending_navigation)
{
    if (m_pending_session_history_navigation.has_value() && m_pending_session_history_navigation->url == url)
        return { .status = NavigationCancelStatus::RestorePendingSessionHistoryNavigation };

    if (m_session_history_entry_url_loading_from_ui_process.has_value() && *m_session_history_entry_url_loading_from_ui_process == url) {
        m_session_history_entry_url_loading_from_ui_process.clear();
        abandon_pending_web_content_session_history_state_install();
        m_session_history.forget_web_content_state();
        m_pending_session_history_reload_step.clear();
        return { .status = NavigationCancelStatus::CanceledUIHistoryLoad };
    }

    if (has_webdriver_pending_navigation) {
        m_session_history.clear_current_entry_reload_pending();
        m_pending_session_history_reload_step.clear();
        return { .status = NavigationCancelStatus::CompleteWebdriverPendingNavigation };
    }

    return {};
}

NavigationFinishResult CanonicalTraversable::did_finish_navigation(URL::URL const& url)
{
    if (m_pending_session_history_navigation.has_value() && m_pending_session_history_navigation->url == url)
        m_pending_session_history_navigation.clear();

    if (!m_pending_web_content_session_history_state_install.should_install_state)
        return {};

    if (auto const* current_entry = m_session_history.current_entry(); current_entry && current_entry->url == url) {
        m_session_history.clear_current_entry_reload_pending();
        auto allow_current_entry_reconstruction = m_pending_web_content_session_history_state_install.should_install_after_current_history_load;
        m_pending_web_content_session_history_state_install.should_install_after_current_history_load = false;
        return { .should_install_web_content_history_state = true, .allow_current_entry_reconstruction = allow_current_entry_reconstruction };
    }

    // NB: The first finish notification from a fresh WebContent process can still report about:blank before the
    //     traversed-to entry is ready. Keep the pendingstate install state intact until we can install the full UI-owned history.
    return { .dump_reason = "skip-state-install-webcontent-session-history"sv };
}

RestorePendingSessionHistoryNavigationResult CanonicalTraversable::restore_pending_session_history_navigation()
{
    if (!m_pending_session_history_navigation.has_value())
        return {};

    auto web_content_restore_mode = m_pending_session_history_navigation->web_content_restore_mode;
    m_session_history = move(m_pending_session_history_navigation->previous_session_history);
    m_pending_session_history_navigation.clear();
    m_pending_session_history_traversal.clear();
    m_pending_session_history_reload_step.clear();

    RestorePendingSessionHistoryNavigationResult result { .restored = true, .web_content_restore_mode = web_content_restore_mode };
    if (auto* current_entry = m_session_history.current_entry()) {
        result.current_url = current_entry->url;
        if (web_content_restore_mode == PendingSessionHistoryNavigation::WebContentRestoreMode::PreserveCurrentProcessState) {
            m_session_history_entry_url_loading_from_ui_process.clear();
            abandon_pending_web_content_session_history_state_install();
        }
    } else {
        m_session_history.mark_web_content_history_match_unproven();
    }
    return result;
}

Optional<Web::HTML::ApplySessionHistoryStepCommand> CanonicalTraversable::create_apply_session_history_step_command(i32 step, Web::HTML::ApplySessionHistoryStepKind kind, Optional<u64> history_traversal_request_id, Web::HTML::SessionHistoryOperationId apply_after_mutation_id)
{
    auto target = m_session_history.traversal_target_for_step(step);
    if (!target.has_value() || !target->target_entry || !target->target_top_level_entry)
        return {};

    return Web::HTML::ApplySessionHistoryStepCommand {
        .epoch = m_web_content_session_history_epoch,
        .command_id = m_next_apply_session_history_step_command_id++,
        .apply_after_mutation_id = apply_after_mutation_id,
        .history_traversal_request_id = history_traversal_request_id,
        .kind = kind,
        .target_step = target->target_step,
        .target_step_index = target->target_step_index,
        .target_history_object_length_and_index = {
            .script_history_length = m_session_history.used_step_count(),
            .script_history_index = target->target_step_index,
        },
        .target_entry = *target->target_entry,
        .target_top_level_entry = *target->target_top_level_entry,
        .target_step_is_top_level_entry = target->target_step_is_top_level_entry,
        .changes_top_level_entry = target->changes_top_level_entry,
    };
}

Optional<Web::HTML::CommittedSessionHistoryState> CanonicalTraversable::current_session_history_state(Web::HTML::SessionHistoryOperationId last_applied_mutation_id) const
{
    auto current_used_step_index = m_session_history.current_used_step_index();
    if (!current_used_step_index.has_value())
        return {};

    auto current_step = m_session_history.step_at(*current_used_step_index);
    if (!current_step.has_value())
        return {};

    return Web::HTML::CommittedSessionHistoryState {
        .epoch = m_web_content_session_history_epoch,
        .last_applied_mutation_id = last_applied_mutation_id,
        .last_handled_mutation_id = m_last_handled_web_content_session_history_mutation_id,
        .current_step = *current_step,
        .history_object_length_and_index = {
            .script_history_length = m_session_history.used_step_count(),
            .script_history_index = *current_used_step_index,
        },
    };
}

HistoryTraversalDecision CanonicalTraversable::traverse_the_history_by_delta(int delta, CheckForCancelation check_for_cancelation, URL::URL const& current_url, Function<void(HistoryTraversalOutcome)> on_cancelation_check_complete, Optional<u64> history_traversal_request_id, Web::HTML::SessionHistoryOperationId apply_after_mutation_id)
{
    auto target = m_session_history.traversal_target_for_delta(delta);
    if (!target.has_value())
        return { .outcome = { .status = HistoryTraversalStatus::NoEntry } };

    // FIXME: This pre-flight prediction exists only because WebContent applies the history step itself, so the UI must
    //        choose between delegating the traversal to the current process and driving a cross-process load before
    //        sending anything. Once the UI process owns apply-the-history-step and issues per-navigable load commands,
    //        placement is decided per command and this prediction goes away.
    auto will_replace_web_content_process = SiteIsolationManager::the().navigation_requires_process_swap(current_url, target->target_top_level_entry->url);
    auto webdriver_pending_navigation_completes_with_session_history_update = false;
    if (auto const* current_entry = m_session_history.current_entry()) {
        webdriver_pending_navigation_completes_with_session_history_update = current_entry->document_state.id == target->target_top_level_entry->document_state.id;
    }
    auto command = create_apply_session_history_step_command(target->target_step, Web::HTML::ApplySessionHistoryStepKind::Traverse, history_traversal_request_id, apply_after_mutation_id);
    if (!command.has_value())
        return { .outcome = { .status = HistoryTraversalStatus::NoEntry } };

    auto pending_traversal = PendingSessionHistoryTraversal {
        .command_id = command->command_id,
        .apply_after_mutation_id = apply_after_mutation_id,
        .history_traversal_request_id = history_traversal_request_id,
        .command_kind = command->kind,
        .target_step = target->target_step,
        .target_step_index = target->target_step_index,
        .will_change_top_level_entry = target->changes_top_level_entry,
        .will_replace_web_content_process = will_replace_web_content_process,
        .webdriver_pending_navigation_completes_with_session_history_update = webdriver_pending_navigation_completes_with_session_history_update,
        .on_cancelation_check_complete = nullptr,
    };

    auto current_web_content_can_apply_history_step = !web_content_session_history_state_install_is_pending(m_pending_web_content_session_history_state_install);
    if (!will_replace_web_content_process && current_web_content_can_apply_history_step) {
        pending_traversal.stage = PendingSessionHistoryTraversal::Stage::ApplyingCommandInWebContent;
        m_pending_session_history_traversal = move(pending_traversal);
        return {
            .outcome = { .status = HistoryTraversalStatus::Started, .will_replace_web_content_process = will_replace_web_content_process, .will_change_top_level_entry = target->changes_top_level_entry },
            .action = HistoryTraversalAction::ApplySessionHistoryStepInWebContent,
            .command = move(command),
            .target_step = target->target_step,
            .webdriver_pending_navigation_url = target->target_top_level_entry->url,
            .webdriver_pending_navigation_completes_with_session_history_update = webdriver_pending_navigation_completes_with_session_history_update,
        };
    }

    auto needs_cancelation_check = check_for_cancelation == CheckForCancelation::Yes && current_web_content_can_apply_history_step;
    if (needs_cancelation_check) {
        command->kind = Web::HTML::ApplySessionHistoryStepKind::CheckForCancelationBeforeLoad;
        pending_traversal.command_kind = command->kind;
        pending_traversal.stage = PendingSessionHistoryTraversal::Stage::ApplyingCommandInWebContent;
        pending_traversal.on_cancelation_check_complete = move(on_cancelation_check_complete);
        m_pending_session_history_traversal = move(pending_traversal);
        return {
            .outcome = { .status = HistoryTraversalStatus::Started, .will_replace_web_content_process = will_replace_web_content_process, .will_change_top_level_entry = target->changes_top_level_entry, .waiting_for_cancelation_check = true },
            .action = HistoryTraversalAction::ApplySessionHistoryStepInWebContent,
            .command = move(command),
            .target_step = target->target_step,
        };
    }

    pending_traversal.stage = PendingSessionHistoryTraversal::Stage::LoadingEntryFromUIProcess;
    m_pending_session_history_traversal = move(pending_traversal);
    auto target_url = target->target_top_level_entry->url;
    auto previous_session_history = m_session_history;
    m_session_history.traverse_to(target->target_step_index);
    prepare_to_install_web_content_session_history_state();
    m_pending_session_history_navigation = PendingSessionHistoryNavigation { target_url, move(previous_session_history) };
    return {
        .outcome = { .status = HistoryTraversalStatus::Started, .will_replace_web_content_process = will_replace_web_content_process, .will_change_top_level_entry = target->changes_top_level_entry },
        .action = HistoryTraversalAction::LoadCurrentEntryFromUIProcess,
        .webdriver_pending_navigation_url = move(target_url),
        .webdriver_pending_navigation_completes_with_session_history_update = true,
    };
}

WebContentHistoryStepResult CanonicalTraversable::did_apply_session_history_step(Web::HTML::SessionHistoryOperationId command_id, bool step_was_available, Web::HTML::HistoryStepResult result, Web::HTML::SessionHistoryEpoch epoch)
{
    if (epoch != m_web_content_session_history_epoch)
        return { .dump_reason = "ignored-stale-apply-session-history-step-result-epoch"sv };

    if (!m_pending_session_history_traversal.has_value()
        || m_pending_session_history_traversal->command_id != command_id) {
        return { .dump_reason = "ignored-stale-apply-session-history-step-result"sv };
    }

    auto step = m_pending_session_history_traversal->target_step;
    if (m_pending_session_history_traversal->command_kind == Web::HTML::ApplySessionHistoryStepKind::CheckForCancelationBeforeLoad) {
        auto on_cancelation_check_complete = move(m_pending_session_history_traversal->on_cancelation_check_complete);

        if (result != Web::HTML::HistoryStepResult::Applied) {
            m_pending_session_history_traversal.clear();
            return {
                .dump_reason = "apply-session-history-step-command-canceled-before-load"sv,
                .on_cancelation_check_complete = move(on_cancelation_check_complete),
                .outcome = { .status = HistoryTraversalStatus::Canceled },
                .should_update_navigation_action_state = true,
                .should_complete_webdriver_pending_navigation = true,
                .should_update_webdriver_pending_navigation_to_current_url = true,
                .should_reset_webdriver_pending_navigation_completion = true,
            };
        }

        auto target = m_session_history.traversal_target_for_step(step);
        if (!target.has_value()) {
            m_session_history.forget_web_content_state();
            m_pending_session_history_traversal.clear();
            return {
                .dump_reason = "apply-session-history-step-command-precheck-without-ui-target"sv,
                .on_cancelation_check_complete = move(on_cancelation_check_complete),
                .outcome = { .status = HistoryTraversalStatus::NoEntry },
                .should_update_navigation_action_state = true,
            };
        }

        auto outcome = HistoryTraversalOutcome {
            .status = HistoryTraversalStatus::Started,
            .will_replace_web_content_process = m_pending_session_history_traversal->will_replace_web_content_process,
            .will_change_top_level_entry = m_pending_session_history_traversal->will_change_top_level_entry,
        };
        m_pending_session_history_traversal->stage = PendingSessionHistoryTraversal::Stage::LoadingEntryFromUIProcess;
        auto target_url = target->target_top_level_entry->url;
        auto previous_session_history = m_session_history;
        m_session_history.traverse_to(target->target_step_index);
        prepare_to_install_web_content_session_history_state();
        m_pending_session_history_navigation = PendingSessionHistoryNavigation { target_url, move(previous_session_history) };
        return {
            .dump_reason = "apply-session-history-step-command-load-after-cancelation-check"sv,
            .on_cancelation_check_complete = move(on_cancelation_check_complete),
            .outcome = move(outcome),
            .current_url = move(target_url),
            .should_load_current_session_history_entry_from_ui_process = true,
            .should_update_navigation_action_state = true,
        };
    }

    if (m_pending_session_history_traversal->stage == PendingSessionHistoryTraversal::Stage::RestoringCurrentStepAfterStateInstall) {
        if (step_was_available && result == Web::HTML::HistoryStepResult::Applied) {
            m_session_history.record_web_content_history_synchronized();
            m_pending_web_content_session_history_state_install.restore_command_after_loading_top_level_entry.clear();
            m_pending_session_history_traversal.clear();
            return { .dump_reason = "did-apply-restore-current-session-history-step-command"sv, .should_update_navigation_action_state = true, .should_complete_webdriver_pending_navigation = true, .should_send_session_history_state = true };
        }

        auto pending_step_dump_reason = step_was_available ? "apply-session-history-step-command-canceled"sv : "apply-session-history-step-command-target-unavailable"sv;
        if (m_pending_session_history_navigation.has_value())
            return { .dump_reason = pending_step_dump_reason, .should_restore_pending_navigation = true };

        m_pending_web_content_session_history_state_install.restore_command_after_loading_top_level_entry.clear();
        m_session_history.forget_web_content_state();
        m_pending_session_history_traversal.clear();
        return { .dump_reason = pending_step_dump_reason, .should_update_navigation_action_state = true };
    }

    if (!step_was_available) {
        if (result != Web::HTML::HistoryStepResult::Applied) {
            m_pending_session_history_traversal.clear();
            return { .dump_reason = "apply-session-history-step-command-canceled"sv, .should_update_navigation_action_state = true, .should_complete_webdriver_pending_navigation = true, .should_update_webdriver_pending_navigation_to_current_url = true, .should_reset_webdriver_pending_navigation_completion = true };
        }

        m_session_history.forget_web_content_state();
        m_pending_session_history_traversal.clear();
        return { .dump_reason = "apply-session-history-step-command-target-unavailable"sv, .should_update_navigation_action_state = true };
    }

    if (result != Web::HTML::HistoryStepResult::Applied) {
        m_pending_session_history_traversal.clear();
        return { .dump_reason = "apply-session-history-step-command-canceled"sv, .should_update_navigation_action_state = true, .should_complete_webdriver_pending_navigation = true, .should_update_webdriver_pending_navigation_to_current_url = true, .should_reset_webdriver_pending_navigation_completion = true };
    }

    auto should_complete_webdriver_pending_navigation = m_pending_session_history_traversal->webdriver_pending_navigation_completes_with_session_history_update;
    if (!m_session_history.apply_traversal_to_step(step)) {
        m_session_history.forget_web_content_state();
        m_pending_session_history_traversal.clear();
        return { .dump_reason = "apply-session-history-step-command-without-ui-target"sv, .should_update_navigation_action_state = true };
    }
    m_session_history.record_web_content_history_synchronized();

    Optional<URL::URL> current_url;
    if (auto const* current_entry = m_session_history.current_entry())
        current_url = current_entry->url;
    m_pending_session_history_traversal.clear();
    return {
        .dump_reason = "did-apply-session-history-step-command"sv,
        .current_url = move(current_url),
        .should_update_navigation_action_state = true,
        .should_complete_webdriver_pending_navigation = should_complete_webdriver_pending_navigation,
        .should_send_session_history_state = true,
    };
}

Optional<WebContentSessionHistoryStateInstall> CanonicalTraversable::prepare_web_content_session_history_state_install(bool allow_current_entry_reconstruction)
{
    auto current_top_level_entry_index = m_session_history.current_top_level_entry_index();
    if (!current_top_level_entry_index.has_value()) {
        abandon_pending_web_content_session_history_state_install();
        m_session_history.forget_web_content_state();
        return {};
    }

    auto entries = m_session_history.entries();
    if (entries.is_empty()) {
        abandon_pending_web_content_session_history_state_install();
        m_session_history.forget_web_content_state();
        return {};
    }

    auto current_entry = entries[*current_top_level_entry_index];
    Vector<Web::HTML::SessionHistoryEntryDescriptor> entries_for_navigation_api;
    entries_for_navigation_api.append(current_entry);

    auto starting_origin = current_entry.document_state.origin;
    auto i = static_cast<int>(*current_top_level_entry_index) - 1;
    while (i >= 0) {
        auto const& entry = entries[static_cast<size_t>(i)];
        auto entry_origin = entry.document_state.origin;
        if (starting_origin.has_value() && entry_origin.has_value() && !entry_origin->is_same_origin(*starting_origin))
            break;
        entries_for_navigation_api.prepend(entry);
        --i;
    }

    i = static_cast<int>(*current_top_level_entry_index) + 1;
    while (i < static_cast<int>(entries.size())) {
        auto const& entry = entries[static_cast<size_t>(i)];
        auto entry_origin = entry.document_state.origin;
        if (starting_origin.has_value() && entry_origin.has_value() && !entry_origin->is_same_origin(*starting_origin))
            break;
        entries_for_navigation_api.append(entry);
        ++i;
    }

    auto is_restoring_traversal_target = m_pending_session_history_traversal.has_value()
        && (m_pending_session_history_traversal->stage == PendingSessionHistoryTraversal::Stage::LoadingEntryFromUIProcess
            || m_pending_session_history_traversal->stage == PendingSessionHistoryTraversal::Stage::ReplacingWebContentProcess
            || m_pending_session_history_traversal->stage == PendingSessionHistoryTraversal::Stage::RestoringCurrentStepAfterStateInstall);
    auto allow_reconstructing_current_entry = is_restoring_traversal_target
        || m_pending_web_content_session_history_state_install.restore_command_after_loading_top_level_entry.has_value()
        || allow_current_entry_reconstruction;
    auto used_steps = m_session_history.used_steps();
    auto current_top_level_step = entries[*current_top_level_entry_index].step;
    auto current_used_step_index = used_steps.find_first_index(current_top_level_step);
    if (!current_used_step_index.has_value()) {
        abandon_pending_web_content_session_history_state_install();
        m_session_history.forget_web_content_state();
        return {};
    }
    auto session_history_state = current_session_history_state(m_last_applied_web_content_session_history_mutation_id);
    if (!session_history_state.has_value()) {
        abandon_pending_web_content_session_history_state_install();
        m_session_history.forget_web_content_state();
        return {};
    }

    return WebContentSessionHistoryStateInstall {
        .current_entry = move(current_entry),
        .entries_for_navigation_api = move(entries_for_navigation_api),
        .session_history_state = *session_history_state,
        .current_step = current_top_level_step,
        .allow_current_entry_reconstruction = allow_reconstructing_current_entry,
    };
}

u64 CanonicalTraversable::did_send_web_content_session_history_state_install(i32 current_step)
{
    auto state_install_id = m_next_web_content_session_history_state_install_id++;
    m_pending_web_content_session_history_state_install.waiting_for_state_install_ack = true;
    m_pending_web_content_session_history_state_install.should_install_state = false;
    m_pending_web_content_session_history_state_install.expected_current_step = current_step;
    m_pending_web_content_session_history_state_install.expected_state_install_id = state_install_id;
    return state_install_id;
}

bool CanonicalTraversable::prepare_to_restore_current_session_history_entry_from_ui_process()
{
    auto should_install_state = !m_pending_web_content_session_history_state_install.restore_command_after_loading_top_level_entry.has_value();
    if (should_install_state)
        m_pending_web_content_session_history_state_install.should_install_after_current_history_load = true;
    return should_install_state;
}

CurrentSessionHistoryEntryLoad CanonicalTraversable::prepare_current_session_history_entry_load(URL::URL const& current_url)
{
    auto const* current_entry = m_session_history.current_entry();
    if (!current_entry) {
        m_session_history_entry_url_loading_from_ui_process = current_url;
        return { .url = current_url, .document_resource = Empty {}, .history_handling = Web::Bindings::NavigationHistoryBehavior::Auto };
    }

    m_session_history_entry_url_loading_from_ui_process = current_entry->url;
    auto history_handling = m_pending_web_content_session_history_state_install.waiting_for_state_install_ack || m_session_history.web_content_history_is_synchronized()
        ? Web::Bindings::NavigationHistoryBehavior::Replace
        : Web::Bindings::NavigationHistoryBehavior::Auto;
    return { .url = current_entry->url, .document_resource = current_entry->document_state.resource, .history_handling = history_handling };
}

void CanonicalTraversable::did_crash_requiring_web_content_session_history_state_install()
{
    m_session_history_entry_url_loading_from_ui_process.clear();
    m_pending_session_history_reload_step.clear();
    prepare_to_install_web_content_session_history_state();
}

Web::HTML::SessionHistoryEpoch CanonicalTraversable::reset_session_history_for_testing()
{
    begin_new_web_content_session_history_epoch();
    m_session_history.clear();
    m_session_history.mark_web_content_history_match_unproven();
    m_pending_session_history_navigation.clear();
    m_pending_session_history_traversal.clear();
    m_pending_session_history_reload_step.clear();
    m_session_history_entry_url_loading_from_ui_process.clear();
    abandon_pending_web_content_session_history_state_install();
    return m_web_content_session_history_epoch;
}

void CanonicalTraversable::mark_web_content_session_history_stale_for_testing()
{
    m_session_history.mark_web_content_history_match_unproven();
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
    case PendingSessionHistoryTraversal::Stage::ApplyingCommandInWebContent:
        return "applying-command-in-webcontent"sv;
    case PendingSessionHistoryTraversal::Stage::LoadingEntryFromUIProcess:
        return "loading-entry-from-ui-process"sv;
    case PendingSessionHistoryTraversal::Stage::ReplacingWebContentProcess:
        return "replacing-webcontent-process"sv;
    case PendingSessionHistoryTraversal::Stage::RestoringCurrentStepAfterStateInstall:
        return "restoring-current-step-after-state-install"sv;
    }
    VERIFY_NOT_REACHED();
}

}
