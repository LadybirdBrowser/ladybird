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
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/Navigation.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWeb/Page/Page.h>
#include <LibWebView/ApplyHistoryStep.h>
#include <LibWebView/CanonicalNavigable.h>
#include <LibWebView/Export.h>
#include <LibWebView/SessionHistory.h>
#include <LibWebView/SessionHistoryTraversalQueue.h>

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
    IfWebContentCannotTraverseTarget,
};

struct HistoryTraversalOutcome {
    HistoryTraversalStatus status { HistoryTraversalStatus::NoEntry };
    bool will_replace_web_content_process { false };
    bool will_change_top_level_entry { false };
};

struct PendingSessionHistoryNavigation {
    enum class WebContentRestoreMode : u8 {
        PreserveCurrentProcessState,
        RestoreFromUIProcess,
    };

    URL::URL url;
    TraversableSessionHistory previous_session_history;
    WebContentRestoreMode web_content_restore_mode { WebContentRestoreMode::PreserveCurrentProcessState };
};

struct BrowserHistoryTraversalState : RefCounted<BrowserHistoryTraversalState> {
    bool will_change_top_level_entry { false };
    bool will_replace_web_content_process { false };
    Function<void()> on_top_level_traversal_applied;
    bool did_notify_top_level_traversal_applied { false };

    void notify_top_level_traversal_applied()
    {
        auto callback = move(on_top_level_traversal_applied);
        if (!callback)
            return;
        did_notify_top_level_traversal_applied = true;
        callback();
    }
};

// NB: The results below tell ViewImplementation which UI-process side effects to apply. Each
//     carries the reason string for the session-history debug dump, so the producer is the
//     single place that decides both the state transition and how it is logged.

struct NavigationStartResult {
    Optional<StringView> dump_reason {};
    bool should_update_navigation_action_state { false };
    bool should_update_webdriver_pending_navigation_url { false };
    bool did_clear_crash_page { false };
};

enum class NavigationCancelStatus : u8 {
    Ignored,
    RestorePendingSessionHistoryNavigation,
    CompleteWebdriverPendingNavigation,
};

struct NavigationCancelResult {
    NavigationCancelStatus status { NavigationCancelStatus::Ignored };
};

struct NavigationFinishResult {
    bool should_update_webdriver_pending_navigation_url { false };
    Optional<StringView> dump_reason {};
};

struct RestorePendingSessionHistoryNavigationResult {
    Optional<URL::URL> current_url {};
    PendingSessionHistoryNavigation::WebContentRestoreMode web_content_restore_mode { PendingSessionHistoryNavigation::WebContentRestoreMode::PreserveCurrentProcessState };
};

struct WebContentHistoryStepResult {
    StringView dump_reason;
    bool should_restore_pending_navigation { false };
    bool should_update_navigation_action_state { false };
    Optional<URL::URL> current_url {};
    bool should_complete_webdriver_pending_navigation { false };
    bool should_update_webdriver_pending_navigation_to_current_url { false };
    bool should_reset_webdriver_pending_navigation_completion { false };
};

struct HistoryStepCancelationCheckResult {
    StringView dump_reason;
    Function<void(HistoryTraversalOutcome)> on_cancelation_check_complete {};
    HistoryTraversalOutcome outcome {};
    // When set, WebContent must reconstruct the UI-owned target after reporting the outcome.
    Optional<TraversableSessionHistory::TraversalTarget> target {};
    bool should_restore_pending_navigation { false };
    bool should_update_navigation_action_state { false };
    bool should_complete_webdriver_pending_navigation { false };
    bool should_update_webdriver_pending_navigation_to_current_url { false };
    bool should_reset_webdriver_pending_navigation_completion { false };
};

struct ProcessSwapNavigationPreparation {
    bool should_update_navigation_action_state { false };
    Optional<Web::HistoryLoad> history_load;
};

struct PageLoadPreparation {
    bool should_defer_ui_process_history_update { false };
    bool should_update_navigation_action_state { false };
};

