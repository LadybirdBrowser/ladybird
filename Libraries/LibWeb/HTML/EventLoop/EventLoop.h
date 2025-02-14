/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/WeakPtr.h>
#include <LibCore/Forward.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/HTML/EventLoop/TaskQueue.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::HTML {

class EventLoop : public JS::Cell {
    GC_CELL(EventLoop, JS::Cell);
    GC_DECLARE_ALLOCATOR(EventLoop);

    struct PauseHandle {
        PauseHandle(EventLoop&, JS::Object const& global, HighResolutionTime::DOMHighResTimeStamp);
        ~PauseHandle();

        AK_MAKE_NONCOPYABLE(PauseHandle);
        AK_MAKE_NONMOVABLE(PauseHandle);

        GC::Ref<EventLoop> event_loop;
        GC::Ref<JS::Object const> global;
        HighResolutionTime::DOMHighResTimeStamp const time_before_pause;
    };

public:
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

    TaskQueue& task_queue() { return *m_task_queue; }
    TaskQueue const& task_queue() const { return *m_task_queue; }

    TaskQueue& microtask_queue() { return *m_microtask_queue; }
    TaskQueue const& microtask_queue() const { return *m_microtask_queue; }

    void spin_until(GC::Ref<GC::Function<bool()>> goal_condition);
    void spin_processing_tasks_with_source_until(Task::Source, GC::Ref<GC::Function<bool()>> goal_condition);
    void process();
    void queue_task_to_update_the_rendering();

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#termination-nesting-level
    size_t termination_nesting_level() const { return m_termination_nesting_level; }
    void increment_termination_nesting_level() { ++m_termination_nesting_level; }
    void decrement_termination_nesting_level() { --m_termination_nesting_level; }

    Task const* currently_running_task() const { return m_currently_running_task; }

    void schedule();

    void perform_a_microtask_checkpoint();

    void register_document(Badge<DOM::Document>, DOM::Document&);
    void unregister_document(Badge<DOM::Document>, DOM::Document&);

    [[nodiscard]] Vector<GC::Root<DOM::Document>> documents_in_this_event_loop_matching(Function<bool(DOM::Document&)> callback) const;

    Vector<GC::Root<HTML::Window>> same_loop_windows() const;

    void push_onto_backup_incumbent_realm_stack(JS::Realm&);
    void pop_backup_incumbent_realm_stack();
    JS::Realm& top_of_backup_incumbent_realm_stack();
    bool is_backup_incumbent_realm_stack_empty() const { return m_backup_incumbent_realm_stack.is_empty(); }

    void register_environment_settings_object(Badge<EnvironmentSettingsObject>, EnvironmentSettingsObject&);
    void unregister_environment_settings_object(Badge<EnvironmentSettingsObject>, EnvironmentSettingsObject&);

    double compute_deadline() const;

    [[nodiscard]] PauseHandle pause();
    void unpause(Badge<PauseHandle>, JS::Object const& global, HighResolutionTime::DOMHighResTimeStamp);
    bool execution_paused() const { return m_execution_paused; }

private:
    explicit EventLoop(Type);

    virtual void visit_edges(Visitor&) override;

    void process_input_events() const;
    void update_the_rendering();

    Type m_type { Type::Window };

    GC::Ptr<TaskQueue> m_task_queue;
    GC::Ptr<TaskQueue> m_microtask_queue;

    // https://html.spec.whatwg.org/multipage/webappapis.html#currently-running-task
    GC::Ptr<Task> m_currently_running_task { nullptr };

    // https://html.spec.whatwg.org/multipage/webappapis.html#last-render-opportunity-time
    double m_last_render_opportunity_time { 0 };
    // https://html.spec.whatwg.org/multipage/webappapis.html#last-idle-period-start-time
    double m_last_idle_period_start_time { 0 };

    GC::Ptr<Platform::Timer> m_system_event_loop_timer;

    // https://html.spec.whatwg.org/multipage/webappapis.html#performing-a-microtask-checkpoint
    bool m_performing_a_microtask_checkpoint { false };

    Vector<WeakPtr<DOM::Document>> m_documents;

    // Used to implement step 4 of "perform a microtask checkpoint".
    // NOTE: These are weak references! ESO registers and unregisters itself from the event loop manually.
    Vector<RawPtr<EnvironmentSettingsObject>> m_related_environment_settings_objects;

    // https://html.spec.whatwg.org/multipage/webappapis.html#backup-incumbent-settings-object-stack
    // https://whatpr.org/html/9893/webappapis.html#backup-incumbent-realm-stack
    Vector<GC::Ref<JS::Realm>> m_backup_incumbent_realm_stack;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#termination-nesting-level
    size_t m_termination_nesting_level { 0 };

    bool m_execution_paused { false };

    bool m_skip_event_loop_processing_steps { false };

    bool m_is_running_rendering_task { false };

    GC::Ptr<GC::Function<void()>> m_rendering_task_function;
};

EventLoop& main_thread_event_loop();
TaskID queue_a_task(HTML::Task::Source, GC::Ptr<EventLoop>, GC::Ptr<DOM::Document>, GC::Ref<GC::Function<void()>> steps);
TaskID queue_global_task(HTML::Task::Source, JS::Object&, GC::Ref<GC::Function<void()>> steps);
void queue_a_microtask(DOM::Document const*, GC::Ref<GC::Function<void()>> steps);
void perform_a_microtask_checkpoint();

}
