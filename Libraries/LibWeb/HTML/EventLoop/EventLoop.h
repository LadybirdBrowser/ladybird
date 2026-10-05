/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/OwnPtr.h>
#include <AK/Queue.h>
#include <LibCore/Forward.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/EventLoop/TaskQueue.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::Layout::RustFFI {

enum class FfiFlightBlocker : uint8_t;

}

namespace Web::HTML {

class PresentationQueue;

class WEB_API EventLoop : public JS::Cell {
    GC_CELL(EventLoop, JS::Cell);
    GC_DECLARE_ALLOCATOR(EventLoop);

    struct WEB_API PauseHandle {
        PauseHandle(EventLoop&, JS::Object const& global, HighResolutionTime::DOMHighResTimeStamp);
        ~PauseHandle();

        AK_MAKE_NONCOPYABLE(PauseHandle);
        AK_MAKE_NONMOVABLE(PauseHandle);

        GC::Ref<EventLoop> event_loop;
        GC::Ref<JS::Object const> global;
        HighResolutionTime::DOMHighResTimeStamp const time_before_pause;
    };

public:
    struct RenderingSchedulerCounters {
        u64 update_requests { 0 };
        u64 coalesced_update_requests { 0 };
        u64 update_requests_while_rendering { 0 };
        u64 opportunities_received { 0 };
        u64 opportunities_that_queued_a_task { 0 };
        u64 watchdog_opportunities { 0 };
        u64 updates_run { 0 };
        u64 updates_skipped_as_unnecessary { 0 };
        u64 update_microseconds { 0 };
        u64 tasks_between_updates { 0 };
        u64 task_microseconds_between_updates { 0 };
        u64 posted_message_tasks_between_updates { 0 };
        u64 posted_message_task_microseconds_between_updates { 0 };
        u64 timer_tasks_between_updates { 0 };
        u64 timer_task_microseconds_between_updates { 0 };
        u64 networking_tasks_between_updates { 0 };
        u64 networking_task_microseconds_between_updates { 0 };
        u64 dom_manipulation_tasks_between_updates { 0 };
        u64 dom_manipulation_task_microseconds_between_updates { 0 };
        u64 paints { 0 };
    };

    enum class Type {
        // https://html.spec.whatwg.org/multipage/webappapis.html#window-event-loop
        Window,
        // https://html.spec.whatwg.org/multipage/webappapis.html#worker-event-loop
        Worker,
        // https://html.spec.whatwg.org/multipage/webappapis.html#worklet-event-loop
        Worklet,
    };

    virtual ~EventLoop() override;

    Type type() const { return m_type; }

    void run_upon_reaching_step_1(GC::Ref<GC::Function<void()>> task) { m_reached_step_1_tasks.append(task); }

    TaskQueue& task_queue() { return *m_task_queue; }
    TaskQueue const& task_queue() const { return *m_task_queue; }

    bool microtask_queue_empty() const { return m_microtask_queue.is_empty(); }
    void enqueue_microtask(GC::Ref<HTML::Task> task) { m_microtask_queue.enqueue(task); }
    GC::Ref<HTML::Task> dequeue_microtask() { return m_microtask_queue.dequeue(); }

    void spin_until(GC::Ref<GC::Function<bool()>> goal_condition);
    void process();
    void request_rendering_update();
    enum class RenderingOpportunitySource {
        Compositor,
        LocalTimer,
        Watchdog,
        Manual,
    };
    bool rendering_opportunity(HighResolutionTime::DOMHighResTimeStamp frame_time, RenderingOpportunitySource);
    bool rendering_task_queued_or_running() const { return m_rendering_task_queued || m_running_rendering_task; }
    bool running_synchronous_rendering_update() const { return m_running_synchronous_rendering_update; }

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#termination-nesting-level
    size_t termination_nesting_level() const { return m_termination_nesting_level; }
    void increment_termination_nesting_level() { ++m_termination_nesting_level; }
    void decrement_termination_nesting_level() { --m_termination_nesting_level; }

    GC::Ptr<Task const> currently_running_task() const { return m_currently_running_task; }

    u64 task_generation() const { return m_task_generation; }

    void schedule();

    void perform_a_microtask_checkpoint();