class WEBVIEW_API CanonicalTraversable final
    : public CanonicalNavigable {
public:
    CanonicalTraversable();
    virtual ~CanonicalTraversable() override;

    virtual bool is_top_level_traversable() const override { return true; }

    // Apply-the-history-step coordination. Operations serialize on the traversable's session history traversal
    // queue; the algorithm runs here and dispatches its per-navigable jobs to the processes hosting the documents.
    using OnHistoryOperationComplete = Function<void(Web::HTML::HistoryStepResult, Optional<i32> committed_step)>;

    struct BrowserHistoryTraversalOperation {
        i32 target_step { 0 };
        bool check_for_cancelation { true };
        bool reconstructs_web_content_history { false };
        bool requires_process_replacement { false };
        bool restores_replacement_process { false };
        RefPtr<BrowserHistoryTraversalState> state;
    };

    struct HistoryStepCancelationCheckOperation {
        i32 target_step { 0 };
        Optional<Web::HTML::CrossProcessId> initiator_to_check {};
        Web::HTML::UserNavigationInvolvement user_involvement { Web::HTML::UserNavigationInvolvement::BrowserUI };
        bool will_change_top_level_entry { false };
        bool will_replace_web_content_process { false };
    };

    struct BrowserHistoryTraversalDiagnostic {
        enum class Stage : u8 {
            ApplyingInWebContent,
            CheckingCancelation,
        };

        i32 target_step { 0 };
        size_t target_step_index { 0 };
        bool will_change_top_level_entry { false };
        bool will_replace_web_content_process { false };
        Stage stage { Stage::ApplyingInWebContent };
    };

    void enqueue_history_operation(u64 initiation_id, Web::HistoryOperationParameters, WebContentClient& requesting_client, u64 requesting_page_id, OnHistoryOperationComplete = nullptr);
    void enqueue_history_operation(BrowserHistoryTraversalOperation, OnHistoryOperationComplete = nullptr);
    // Appends plain algorithm steps; a requested traversal defers its target resolution to its queued position, the
    // way the specification's queued steps do, and then starts its operation at that position.
    void append_history_queue_steps(SessionHistoryTraversalSteps);
    void run_history_operation_at_queue_position(u64 initiation_id, Web::HistoryOperationParameters, WebContentClient& requesting_client, u64 requesting_page_id, Optional<i32> resolved_step, OnHistoryOperationComplete, NonnullRefPtr<Core::Promise<Empty>>);
    void run_history_operation_at_queue_position(u64 initiation_id, HistoryStepCancelationCheckOperation, WebContentClient& requesting_client, u64 requesting_page_id, OnHistoryOperationComplete, NonnullRefPtr<Core::Promise<Empty>>);
    void abandon_history_operations();

    struct HistoryJobEndpoint {
        WebContentClient* client { nullptr };
        u64 page_id { 0 };
    };
    HistoryJobEndpoint history_job_endpoint_for(CanonicalNavigable const&) const;

    void did_receive_history_operation_ready(u64 operation_id, bool proceed, Optional<Web::HTML::CrossProcessId> creation_parent_document_state_id, Optional<Web::HTML::SameDocumentNavigationEntry>, Optional<Web::CrossDocumentNavigationFinalization>, Web::HTML::HistoryStepResult abandon_result);
    void did_receive_initiator_sandboxing_check_result(u64 operation_id, bool allowed);
    void did_receive_history_step_unload_cancelation_result(u64 operation_id, Web::HTML::HistoryStepResult);
    void did_receive_changing_navigable_history_job_ready(u64 operation_id, Web::HTML::CrossProcessId navigable_id, Web::HTML::ChangingNavigableHistoryStepJobDisposition);
    void did_receive_changing_navigable_continuation_applied(u64 operation_id, Web::HTML::CrossProcessId navigable_id, Optional<Web::HTML::SessionHistoryEntryPersistedState> previous_entry_persisted_state);
    void did_receive_nonchanging_navigable_history_state_updated(u64 operation_id, Web::HTML::CrossProcessId navigable_id);

    CanonicalNavigable& insert(WebContentClient& reporting_client, u64 page_id, Web::HTML::CrossProcessId parent_frame_id, Web::HTML::CrossProcessId frame_id, CanonicalNavigable& fallback_parent);
    Optional<CanonicalNavigable&> find(Web::HTML::CrossProcessId navigable_id);
    Optional<CanonicalNavigable const&> find(Web::HTML::CrossProcessId navigable_id) const;
    void remove(CanonicalNavigable&);

    TraversableSessionHistory const& session_history() const { return m_session_history; }

    Web::HTML::VisibilityState system_visibility_state() const { return m_system_visibility_state; }
    void set_system_visibility_state(Web::HTML::VisibilityState visibility_state) { m_system_visibility_state = visibility_state; }

    Optional<PendingSessionHistoryNavigation> const& pending_session_history_navigation() const { return m_pending_session_history_navigation; }
    Optional<BrowserHistoryTraversalDiagnostic> browser_history_traversal_for_testing() const;

    ProcessSwapNavigationPreparation prepare_for_process_swap_navigation(URL::URL const&, Web::HTML::DocumentResource, Web::Bindings::NavigationHistoryBehavior);
    PageLoadPreparation prepare_for_page_load(URL::URL const&, Web::Bindings::NavigationHistoryBehavior);
    void prepare_for_non_history_page_load();
    void prepare_for_reload();
    void did_create_top_level_traversable(Web::HTML::SessionHistoryEntryDescriptor initial_history_entry);
    bool update_session_history_entry_navigation_api_state(CanonicalNavigable const&, Utf16String const& navigation_api_key, Web::HTML::StorageSerializationRecord navigation_api_state);
    bool update_session_history_entry_scroll_restoration_mode(CanonicalNavigable const&, Utf16String const& navigation_api_key, Web::HTML::ScrollRestorationMode scroll_restoration_mode);
    bool update_session_history_entry_document_state_navigable_target_name(CanonicalNavigable const&, Utf16String const& navigation_api_key, Utf16String navigable_target_name);
    bool set_session_history_entry_document_state_reload_pending(CanonicalNavigable const&, Utf16String const& navigation_api_key, bool reload_pending);
    Optional<i32> append_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id, Web::HTML::PendingSessionHistoryEntryDescriptor initial_history_entry);
    bool remove_nested_history(CanonicalNavigable const& parent_navigable, Web::HTML::CrossProcessId parent_document_state_id, Web::HTML::CrossProcessId child_navigable_id);
    Optional<i32> navigation_api_traversal_target(CanonicalNavigable const&, Utf16String const& navigation_api_key) const;
    NavigationStartResult did_start_navigation(URL::URL const&, Optional<Web::HTML::PendingSessionHistoryEntryDescriptor>, bool is_history_load, bool is_redirect, Web::Bindings::NavigationHistoryBehavior, bool is_showing_crash_page);
    NavigationCancelResult did_cancel_navigation(URL::URL const&, bool has_webdriver_pending_navigation);
    NavigationFinishResult did_finish_navigation(URL::URL const&);
    RestorePendingSessionHistoryNavigationResult restore_pending_session_history_navigation();
    void traverse_the_history_by_delta(int delta, CheckForCancelation, Function<void(HistoryTraversalOutcome)> on_complete = nullptr, Function<void()> on_top_level_traversal_applied = nullptr);
    void traverse_the_history_to_step(i32 step, CheckForCancelation, Function<void(HistoryTraversalOutcome)> on_complete = nullptr, Function<void()> on_top_level_traversal_applied = nullptr);
    void reconstruct_the_history_to_step(i32 step, bool requires_process_replacement, Function<void()> on_top_level_traversal_applied);
    WebContentHistoryStepResult did_traverse_the_history_to_step(Web::HTML::HistoryStepResult, RefPtr<BrowserHistoryTraversalState> const&);
    HistoryStepCancelationCheckResult did_check_if_traverse_history_step_is_canceled(i32 step, Web::HTML::HistoryStepResult, Function<void(HistoryTraversalOutcome)>, HistoryTraversalOutcome);
    Optional<Web::HistoryLoad> prepare_history_load();
    void abandon_after_web_content_process_crash();
    void recover_from_web_content_process_crash(OnHistoryOperationComplete);
    void reset_session_history_for_testing();

    static StringView pending_session_history_navigation_web_content_restore_mode_to_string(PendingSessionHistoryNavigation::WebContentRestoreMode);
    static StringView browser_history_traversal_stage_to_string(BrowserHistoryTraversalDiagnostic::Stage);

