/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/Bindings/NavigationType.h>
#include <LibWeb/Export.h>
#include <LibWeb/Geolocation/Geolocation.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/SessionHistoryTraversalQueue.h>
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/StorageAPI/StorageShed.h>

#ifdef AK_OS_MACOS
#    include <LibGfx/MetalContext.h>
#endif

#ifdef USE_VULKAN
#    include <LibGfx/VulkanContext.h>
#endif

namespace Web::HTML {

class ApplyHistoryStepState;

// https://html.spec.whatwg.org/multipage/document-sequences.html#traversable-navigable
class WEB_API TraversableNavigable final : public Navigable {
    GC_CELL(TraversableNavigable, Navigable);
    GC_DECLARE_ALLOCATOR(TraversableNavigable);

public:
    static GC::Ref<TraversableNavigable> create_a_new_top_level_traversable(GC::Ref<Page>, GC::Ptr<BrowsingContext> opener, String target_name);
    static GC::Ref<TraversableNavigable> create_a_fresh_top_level_traversable(GC::Ref<Page>, URL::URL const& initial_navigation_url, Variant<Empty, String, POSTResource> = Empty {});

    virtual ~TraversableNavigable() override;

    virtual bool is_top_level_traversable() const override;

    int current_session_history_step() const { return m_current_session_history_step; }

    // Claims the step number for a new push-type session history entry. Claims are tracked separately from the current
    // step: The current step only advances when an apply-history-step run commits — and several runs can have claimed
    // steps in flight at once. So, computing a new step from the current step alone can hand out a step number that an
    // existing entry already holds. A claim is retired when the run that applies it completes.
    [[nodiscard]] int claim_next_session_history_step();
    void retire_claimed_session_history_step(int step);
    Vector<NonnullRefPtr<SessionHistoryEntry>>& session_history_entries() { return m_session_history_entries; }
    Vector<NonnullRefPtr<SessionHistoryEntry>> const& session_history_entries() const { return m_session_history_entries; }
    struct SessionHistorySnapshot {
        Vector<SessionHistoryEntryDescriptor> top_level_session_history_entries;
        Vector<i32> used_session_history_steps;
        size_t current_used_step_index { 0 };
    };
    enum class SaveActiveEntryPersistedState : bool {
        No,
        Yes,
    };
    SessionHistorySnapshot create_session_history_snapshot(SaveActiveEntryPersistedState = SaveActiveEntryPersistedState::Yes);

    VisibilityState system_visibility_state() const { return m_system_visibility_state; }
    void set_system_visibility_state(VisibilityState);

    bool is_created_by_web_content() const { return m_is_created_by_web_content; }
    void set_is_created_by_web_content(bool value) { m_is_created_by_web_content = value; }

    struct HistoryObjectLengthAndIndex {
        u64 script_history_length;
        u64 script_history_index;
    };
    HistoryObjectLengthAndIndex get_the_history_object_length_and_index(int) const;

    void apply_the_traverse_history_step(int, GC::Ptr<SourceSnapshotParams>, GC::Ptr<Navigable>, UserNavigationInvolvement, GC::Ref<GC::Function<void(HistoryStepResult)>> on_complete);
    void resume_applying_the_traverse_history_step(int, UserNavigationInvolvement, GC::Ref<GC::Function<void(HistoryStepResult)>> on_complete);
    void apply_the_reload_history_step(UserNavigationInvolvement, GC::Ref<GC::Function<void(HistoryStepResult)>> on_complete);
    enum class SynchronousNavigation : bool {
        Yes,
        No,
    };
    [[nodiscard]] bool try_to_synchronously_commit_same_document_navigation(GC::Ref<Navigable>, NonnullRefPtr<SessionHistoryEntry>, RefPtr<SessionHistoryEntry> entry_to_replace);
    void apply_the_push_or_replace_history_step(int step, HistoryHandlingBehavior history_handling, UserNavigationInvolvement, SynchronousNavigation, GC::Ptr<DOM::Document> pending_document, GC::Ptr<Navigable> expected_ongoing_navigation_navigable, Optional<String> expected_ongoing_navigation_id, GC::Ref<OnApplyHistoryStepComplete> on_complete);
    void update_for_navigable_creation_or_destruction(GC::Ref<OnApplyHistoryStepComplete> on_complete);