    void register_document(Badge<DOM::Document>, DOM::Document&);
    void unregister_document(Badge<DOM::Document>, DOM::Document&);
    void document_navigable_did_change(Badge<DOM::Document>);

    [[nodiscard]] Vector<GC::Root<DOM::Document>> documents_in_this_event_loop_matching(Function<bool(DOM::Document&)> callback) const;

    Vector<GC::Root<HTML::Window>> same_loop_windows() const;

    void push_onto_backup_incumbent_realm_stack(GC::Ref<EnvironmentSettingsObject>);
    void pop_backup_incumbent_realm_stack();
    EnvironmentSettingsObject& top_of_backup_incumbent_realm_stack();
    bool is_backup_incumbent_realm_stack_empty() const { return m_backup_incumbent_realm_stack.is_empty(); }

    void register_environment_settings_object(Badge<EnvironmentSettingsObject>, EnvironmentSettingsObject&);
    void unregister_environment_settings_object(Badge<EnvironmentSettingsObject>, EnvironmentSettingsObject&);

    double compute_deadline() const;

    enum class UpdateTheRendering {
        No,
        Yes,
    };

    [[nodiscard]] PauseHandle pause(UpdateTheRendering = UpdateTheRendering::Yes);
    void unpause(Badge<PauseHandle>, JS::Object const& global, HighResolutionTime::DOMHighResTimeStamp);
    bool execution_paused() const { return m_execution_pause_depth > 0; }

    bool running_rendering_task() const { return m_running_rendering_task; }

    // A rendering update's style transaction and its recording fly beside the event loop until the event loop takes them
    // in between two tasks. A rendering update whose style transaction flies runs its steps from its style and layout
    // on once the transaction is taken in.
    void did_let_recording_fly(LocalNavigable&);
    // The frames of the navigables this event loop renders, in the order the compositor presents them.
    PresentationQueue& presentation_queue() { return *m_presentation_queue; }
    // A test that injects its rendering opportunities injects its render clock's ticks as well.
    void set_render_clock_is_manual_for_testing(bool manual) { m_render_clock_is_manual_for_testing = manual; }
    bool render_clock_is_manual_for_testing() const { return m_render_clock_is_manual_for_testing; }
    // Called before a rendering update submits a recording, on a thread with a Core event loop.
    void ensure_frame_completion_registered();
    // Whether a frame flies beside the event loop, which has not taken it in yet.
    bool has_frame_in_flight() const;
    // Whether a rendering update has let its frame fly and has not run its steps from its style and layout on yet.
    bool has_rendering_update_in_flight() const { return m_rendering_update_in_flight; }
    // Whether the layout of the rendering update in flight runs, or ran, beside the event loop.
    bool lays_out_rendering_update_in_flight() const;
    // Runs the steps of the rendering update whose style transaction flies, which take the transaction in.
    void finish_rendering_update_in_flight();
    // A rendering task that would find a frame still in flight, or a rendering update not yet finished, keeps its place
    // in the queue until the frame has been taken in, rather than wait for it.
    bool holds_rendering_opportunity() const;
    // Whether the tasks of `document` wait for the rendering update in flight: its steps after its style and layout
    // deliver to the document's script what comes before any of its tasks. A task of no document waits whenever the
    // tasks of any document do, and an update with intersection observations to update holds every task.
    bool holds_tasks_of(DOM::Document const*) const;
    void hold_next_frame_for_testing() { m_holds_next_frame_for_testing = true; }
    // Has the next recording that flies leave its frame for the host to present once it lands.
    void hold_next_frame_before_present_for_testing() { m_holds_next_frame_before_present_for_testing = true; }
    bool takes_next_frame_presentation_for_testing() { return exchange(m_holds_next_frame_before_present_for_testing, false); }
    void release_held_frames_for_testing();

    RenderingSchedulerCounters const& rendering_scheduler_counters() const { return m_rendering_scheduler_counters; }
    void reset_rendering_scheduler_counters();

private:
    explicit EventLoop(Type);

    virtual void visit_edges(Visitor&) override;