private:
    struct HistoryOperation;
    HistoryOperation* find_history_operation(u64 operation_id);
    void add_history_operation_completion_endpoint(HistoryOperation&, HistoryJobEndpoint, Optional<u64> initiation_id = {});
    bool is_history_traversal_operation(HistoryOperation const&) const;
    ApplyHistoryStepJobs create_apply_history_step_jobs(u64 operation_id);
    void run_ui_history_operation_at_queue_position(Variant<BrowserHistoryTraversalOperation, HistoryStepCancelationCheckOperation>, OnHistoryOperationComplete, NonnullRefPtr<Core::Promise<Empty>>);
    void start_history_operation(HistoryOperation&, NonnullRefPtr<Core::Promise<Empty>>);
    void apply_history_step(HistoryOperation&, i32 step, bool check_for_cancelation, Optional<Web::HTML::CrossProcessId> initiator_to_check, Web::HTML::UserNavigationInvolvement, Optional<Web::Bindings::NavigationType>, Web::HTML::SynchronousNavigation, Optional<Web::HTML::CrossProcessId> navigable_with_finalized_entry);
    void update_for_navigable_creation_or_destruction(HistoryOperation&);
    void check_history_step_cancelation(HistoryOperation&, HistoryStepCancelationCheckOperation const&);
    void finish_history_operation(u64 operation_id, Web::HTML::HistoryStepResult, Optional<i32> committed_step);

    bool web_content_can_apply_traversal(TraversableSessionHistory::TraversalTarget const&) const;
    Optional<Web::HTML::CrossProcessId> nested_history_id_for(CanonicalNavigable const&) const;
    void traverse_the_history(TraversableSessionHistory::TraversalTarget const&, CheckForCancelation, Function<void(HistoryTraversalOutcome)> on_complete, Function<void()> on_top_level_traversal_applied, NonnullRefPtr<Core::Promise<Empty>>, bool requires_process_replacement = false);
    void remove_from_index(CanonicalNavigable&);

    HashMap<Web::HTML::CrossProcessId, WeakPtr<CanonicalNavigable>> m_navigable_index;
    TraversableSessionHistory m_session_history;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-traversal-queue
    SessionHistoryTraversalQueue m_history_traversal_queue;
    TraversableApplyHistoryStepState m_apply_history_step_traversable_state;
    u64 m_next_history_operation_id { 1 };
    u64 m_next_history_load_id { 1 };
    HashMap<u64, NonnullOwnPtr<HistoryOperation>> m_history_operations;
    Web::HTML::VisibilityState m_system_visibility_state { Web::HTML::VisibilityState::Hidden };
    Optional<PendingSessionHistoryNavigation> m_pending_session_history_navigation;
};

}