    int get_the_used_step(int step) const;
    Vector<GC::Root<Navigable>> get_all_navigables_whose_current_session_history_entry_will_change_or_reload(int) const;
    Vector<GC::Root<Navigable>> get_all_navigables_that_only_need_history_object_length_index_update(int) const;
    Vector<GC::Root<Navigable>> get_all_navigables_that_might_experience_a_cross_document_traversal(int) const;

    Vector<int> get_all_used_history_steps() const;
    void clear_the_forward_session_history();
    void traverse_the_history_by_delta(int delta, GC::Ptr<DOM::Document> source_document = {});
    void traverse_the_history_to_step(int step, GC::Ref<GC::Function<void(bool step_was_available, HistoryStepResult)>> on_complete);
    void check_if_traverse_history_step_is_canceled(int step, GC::Ref<OnApplyHistoryStepComplete> on_complete);
    bool replace_top_level_session_history_entries_from_ui_process(Vector<SessionHistoryEntryDescriptor>, size_t current_top_level_entry_index, bool allow_reconstructing_current_entry);
    void reset_session_history_for_testing(GC::Ref<GC::Function<void()>> on_complete);

    void close_top_level_traversable();
    void definitely_close_top_level_traversable();
    void destroy_top_level_traversable();

    void append_session_history_traversal_steps(GC::Ref<SessionHistoryTraversalSteps> steps)
    {
        m_session_history_traversal_queue->append(steps);
    }

    void append_session_history_synchronous_navigation_steps(GC::Ref<Navigable> target_navigable, GC::Ref<SessionHistoryTraversalSteps> steps)
    {
        m_session_history_traversal_queue->append_sync(steps, target_navigable);
    }

    String window_handle() const { return m_window_handle; }
    void set_window_handle(String window_handle) { m_window_handle = move(window_handle); }

    [[nodiscard]] GC::Ptr<DOM::Node> currently_focused_area();

    enum class CheckIfUnloadingIsCanceledResult {
        CanceledByBeforeUnload,
        CanceledByNavigate,
        Continue,
    };
    void check_if_unloading_is_canceled(Vector<GC::Root<Navigable>> navigables_that_need_before_unload, GC::Ref<GC::Function<void(CheckIfUnloadingIsCanceledResult)>> callback);

    StorageAPI::StorageShed& storage_shed() { return m_storage_shed; }
    StorageAPI::StorageShed const& storage_shed() const { return m_storage_shed; }

    // https://w3c.github.io/geolocation/#dfn-emulated-position-data
    Geolocation::EmulatedPositionData const& emulated_position_data() const;
    void set_emulated_position_data(Geolocation::EmulatedPositionData data);

    void process_screenshot_requests();
    void queue_screenshot_task(Optional<UniqueNodeID> node_id)
    {
        m_screenshot_tasks.enqueue({ node_id });
        set_needs_repaint();
        page().client().request_frame();
    }

private:
    friend class ApplyHistoryStepState;

    TraversableNavigable(GC::Ref<Page>);

    virtual bool is_traversable() const override { return true; }

    virtual void visit_edges(Cell::Visitor&) override;

    // NB: The HTML Standard spells this algorithm argument "checkForCancelation".
    void apply_the_history_step(
        int step,
        bool check_for_cancelation,
        GC::Ptr<SourceSnapshotParams>,
        GC::Ptr<Navigable> initiator_to_check,
        UserNavigationInvolvement user_involvement,
        Optional<Bindings::NavigationType> navigation_type,
        SynchronousNavigation,
        Navigable::NavigationAPIAbortBehavior,
        GC::Ptr<DOM::Document> pending_document,
        GC::Ptr<Navigable> expected_ongoing_navigation_navigable,
        Optional<String> expected_ongoing_navigation_id,
        GC::Ref<OnApplyHistoryStepComplete> on_complete);

    void apply_the_history_step_after_unload_check(
        int step,
        int target_step,
        GC::Ptr<SourceSnapshotParams> source_snapshot_params,
        UserNavigationInvolvement user_involvement,
        Optional<Bindings::NavigationType> navigation_type,
        SynchronousNavigation,
        Navigable::NavigationAPIAbortBehavior,
        GC::Ptr<DOM::Document> pending_document,
        GC::Ptr<Navigable> expected_ongoing_navigation_navigable,
        Optional<String> expected_ongoing_navigation_id,
        GC::Ref<OnApplyHistoryStepComplete> on_complete);

