/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashFunctions.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/Navigation.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/Export.h>
#include <LibWebView/SessionHistory.h>

namespace WebView {

enum class HistoryTraversalStatus : u8 {
    Started,
    NoEntry,
    Canceled,
};

// NB: The HTML Standard spells this algorithm argument "checkForCancelation".
enum class CheckForCancelation : u8 {
    Yes,
    No,
};

struct HistoryTraversalOutcome {
    HistoryTraversalStatus status { HistoryTraversalStatus::NoEntry };
    bool will_replace_web_content_process { false };
    bool will_change_top_level_entry { false };
    bool waiting_for_cancelation_check { false };
};

struct PendingSessionHistoryNavigation {
    enum class WebContentRestoreMode : u8 {
        PreserveCurrentProcessState,
        RestoreFromUIProcess,
    };

    URL::URL url;
    TraversableSessionHistory previous_session_history;
    WebContentRestoreMode web_content_restore_mode { WebContentRestoreMode::PreserveCurrentProcessState };
    bool web_content_state_install_was_accepted { false };
};

struct PendingWebContentSessionHistoryStateInstall {
    bool should_install_state { false };
    bool waiting_for_state_install_ack { false };
    bool should_install_after_current_history_load { false };
    Optional<Web::HTML::ApplySessionHistoryStepCommand> restore_command_after_loading_top_level_entry;
    Optional<i32> expected_current_step;
    Optional<u64> expected_state_install_id;

    void clear() { *this = {}; }
};

struct PendingSessionHistoryTraversal {
    enum class Stage : u8 {
        ApplyingCommandInWebContent,
        LoadingEntryFromUIProcess,
        ReplacingWebContentProcess,
        RestoringCurrentStepAfterStateInstall,
    };

