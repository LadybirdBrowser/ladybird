/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/NavigationType.h>
#include <LibWeb/HTML/SessionHistoryTraversalQueue.h>
#include <LibWeb/HTML/VisibilityState.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <WebContent/BackingStoreManager.h>

#ifdef AK_OS_MACOS
#    include <LibCore/MetalContext.h>
#endif

#ifdef USE_VULKAN
#    include <LibCore/VulkanContext.h>
#endif

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/document-sequences.html#traversable-navigable
class TraversableNavigable final : public Navigable {
    JS_CELL(TraversableNavigable, Navigable);
    JS_DECLARE_ALLOCATOR(TraversableNavigable);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<TraversableNavigable>> create_a_new_top_level_traversable(JS::NonnullGCPtr<Page>, JS::GCPtr<BrowsingContext> opener, String target_name);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<TraversableNavigable>> create_a_fresh_top_level_traversable(JS::NonnullGCPtr<Page>, URL::URL const& initial_navigation_url, Variant<Empty, String, POSTResource> = Empty {});

    virtual ~TraversableNavigable() override;

    virtual bool is_top_level_traversable() const override;

    int current_session_history_step() const { return m_current_session_history_step; }
    Vector<JS::NonnullGCPtr<SessionHistoryEntry>>& session_history_entries() { return m_session_history_entries; }
    Vector<JS::NonnullGCPtr<SessionHistoryEntry>> const& session_history_entries() const { return m_session_history_entries; }
    bool running_nested_apply_history_step() const { return m_running_nested_apply_history_step; }

    VisibilityState system_visibility_state() const { return m_system_visibility_state; }
    void set_system_visibility_state(VisibilityState);

    struct HistoryObjectLengthAndIndex {
        u64 script_history_length;
        u64 script_history_index;
    };
    HistoryObjectLengthAndIndex get_the_history_object_length_and_index(int) const;

    enum class HistoryStepResult {
        InitiatorDisallowed,
        CanceledByBeforeUnload,
        CanceledByNavigate,
        Applied,
    };

    HistoryStepResult apply_the_traverse_history_step(int, Optional<SourceSnapshotParams>, JS::GCPtr<Navigable>, UserNavigationInvolvement);
    HistoryStepResult apply_the_reload_history_step();
    enum class SynchronousNavigation : bool {
        Yes,
        No,
    };
    HistoryStepResult apply_the_push_or_replace_history_step(int step, HistoryHandlingBehavior history_handling, SynchronousNavigation);
    HistoryStepResult update_for_navigable_creation_or_destruction();

    int get_the_used_step(int step) const;
    Vector<JS::Handle<Navigable>> get_all_navigables_whose_current_session_history_entry_will_change_or_reload(int) const;
    Vector<JS::Handle<Navigable>> get_all_navigables_that_only_need_history_object_length_index_update(int) const;
    Vector<JS::Handle<Navigable>> get_all_navigables_that_might_experience_a_cross_document_traversal(int) const;

    Vector<int> get_all_used_history_steps() const;
    void clear_the_forward_session_history();
    void traverse_the_history_by_delta(int delta, Optional<DOM::Document&> source_document = {});

    void close_top_level_traversable();
    void destroy_top_level_traversable();

    void append_session_history_traversal_steps(ESCAPING Function<void()> steps)
    {
        m_session_history_traversal_queue->append(move(steps));
    }

    void append_session_history_synchronous_navigation_steps(JS::NonnullGCPtr<Navigable> target_navigable, ESCAPING Function<void()> steps)
    {
        m_session_history_traversal_queue->append_sync(move(steps), target_navigable);
    }

    String window_handle() const { return m_window_handle; }
    void set_window_handle(String window_handle) { m_window_handle = move(window_handle); }

    [[nodiscard]] JS::GCPtr<DOM::Node> currently_focused_area();

    void paint(Web::DevicePixelRect const&, Painting::BackingStore&, Web::PaintOptions);

private:
    TraversableNavigable(JS::NonnullGCPtr<Page>);

    virtual void visit_edges(Cell::Visitor&) override;

    // FIXME: Fix spec typo cancelation --> cancellation
    HistoryStepResult apply_the_history_step(
        int step,
        bool check_for_cancelation,
        Optional<SourceSnapshotParams>,
        JS::GCPtr<Navigable> initiator_to_check,
        Optional<UserNavigationInvolvement> user_involvement_for_navigate_events,
        Optional<Bindings::NavigationType> navigation_type,
        SynchronousNavigation);

    Vector<JS::NonnullGCPtr<SessionHistoryEntry>> get_session_history_entries_for_the_navigation_api(JS::NonnullGCPtr<Navigable>, int);

    [[nodiscard]] bool can_go_forward() const;

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-current-session-history-step
    int m_current_session_history_step { 0 };

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-entries
    Vector<JS::NonnullGCPtr<SessionHistoryEntry>> m_session_history_entries;

    // FIXME: https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-traversal-queue

    // https://html.spec.whatwg.org/multipage/document-sequences.html#tn-running-nested-apply-history-step
    bool m_running_nested_apply_history_step { false };

    // https://html.spec.whatwg.org/multipage/document-sequences.html#system-visibility-state
    VisibilityState m_system_visibility_state { VisibilityState::Visible };

    JS::NonnullGCPtr<SessionHistoryTraversalQueue> m_session_history_traversal_queue;

    String m_window_handle;

    OwnPtr<Web::Painting::SkiaBackendContext> m_skia_backend_context;

#ifdef AK_OS_MACOS
    OwnPtr<Core::MetalContext> m_metal_context;
#endif
};

struct BrowsingContextAndDocument {
    JS::NonnullGCPtr<HTML::BrowsingContext> browsing_context;
    JS::NonnullGCPtr<DOM::Document> document;
};

WebIDL::ExceptionOr<BrowsingContextAndDocument> create_a_new_top_level_browsing_context_and_document(JS::NonnullGCPtr<Page> page);
void finalize_a_same_document_navigation(JS::NonnullGCPtr<TraversableNavigable> traversable, JS::NonnullGCPtr<Navigable> target_navigable, JS::NonnullGCPtr<SessionHistoryEntry> target_entry, JS::GCPtr<SessionHistoryEntry> entry_to_replace, HistoryHandlingBehavior);

}