    void process_input_events() const;
    void update_the_rendering();
    void update_the_rendering_after_style_and_layout(Vector<GC::Root<DOM::Document>> const& docs, double frame_timestamp, double update_start_time);
    void finish_rendering_update(double update_start_time);
    Layout::RustFFI::FfiFlightBlocker style_flight_blocker(DOM::Document&) const;
    void resume_rendering_update_in_flight();
    // Goes on with the rendering update in flight where it has landed, and presents the recordings that have landed.
    void take_finished_frames_in();
    bool let_layout_of_rendering_update_fly();
    void lease_clocks_for_task();

    Type m_type { Type::Window };

    Vector<GC::Ref<GC::Function<void()>>> m_reached_step_1_tasks;

    GC::Ptr<TaskQueue> m_task_queue;
    Queue<GC::Ref<HTML::Task>> m_microtask_queue;

    // https://html.spec.whatwg.org/multipage/webappapis.html#currently-running-task
    GC::Ptr<Task> m_currently_running_task { nullptr };

    u64 m_task_generation { 0 };

    // https://html.spec.whatwg.org/multipage/webappapis.html#last-render-opportunity-time
    double m_last_render_opportunity_time { 0 };
    // https://html.spec.whatwg.org/multipage/webappapis.html#last-idle-period-start-time
    double m_last_idle_period_start_time { 0 };

    GC::Ptr<Platform::Timer> m_system_event_loop_timer;
    GC::Ptr<Platform::Timer> m_idle_period_timer;

    // https://html.spec.whatwg.org/multipage/webappapis.html#performing-a-microtask-checkpoint
    bool m_performing_a_microtask_checkpoint { false };

    mutable Vector<GC::Weak<DOM::Document>> m_documents;
    mutable bool m_documents_sort_dirty { false };
    void ensure_documents_sorted() const;

    // Used to implement step 4 of "perform a microtask checkpoint".
    // NOTE: These are weak references! ESO registers and unregisters itself from the event loop manually.
    Vector<RawPtr<EnvironmentSettingsObject>> m_related_environment_settings_objects;

    // https://html.spec.whatwg.org/multipage/webappapis.html#backup-incumbent-settings-object-stack
    Vector<GC::Ref<EnvironmentSettingsObject>> m_backup_incumbent_realm_stack;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#termination-nesting-level
    size_t m_termination_nesting_level { 0 };

    size_t m_execution_pause_depth { 0 };

    bool m_running_rendering_task { false };
    bool m_running_synchronous_rendering_update { false };
    bool m_rendering_task_queued { false };
    bool m_rendering_update_requested { false };

    RenderingSchedulerCounters m_rendering_scheduler_counters;
    RenderingSchedulerCounters m_rendering_scheduler_counters_at_last_update;
    double m_last_rendering_update_end_time { 0 };

    GC::Ptr<GC::Function<void()>> m_rendering_task_function;

    NonnullOwnPtr<PresentationQueue> m_presentation_queue;
    // The navigables whose active document the last rendering update left a plan for a clock lease, which a task takes.
    Vector<GC::Ref<LocalNavigable>> m_navigables_with_clock_plans;
    bool m_render_clock_is_manual_for_testing { false };
    bool m_frame_completion_registered { false };
    bool m_holds_next_frame_for_testing { false };
    bool m_holds_next_frame_before_present_for_testing { false };

    struct RenderingUpdateInFlight;
    OwnPtr<RenderingUpdateInFlight> m_rendering_update_in_flight;
    // How deep the event loop is spun inside a task.
    size_t m_spin_depth { 0 };
};

WEB_API EventLoop& main_thread_event_loop();
WEB_API void run_when_event_loop_reaches_step_1(GC::Ref<GC::Function<void()>> steps);
WEB_API TaskID queue_a_task(HTML::Task::Source, GC::Ptr<EventLoop>, GC::Ptr<DOM::Document>, GC::Ref<GC::Function<void()>> steps, Task::Priority = Task::Priority::Normal, GC::Ptr<GC::Function<void()>> discard_steps = {});
WEB_API TaskID queue_global_task(HTML::Task::Source, JS::Object&, GC::Ref<GC::Function<void()>> steps, Task::Priority = Task::Priority::Normal, GC::Ptr<GC::Function<void()>> discard_steps = {});
WEB_API void queue_a_microtask(GC::Ptr<DOM::Document const>, GC::Ref<GC::Function<void()>> steps);
void perform_a_microtask_checkpoint();

}