    Web::HTML::SessionHistoryOperationId command_id { 0 };
    Web::HTML::SessionHistoryOperationId apply_after_mutation_id { 0 };
    Optional<u64> history_traversal_request_id;
    Web::HTML::ApplySessionHistoryStepKind command_kind { Web::HTML::ApplySessionHistoryStepKind::Traverse };
    i32 target_step { 0 };
    size_t target_step_index { 0 };
    bool will_change_top_level_entry { false };
    bool will_replace_web_content_process { false };
    bool webdriver_pending_navigation_completes_with_session_history_update { false };
    Stage stage { Stage::ApplyingCommandInWebContent };
    Function<void(HistoryTraversalOutcome)> on_cancelation_check_complete;
};

// NB: The results below tell ViewImplementation which UI-process side effects to apply. Each
//     carries the reason string for the session-history debug dump, so the producer is the
//     single place that decides both the state transition and how it is logged.

struct WebContentSessionHistoryMutationResult {
    bool accepted { false };
    StringView dump_reason;
    bool should_update_navigation_action_state { false };
    Optional<URL::URL> current_url {};
    bool should_complete_webdriver_pending_navigation { false };
};

struct WebContentSessionHistoryStateInstallAckResult {
    bool ignored { false };
    StringView dump_reason;
    Optional<i32> step_to_traverse {};
    Optional<Web::HTML::ApplySessionHistoryStepCommand> command_to_apply {};
    bool should_complete_webdriver_pending_navigation { false };
    bool should_update_navigation_action_state { false };
    bool should_send_session_history_state { false };
};

struct NavigationStartResult {
    Optional<StringView> dump_reason {};
    bool should_update_navigation_action_state { false };
    bool should_update_webdriver_pending_navigation_url { false };
    bool did_clear_crash_page { false };
};

enum class NavigationCancelStatus : u8 {
    Ignored,
    RestorePendingSessionHistoryNavigation,
    CanceledUIHistoryLoad,
    CompleteWebdriverPendingNavigation,
};

struct NavigationCancelResult {
    NavigationCancelStatus status { NavigationCancelStatus::Ignored };
};

struct NavigationFinishResult {
    bool should_install_web_content_history_state { false };
    bool allow_current_entry_reconstruction { false };
    Optional<StringView> dump_reason {};
};

struct RestorePendingSessionHistoryNavigationResult {
    bool restored { false };
    Optional<URL::URL> current_url {};
    PendingSessionHistoryNavigation::WebContentRestoreMode web_content_restore_mode { PendingSessionHistoryNavigation::WebContentRestoreMode::PreserveCurrentProcessState };
};

enum class HistoryTraversalAction : u8 {
    None,
    ApplySessionHistoryStepInWebContent,
    LoadCurrentEntryFromUIProcess,
};

struct HistoryTraversalDecision {
    HistoryTraversalOutcome outcome;
    HistoryTraversalAction action { HistoryTraversalAction::None };
    Optional<Web::HTML::ApplySessionHistoryStepCommand> command {};
    Optional<i32> target_step {};
    Optional<URL::URL> webdriver_pending_navigation_url {};
    bool webdriver_pending_navigation_completes_with_session_history_update { false };
};

struct WebContentHistoryStepResult {
    StringView dump_reason;
    Function<void(HistoryTraversalOutcome)> on_cancelation_check_complete {};
    HistoryTraversalOutcome outcome {};
    Optional<URL::URL> current_url {};
    bool should_restore_pending_navigation { false };
    bool should_load_current_session_history_entry_from_ui_process { false };
    bool should_update_navigation_action_state { false };
    bool should_complete_webdriver_pending_navigation { false };
    bool should_update_webdriver_pending_navigation_to_current_url { false };
    bool should_reset_webdriver_pending_navigation_completion { false };
    bool should_send_session_history_state { false };
};

struct WebContentSessionHistoryStateInstall {
    Web::HTML::SessionHistoryEntryDescriptor current_entry;
    Vector<Web::HTML::SessionHistoryEntryDescriptor> entries_for_navigation_api;
    Web::HTML::CommittedSessionHistoryState session_history_state;
    i32 current_step { 0 };
    bool allow_current_entry_reconstruction { false };
};

struct CurrentSessionHistoryEntryLoad {
    URL::URL url;
    Web::HTML::DocumentResource document_resource;
    Web::Bindings::NavigationHistoryBehavior history_handling { Web::Bindings::NavigationHistoryBehavior::Auto };
};

struct ProcessSwapNavigationPreparation {
    bool should_update_navigation_action_state { false };
    bool should_install_web_content_history_state_before_load { false };
};

struct PageLoadPreparation {
    bool should_defer_ui_process_history_update { false };
    bool should_update_navigation_action_state { false };
};

class WEBVIEW_API CanonicalTraversable final
    : public CanonicalNavigable {
public:
    CanonicalTraversable();

    virtual bool is_top_level_traversable() const override { return true; }

    CanonicalNavigable& insert(WebContentClient& reporting_client, u64 page_id, Web::HTML::CrossProcessId parent_frame_id, Web::HTML::CrossProcessId frame_id, CanonicalNavigable& fallback_parent);
    Optional<CanonicalNavigable&> find(Web::HTML::CrossProcessId frame_id);
    Optional<CanonicalNavigable const&> find(Web::HTML::CrossProcessId frame_id) const;
    void remove(CanonicalNavigable&);

    TraversableSessionHistory const& session_history() const { return m_session_history; }

    Web::HTML::VisibilityState system_visibility_state() const { return m_system_visibility_state; }
    void set_system_visibility_state(Web::HTML::VisibilityState visibility_state) { m_system_visibility_state = visibility_state; }

    bool current_web_content_session_history_is_synchronized() const { return !m_pending_session_history_navigation.has_value() && m_session_history.web_content_history_is_synchronized(); }

    Optional<PendingSessionHistoryNavigation> const& pending_session_history_navigation() const { return m_pending_session_history_navigation; }
    Optional<PendingSessionHistoryTraversal> const& pending_session_history_traversal() const { return m_pending_session_history_traversal; }

    Optional<URL::URL> const& session_history_entry_url_loading_from_ui_process() const { return m_session_history_entry_url_loading_from_ui_process; }
    PendingWebContentSessionHistoryStateInstall const& pending_web_content_session_history_state_install() const { return m_pending_web_content_session_history_state_install; }
    Web::HTML::SessionHistoryEpoch web_content_session_history_epoch() const { return m_web_content_session_history_epoch; }
    bool is_current_web_content_session_history_epoch(Web::HTML::SessionHistoryEpoch epoch) const { return epoch == m_web_content_session_history_epoch; }

    ProcessSwapNavigationPreparation prepare_for_process_swap_navigation(URL::URL const&, Web::HTML::DocumentResource, Web::Bindings::NavigationHistoryBehavior);
    PageLoadPreparation prepare_for_page_load(URL::URL const&, Web::Bindings::NavigationHistoryBehavior);
    void prepare_for_non_history_page_load();
    void prepare_for_reload();
    void prepare_to_install_web_content_session_history_state();
    WebContentSessionHistoryMutationResult did_receive_web_content_session_history_mutation(Web::HTML::WebContentSessionHistoryMutation);
    WebContentSessionHistoryMutationResult did_receive_web_content_session_history_mutation_batch(Web::HTML::WebContentSessionHistoryMutationBatch);
    WebContentSessionHistoryStateInstallAckResult did_receive_web_content_session_history_state_install_ack(u64 state_install_id, bool accepted, i32 current_step) { return did_receive_web_content_session_history_state_install_ack(state_install_id, accepted, current_step, m_web_content_session_history_epoch); }
    WebContentSessionHistoryStateInstallAckResult did_receive_web_content_session_history_state_install_ack(u64 state_install_id, bool accepted, i32 current_step, Web::HTML::SessionHistoryEpoch);
    NavigationStartResult did_start_navigation(URL::URL const&, Web::HTML::DocumentResource, Web::HTML::CrossProcessId document_state_id, bool is_redirect, Web::Bindings::NavigationHistoryBehavior, bool is_showing_crash_page);
    NavigationCancelResult did_cancel_navigation(URL::URL const&, bool has_webdriver_pending_navigation);
    NavigationFinishResult did_finish_navigation(URL::URL const&);
    RestorePendingSessionHistoryNavigationResult restore_pending_session_history_navigation();
    HistoryTraversalDecision traverse_the_history_by_delta(int delta, CheckForCancelation, URL::URL const& current_url, Function<void(HistoryTraversalOutcome)> on_cancelation_check_complete, Optional<u64> history_traversal_request_id = {}, Web::HTML::SessionHistoryOperationId apply_after_mutation_id = 0);
    WebContentHistoryStepResult did_apply_session_history_step(Web::HTML::SessionHistoryOperationId command_id, bool step_was_available, Web::HTML::HistoryStepResult result) { return did_apply_session_history_step(command_id, step_was_available, result, m_web_content_session_history_epoch); }
    WebContentHistoryStepResult did_apply_session_history_step(Web::HTML::SessionHistoryOperationId command_id, bool step_was_available, Web::HTML::HistoryStepResult, Web::HTML::SessionHistoryEpoch);
    Optional<Web::HTML::ApplySessionHistoryStepCommand> create_apply_session_history_step_command(i32 step, Web::HTML::ApplySessionHistoryStepKind = Web::HTML::ApplySessionHistoryStepKind::Traverse, Optional<u64> history_traversal_request_id = {}, Web::HTML::SessionHistoryOperationId apply_after_mutation_id = 0);
    Optional<Web::HTML::CommittedSessionHistoryState> current_session_history_state(Web::HTML::SessionHistoryOperationId last_applied_mutation_id) const;
    Web::HTML::SessionHistoryOperationId last_applied_web_content_session_history_mutation_id() const { return m_last_applied_web_content_session_history_mutation_id; }
    Optional<WebContentSessionHistoryStateInstall> prepare_web_content_session_history_state_install(bool allow_current_entry_reconstruction);
    CurrentSessionHistoryEntryLoad prepare_current_session_history_entry_load(URL::URL const& current_url);
    u64 did_send_web_content_session_history_state_install(i32 current_step);
    bool prepare_to_restore_current_session_history_entry_from_ui_process();
    void did_crash_requiring_web_content_session_history_state_install();
    Web::HTML::SessionHistoryEpoch reset_session_history_for_testing();
    void mark_web_content_session_history_stale_for_testing();
    void did_replace_web_content_process();

    static StringView pending_session_history_navigation_web_content_restore_mode_to_string(PendingSessionHistoryNavigation::WebContentRestoreMode);
    static StringView pending_session_history_traversal_stage_to_string(PendingSessionHistoryTraversal::Stage);

private:
    Web::HTML::SessionHistoryEpoch begin_new_web_content_session_history_epoch();
    void abandon_pending_web_content_session_history_state_install();
    void remove_from_index(CanonicalNavigable&);

    HashMap<Web::HTML::CrossProcessId, WeakPtr<CanonicalNavigable>> m_navigable_index;
    TraversableSessionHistory m_session_history;
    Web::HTML::VisibilityState m_system_visibility_state { Web::HTML::VisibilityState::Hidden };
    Optional<PendingSessionHistoryNavigation> m_pending_session_history_navigation;
    Optional<PendingSessionHistoryTraversal> m_pending_session_history_traversal;
    Optional<i32> m_pending_session_history_reload_step;
    u64 m_next_web_content_session_history_state_install_id { 1 };
    Web::HTML::SessionHistoryEpoch m_web_content_session_history_epoch { 0 };
    Web::HTML::SessionHistoryOperationId m_next_apply_session_history_step_command_id { 1 };
    Web::HTML::SessionHistoryOperationId m_last_applied_web_content_session_history_mutation_id { 0 };
    Web::HTML::SessionHistoryOperationId m_last_handled_web_content_session_history_mutation_id { 0 };
    Optional<URL::URL> m_session_history_entry_url_loading_from_ui_process;
    PendingWebContentSessionHistoryStateInstall m_pending_web_content_session_history_state_install;
};

}
