/*
 * Copyright (c) 2021, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/WeakPtr.h>
#include <LibCore/Forward.h>
#include <LibJS/Forward.h>
#include <LibJS/SafeFunction.h>
#include <LibWeb/HTML/EventLoop/TaskQueue.h>

namespace Web::HTML {

class EventLoop : public JS::Cell {
    JS_CELL(EventLoop, JS::Cell);
    JS_DECLARE_ALLOCATOR(EventLoop);

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

    void spin_until(JS::SafeFunction<bool()> goal_condition);
    void spin_processing_tasks_with_source_until(Task::Source, JS::SafeFunction<bool()> goal_condition);
    void process();

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#termination-nesting-level
    size_t termination_nesting_level() const { return m_termination_nesting_level; }
    void increment_termination_nesting_level() { ++m_termination_nesting_level; }
    void decrement_termination_nesting_level() { --m_termination_nesting_level; }

    Task const* currently_running_task() const { return m_currently_running_task; }

    void schedule();

    void perform_a_microtask_checkpoint();

    void register_document(Badge<DOM::Document>, DOM::Document&);
    void unregister_document(Badge<DOM::Document>, DOM::Document&);

    Vector<JS::Handle<DOM::Document>> documents_in_this_event_loop() const;

    Vector<JS::Handle<HTML::Window>> same_loop_windows() const;

    void push_onto_backup_incumbent_settings_object_stack(Badge<EnvironmentSettingsObject>, EnvironmentSettingsObject& environment_settings_object);
    void pop_backup_incumbent_settings_object_stack(Badge<EnvironmentSettingsObject>);
    EnvironmentSettingsObject& top_of_backup_incumbent_settings_object_stack();
    bool is_backup_incumbent_settings_object_stack_empty() const { return m_backup_incumbent_settings_object_stack.is_empty(); }

    void register_environment_settings_object(Badge<EnvironmentSettingsObject>, EnvironmentSettingsObject&);
    void unregister_environment_settings_object(Badge<EnvironmentSettingsObject>, EnvironmentSettingsObject&);

    double compute_deadline() const;

    // https://html.spec.whatwg.org/multipage/webappapis.html#pause
    void set_execution_paused(bool execution_paused) { m_execution_paused = execution_paused; }
    bool execution_paused() const { return m_execution_paused; }

private:
    explicit EventLoop(Type);

    virtual void visit_edges(Visitor&) override;

    Type m_type { Type::Window };

    JS::GCPtr<TaskQueue> m_task_queue;
    JS::GCPtr<TaskQueue> m_microtask_queue;

    // https://html.spec.whatwg.org/multipage/webappapis.html#currently-running-task
    JS::GCPtr<Task> m_currently_running_task { nullptr };

    // https://html.spec.whatwg.org/multipage/webappapis.html#last-render-opportunity-time
    double m_last_render_opportunity_time { 0 };
    // https://html.spec.whatwg.org/multipage/webappapis.html#last-idle-period-start-time
    double m_last_idle_period_start_time { 0 };

    RefPtr<Platform::Timer> m_system_event_loop_timer;

    // https://html.spec.whatwg.org/#performing-a-microtask-checkpoint
    bool m_performing_a_microtask_checkpoint { false };

    Vector<WeakPtr<DOM::Document>> m_documents;

    // Used to implement step 4 of "perform a microtask checkpoint".
    // NOTE: These are weak references! ESO registers and unregisters itself from the event loop manually.
    Vector<RawPtr<EnvironmentSettingsObject>> m_related_environment_settings_objects;

    // https://html.spec.whatwg.org/multipage/webappapis.html#backup-incumbent-settings-object-stack
    Vector<JS::NonnullGCPtr<EnvironmentSettingsObject>> m_backup_incumbent_settings_object_stack;

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#termination-nesting-level
    size_t m_termination_nesting_level { 0 };

    bool m_execution_paused { false };

    bool m_skip_event_loop_processing_steps { false };

    bool m_is_running_reflow_steps { false };
};

EventLoop& main_thread_event_loop();
TaskID queue_a_task(HTML::Task::Source, JS::GCPtr<EventLoop>, JS::GCPtr<DOM::Document>, JS::NonnullGCPtr<JS::HeapFunction<void()>> steps);
TaskID queue_global_task(HTML::Task::Source, JS::Object&, JS::NonnullGCPtr<JS::HeapFunction<void()>> steps);
void queue_a_microtask(DOM::Document const*, JS::NonnullGCPtr<JS::HeapFunction<void()>> steps);
void perform_a_microtask_checkpoint();

}