    using OnHistoryStepPrechecksComplete = GC::Function<void(HistoryStepResult, int target_step, Navigable::NavigationAPIAbortBehavior)>;
    void run_the_history_step_prechecks(
        int step,
        bool check_for_cancelation,
        GC::Ptr<SourceSnapshotParams>,
        GC::Ptr<Navigable> initiator_to_check,
        UserNavigationInvolvement user_involvement,
        Optional<Bindings::NavigationType> navigation_type,
        Navigable::NavigationAPIAbortBehavior,
        GC::Ref<OnHistoryStepPrechecksComplete>);

    void check_if_unloading_is_canceled(Vector<GC::Root<Navigable>> navigables_that_need_before_unload, GC::Ptr<TraversableNavigable> traversable, Optional<int> target_step, Optional<UserNavigationInvolvement> user_involvement_for_navigate_events, GC::Ref<GC::Function<void(CheckIfUnloadingIsCanceledResult)>> callback);

    Vector<NonnullRefPtr<SessionHistoryEntry>> get_session_history_entries_for_the_navigation_api(GC::Ref<Navigable>, int);

    [[nodiscard]] bool can_go_back() const;
    [[nodiscard]] bool can_go_forward() const;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-current-session-history-step
    int m_current_session_history_step { 0 };

    // Concurrent apply-history-step runs share the step numbering below. Runs are serialized through the session
    // history traversal queue — but a synchronous navigation can jump the queue while another run is paused (see the
    // "sync navigations jump queue" in the spec). So, a nested run mutates this state while an outer run is mid-flight.
    // The spec describes that concurrency — but not the necessary bookkeeping it ends up requiring in implementations.
    // See https://github.com/whatwg/html/issues/12576.
    //
    // Four invariants keep the numbering coherent under that nesting:
    //
    //  - uniqueness: a new step number is claimed past every claimed-but-uncommitted step — never just current step + 1
    //    (claim_next_session_history_step);
    //
    //  - ordering: a run commits its target step only if no newer run has committed first — so the current step can't
    //    move backwards past a newer run's commit (ApplyHistoryStepState::complete);
    //
    //  - integrity: clearing the forward session history spares entries whose steps are claimed by runs still in flight
    //    (clear_the_forward_session_history);
    //
    //  - initialization: step numbers are only compared once assigned; a pushed entry is the active session history
    //    entry before its queued synchronous step assigns its step number (the Push assertion in
    //    ApplyHistoryStepState::start).
    u64 m_apply_history_step_generation_counter { 0 };
    u64 m_committed_apply_history_step_generation { 0 };
    Vector<int> m_outstanding_claimed_session_history_steps;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-entries
    Vector<NonnullRefPtr<SessionHistoryEntry>> m_session_history_entries;

    // FIXME: https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-traversal-queue

    GC::Ptr<ApplyHistoryStepState> m_paused_apply_history_step_state;
    GC::Ptr<ApplyHistoryStepState> m_apply_history_step_state;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#system-visibility-state
    VisibilityState m_system_visibility_state { VisibilityState::Hidden };

    // https://html.spec.whatwg.org/multipage/document-sequences.html#is-created-by-web-content
    bool m_is_created_by_web_content { false };

    // https://storage.spec.whatwg.org/#traversable-navigable-storage-shed
    // A traversable navigable holds a storage shed, which is a storage shed. A traversable navigable’s storage shed holds all session storage data.
    GC::Ref<StorageAPI::StorageShed> m_storage_shed;

    GC::Ref<SessionHistoryTraversalQueue> m_session_history_traversal_queue;

    String m_window_handle;

    // https://w3c.github.io/geolocation/#dfn-emulated-position-data
    Geolocation::EmulatedPositionData m_emulated_position_data;

    struct ScreenshotTask {
        Optional<Web::UniqueNodeID> node_id;
    };
    Queue<ScreenshotTask> m_screenshot_tasks;
};

struct BrowsingContextAndDocument {
    GC::Ref<HTML::BrowsingContext> browsing_context;
    GC::Ref<DOM::Document> document;
};

BrowsingContextAndDocument create_a_new_top_level_browsing_context_and_document(GC::Ref<Page> page);
void finalize_a_same_document_navigation(GC::Ref<TraversableNavigable> traversable, GC::Ref<Navigable> target_navigable, NonnullRefPtr<SessionHistoryEntry> target_entry, RefPtr<SessionHistoryEntry> entry_to_replace, HistoryHandlingBehavior, UserNavigationInvolvement, GC::Ref<OnApplyHistoryStepComplete> on_complete);

template<>
inline bool Navigable::fast_is<TraversableNavigable>() const { return is_traversable(); }

}
