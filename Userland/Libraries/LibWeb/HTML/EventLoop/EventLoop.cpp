/*
 * Copyright (c) 2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/Performance.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/Timer.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(EventLoop);

EventLoop::EventLoop(Type type)
    : m_type(type)
{
    m_task_queue = heap().allocate_without_realm<TaskQueue>(*this);
    m_microtask_queue = heap().allocate_without_realm<TaskQueue>(*this);
}

EventLoop::~EventLoop() = default;

void EventLoop::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_task_queue);
    visitor.visit(m_microtask_queue);
    visitor.visit(m_currently_running_task);
    visitor.visit(m_backup_incumbent_settings_object_stack);
}

void EventLoop::schedule()
{
    if (!m_system_event_loop_timer) {
        m_system_event_loop_timer = Platform::Timer::create_single_shot(0, [this] {
            process();
        });
    }

    if (!m_system_event_loop_timer->is_active())
        m_system_event_loop_timer->restart();
}

EventLoop& main_thread_event_loop()
{
    return *static_cast<Bindings::WebEngineCustomData*>(Bindings::main_thread_vm().custom_data())->event_loop;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#spin-the-event-loop
void EventLoop::spin_until(JS::SafeFunction<bool()> goal_condition)
{
    // FIXME: The spec wants us to do the rest of the enclosing algorithm (i.e. the caller)
    //    in the context of the currently running task on entry. That's not possible with this implementation.
    // 1. Let task be the event loop's currently running task.
    // 2. Let task source be task's source.

    // 3. Let old stack be a copy of the JavaScript execution context stack.
    // 4. Empty the JavaScript execution context stack.
    auto& vm = this->vm();
    vm.save_execution_context_stack();
    vm.clear_execution_context_stack();

    // 5. Perform a microtask checkpoint.
    perform_a_microtask_checkpoint();

    // 6. In parallel:
    //    1. Wait until the condition goal is met.
    //    2. Queue a task on task source to:
    //       1. Replace the JavaScript execution context stack with old stack.
    //       2. Perform any steps that appear after this spin the event loop instance in the original algorithm.
    //       NOTE: This is achieved by returning from the function.

    Platform::EventLoopPlugin::the().spin_until([&] {
        if (goal_condition())
            return true;
        if (m_task_queue->has_runnable_tasks()) {
            schedule();
            // FIXME: Remove the platform event loop plugin so that this doesn't look out of place
            Core::EventLoop::current().wake();
        }
        return goal_condition();
    });

    vm.restore_execution_context_stack();

    // 7. Stop task, allowing whatever algorithm that invoked it to resume.
    // NOTE: This is achieved by returning from the function.
}

void EventLoop::spin_processing_tasks_with_source_until(Task::Source source, JS::SafeFunction<bool()> goal_condition)
{
    auto& vm = this->vm();
    vm.save_execution_context_stack();
    vm.clear_execution_context_stack();

    perform_a_microtask_checkpoint();

    // NOTE: HTML event loop processing steps could run a task with arbitrary source
    m_skip_event_loop_processing_steps = true;

    Platform::EventLoopPlugin::the().spin_until([&] {
        if (goal_condition())
            return true;
        if (m_task_queue->has_runnable_tasks()) {
            auto tasks = m_task_queue->take_tasks_matching([&](auto& task) {
                return task.source() == source && task.is_runnable();
            });

            for (auto& task : tasks) {
                m_currently_running_task = task.ptr();
                task->execute();
                m_currently_running_task = nullptr;
            }
        }

        // FIXME: Remove the platform event loop plugin so that this doesn't look out of place
        Core::EventLoop::current().wake();
        return goal_condition();
    });

    m_skip_event_loop_processing_steps = false;

    schedule();

    vm.restore_execution_context_stack();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#event-loop-processing-model
void EventLoop::process()
{
    if (m_skip_event_loop_processing_steps)
        return;

    // An event loop must continually run through the following steps for as long as it exists:

    // 1. Let oldestTask be null.
    JS::GCPtr<Task> oldest_task;

    // 2. Set taskStartTime to the unsafe shared current time.
    double task_start_time = HighResolutionTime::unsafe_shared_current_time();

    // 3. Let taskQueue be one of the event loop's task queues, chosen in an implementation-defined manner,
    //    with the constraint that the chosen task queue must contain at least one runnable task.
    //    If there is no such task queue, then jump to the microtasks step below.
    auto& task_queue = *m_task_queue;

    // 4. Set oldestTask to the first runnable task in taskQueue, and remove it from taskQueue.
    oldest_task = task_queue.take_first_runnable();

    if (oldest_task) {
        // 5. Set the event loop's currently running task to oldestTask.
        m_currently_running_task = oldest_task.ptr();

        // 6. Perform oldestTask's steps.
        oldest_task->execute();

        // 7. Set the event loop's currently running task back to null.
        m_currently_running_task = nullptr;
    }

    // 8. Microtasks: Perform a microtask checkpoint.
    perform_a_microtask_checkpoint();

    // 9. Let hasARenderingOpportunity be false.
    [[maybe_unused]] bool has_a_rendering_opportunity = false;

    // FIXME: 10. Let now be the current high resolution time. [HRT]

    // FIXME: 11. If oldestTask is not null, then:

    // FIXME:     1. Let top-level browsing contexts be an empty set.

    // FIXME:     2. For each environment settings object settings of oldestTask's script evaluation environment settings object set, append setting's top-level browsing context to top-level browsing contexts.

    // FIXME:     3. Report long tasks, passing in taskStartTime, now (the end time of the task), top-level browsing contexts, and oldestTask.

    // FIXME: 12. Update the rendering: if this is a window event loop, then:

    // FIXME:     1. Let docs be all Document objects whose relevant agent's event loop is this event loop, sorted arbitrarily except that the following conditions must be met:
    //               - Any Document B whose browsing context's container document is A must be listed after A in the list.
    //               - If there are two documents A and B whose browsing contexts are both child browsing contexts whose container documents are another Document C, then the order of A and B in the list must match the shadow-including tree order of their respective browsing context containers in C's node tree.
    // FIXME: NOTE: The sort order specified above is missing here!
    Vector<JS::Handle<DOM::Document>> docs = documents_in_this_event_loop();

    auto for_each_fully_active_document_in_docs = [&](auto&& callback) {
        for (auto& document : docs) {
            if (document->is_fully_active())
                callback(*document);
        }
    };

    // AD-HOC: Since event loop processing steps do not constantly running in parallel, and
    //         something must trigger them, we need to manually schedule a repaint for all
    //         navigables that do not have a rendering opportunity at this event loop iteration.
    //         Otherwise their repaint will be delayed until something else will trigger event
    //         loop processing.
    for_each_fully_active_document_in_docs([&](DOM::Document& document) {
        auto navigable = document.navigable();
        if (navigable && !navigable->has_a_rendering_opportunity() && navigable->needs_repaint())
            schedule();
        if (navigable && navigable->has_a_rendering_opportunity())
            return;
        auto* browsing_context = document.browsing_context();
        if (!browsing_context)
            return;
        auto& page = browsing_context->page();
        page.client().schedule_repaint();
    });

    // 2. Rendering opportunities: Remove from docs all Document objects whose node navigables do not have a rendering opportunity.
    docs.remove_all_matching([&](auto& document) {
        auto navigable = document->navigable();
        return navigable && !navigable->has_a_rendering_opportunity();
    });

    // 3. If docs is not empty, then set hasARenderingOpportunity to true
    //    and set this event loop's last render opportunity time to taskStartTime.
    if (!docs.is_empty()) {
        has_a_rendering_opportunity = true;
        m_last_render_opportunity_time = task_start_time;
    }

    // FIXME:     4. Unnecessary rendering: Remove from docs all Document objects which meet both of the following conditions:
    //               - The user agent believes that updating the rendering of the Document's browsing context would have no visible effect, and
    //               - The Document's map of animation frame callbacks is empty.
    //            https://www.w3.org/TR/intersection-observer/#pending-initial-observation
    //            In the HTML Event Loops Processing Model, under the "Update the rendering" step, the "Unnecessary rendering" step should be
    //            modified to add an additional requirement for skipping the rendering update:
    //              - The document does not have pending initial IntersectionObserver targets.

    // FIXME:     5. Remove from docs all Document objects for which the user agent believes that it's preferable to skip updating the rendering for other reasons.

    // FIXME:     6. For each fully active Document in docs, flush autofocus candidates for that Document if its browsing context is a top-level browsing context.

    // 7. For each fully active Document in docs, run the resize steps for that Document, passing in now as the timestamp. [CSSOMVIEW]
    for_each_fully_active_document_in_docs([&](DOM::Document& document) {
        document.run_the_resize_steps();
    });

    // 8. For each fully active Document in docs, run the scroll steps for that Document, passing in now as the timestamp. [CSSOMVIEW]
    for_each_fully_active_document_in_docs([&](DOM::Document& document) {
        document.run_the_scroll_steps();
    });

    // 9. For each fully active Document in docs, evaluate media queries and report changes for that Document, passing in now as the timestamp. [CSSOMVIEW]
    for_each_fully_active_document_in_docs([&](DOM::Document& document) {
        document.evaluate_media_queries_and_report_changes();
    });

    // 10. For each fully active Document in docs, update animations and send events for that Document, passing in now as the timestamp. [WEBANIMATIONS]
    // Note: This is handled by the document's animation timer, however, if a document has any requestAnimationFrame callbacks, we need
    //       to dispatch events before that happens below. Not dispatching here would be observable.
    for_each_fully_active_document_in_docs([&](DOM::Document& document) {
        if (document.window()->animation_frame_callback_driver().has_callbacks()) {
            document.update_animations_and_send_events(document.window()->performance()->now());
        }
    });

    // FIXME:     11. For each fully active Document in docs, run the fullscreen steps for that Document, passing in now as the timestamp. [FULLSCREEN]

    // FIXME:     12. For each fully active Document in docs, if the user agent detects that the backing storage associated with a CanvasRenderingContext2D or an OffscreenCanvasRenderingContext2D, context, has been lost, then it must run the context lost steps for each such context:

    // FIXME:     13. For each fully active Document in docs, run the animation frame callbacks for that Document, passing in now as the timestamp.
    auto now = HighResolutionTime::unsafe_shared_current_time();
    for_each_fully_active_document_in_docs([&](DOM::Document& document) {
        run_animation_frame_callbacks(document, now);
    });

    // FIXME: This step is implemented following the latest specification, while the rest of this method uses an outdated spec.
    // NOTE: Gathering and broadcasting of resize observations need to happen after evaluating media queries but before
    //       updating intersection observations steps.
    for_each_fully_active_document_in_docs([&](DOM::Document& document) {
        // 1. Let resizeObserverDepth be 0.
        size_t resize_observer_depth = 0;

        // 2. While true:
        while (true) {
            // 1. Recalculate styles and update layout for doc.
            // NOTE: Recalculation of styles is handled by update_layout()
            document.update_layout();

            // FIXME: 2. Let hadInitialVisibleContentVisibilityDetermination be false.
            // FIXME: 3. For each element element with 'auto' used value of 'content-visibility':
            // FIXME: 4. If hadInitialVisibleContentVisibilityDetermination is true, then continue.

            // 5. Gather active resize observations at depth resizeObserverDepth for doc.
            document.gather_active_observations_at_depth(resize_observer_depth);

            // 6. If doc has active resize observations:
            if (document.has_active_resize_observations()) {
                // 1. Set resizeObserverDepth to the result of broadcasting active resize observations given doc.
                resize_observer_depth = document.broadcast_active_resize_observations();

                // 2. Continue.
                continue;
            }

            // 7. Otherwise, break.
            break;
        }

        // 3. If doc has skipped resize observations, then deliver resize loop error given doc.
        if (document.has_skipped_resize_observations()) {
            // FIXME: Deliver resize loop error.
        }
    });

    // 14. For each fully active Document in docs, run the update intersection observations steps for that Document, passing in now as the timestamp. [INTERSECTIONOBSERVER]
    for_each_fully_active_document_in_docs([&](DOM::Document& document) {
        document.run_the_update_intersection_observations_steps(now);
    });

    // FIXME:     15. Invoke the mark paint timing algorithm for each Document object in docs.

    // 16. For each fully active Document in docs, update the rendering or user interface of that Document and its browsing context to reflect the current state.
    for_each_fully_active_document_in_docs([&](DOM::Document& document) {
        auto navigable = document.navigable();
        if (navigable && navigable->needs_repaint()) {
            auto* browsing_context = document.browsing_context();
            auto& page = browsing_context->page();
            if (navigable->is_traversable()) {
                VERIFY(page.client().is_ready_to_paint());
                page.client().paint_next_frame();
            }
        }
    });

    // FIXME: Not in the spec: If there is a screenshot request queued, process it now.
    //        This prevents tests deadlocking on screenshot requests on macOS.
    for (auto& document : docs) {
        if (document->page().top_level_traversable()->needs_repaint())
            document->page().client().process_screenshot_requests();
    }

    // 13. If all of the following are true
    // - this is a window event loop
    // - there is no task in this event loop's task queues whose document is fully active
    // - this event loop's microtask queue is empty
    // - hasARenderingOpportunity is false
    // FIXME: has_a_rendering_opportunity is always true
    if (m_type == Type::Window && !task_queue.has_runnable_tasks() && m_microtask_queue->is_empty() /*&& !has_a_rendering_opportunity*/) {
        // 1. Set this event loop's last idle period start time to the unsafe shared current time.
        m_last_idle_period_start_time = HighResolutionTime::unsafe_shared_current_time();

        // 2. Let computeDeadline be the following steps:
        // NOTE: instead of passing around a function we use this event loop, which has compute_deadline()

        // 3. For each win of the same-loop windows for this event loop,
        //    perform the start an idle period algorithm for win with computeDeadline. [REQUESTIDLECALLBACK]
        for (auto& win : same_loop_windows())
            win->start_an_idle_period();
    }

    // FIXME: 14. If this is a worker event loop, then:

    // FIXME:     1. If this event loop's agent's single realm's global object is a supported DedicatedWorkerGlobalScope and the user agent believes that it would benefit from having its rendering updated at this time, then:
    // FIXME:        1. Let now be the current high resolution time. [HRT]
    // FIXME:        2. Run the animation frame callbacks for that DedicatedWorkerGlobalScope, passing in now as the timestamp.
    // FIXME:        3. Update the rendering of that dedicated worker to reflect the current state.

    // FIXME:     2. If there are no tasks in the event loop's task queues and the WorkerGlobalScope object's closing flag is true, then destroy the event loop, aborting these steps, resuming the run a worker steps described in the Web workers section below.

    // If there are eligible tasks in the queue, schedule a new round of processing. :^)
    if (m_task_queue->has_runnable_tasks() || (!m_microtask_queue->is_empty() && !m_performing_a_microtask_checkpoint))
        schedule();

    // For each doc of docs, process top layer removals given doc.
    for_each_fully_active_document_in_docs([&](DOM::Document& document) {
        document.process_top_layer_removals();
    });
}

// https://html.spec.whatwg.org/multipage/webappapis.html#queue-a-task
int queue_a_task(HTML::Task::Source source, JS::GCPtr<EventLoop> event_loop, JS::GCPtr<DOM::Document> document, JS::NonnullGCPtr<JS::HeapFunction<void()>> steps)
{
    // 1. If event loop was not given, set event loop to the implied event loop.
    if (!event_loop)
        event_loop = main_thread_event_loop();

    // FIXME: 2. If document was not given, set document to the implied document.

    // 3. Let task be a new task.
    // 4. Set task's steps to steps.
    // 5. Set task's source to source.
    // 6. Set task's document to the document.
    // 7. Set task's script evaluation environment settings object set to an empty set.
    auto task = HTML::Task::create(event_loop->vm(), source, document, steps);

    // 8. Let queue be the task queue to which source is associated on event loop.
    auto& queue = event_loop->task_queue();

    // 9. Append task to queue.
    queue.add(task);

    return queue.last_added_task()->id();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#queue-a-global-task
int queue_global_task(HTML::Task::Source source, JS::Object& global_object, JS::NonnullGCPtr<JS::HeapFunction<void()>> steps)
{
    // 1. Let event loop be global's relevant agent's event loop.
    auto& global_custom_data = verify_cast<Bindings::WebEngineCustomData>(*global_object.vm().custom_data());
    auto& event_loop = global_custom_data.event_loop;

    // 2. Let document be global's associated Document, if global is a Window object; otherwise null.
    DOM::Document* document { nullptr };
    if (is<HTML::Window>(global_object)) {
        auto& window_object = verify_cast<HTML::Window>(global_object);
        document = &window_object.associated_document();
    }

    // 3. Queue a task given source, event loop, document, and steps.
    return queue_a_task(source, *event_loop, document, steps);
}

// https://html.spec.whatwg.org/#queue-a-microtask
void queue_a_microtask(DOM::Document const* document, JS::NonnullGCPtr<JS::HeapFunction<void()>> steps)
{
    // 1. If event loop was not given, set event loop to the implied event loop.
    auto& event_loop = HTML::main_thread_event_loop();

    // FIXME: 2. If document was not given, set document to the implied document.

    // 3. Let microtask be a new task.
    // 4. Set microtask's steps to steps.
    // 5. Set microtask's source to the microtask task source.
    // 6. Set microtask's document to document.
    auto& vm = event_loop.vm();
    auto microtask = HTML::Task::create(vm, HTML::Task::Source::Microtask, document, steps);

    // FIXME: 7. Set microtask's script evaluation environment settings object set to an empty set.

    // 8. Enqueue microtask on event loop's microtask queue.
    event_loop.microtask_queue().enqueue(microtask);
}

void perform_a_microtask_checkpoint()
{
    main_thread_event_loop().perform_a_microtask_checkpoint();
}

// https://html.spec.whatwg.org/#perform-a-microtask-checkpoint
void EventLoop::perform_a_microtask_checkpoint()
{
    // 1. If the event loop's performing a microtask checkpoint is true, then return.
    if (m_performing_a_microtask_checkpoint)
        return;

    // 2. Set the event loop's performing a microtask checkpoint to true.
    m_performing_a_microtask_checkpoint = true;

    // 3. While the event loop's microtask queue is not empty:
    while (!m_microtask_queue->is_empty()) {
        // 1. Let oldestMicrotask be the result of dequeuing from the event loop's microtask queue.
        auto oldest_microtask = m_microtask_queue->dequeue();

        // 2. Set the event loop's currently running task to oldestMicrotask.
        m_currently_running_task = oldest_microtask;

        // 3. Run oldestMicrotask.
        oldest_microtask->execute();

        // 4. Set the event loop's currently running task back to null.
        m_currently_running_task = nullptr;
    }

    // 4. For each environment settings object whose responsible event loop is this event loop, notify about rejected promises on that environment settings object.
    for (auto& environment_settings_object : m_related_environment_settings_objects)
        environment_settings_object->notify_about_rejected_promises({});

    // FIXME: 5. Cleanup Indexed Database transactions.

    // 6. Perform ClearKeptObjects().
    vm().finish_execution_generation();

    // 7. Set the event loop's performing a microtask checkpoint to false.
    m_performing_a_microtask_checkpoint = false;
}

Vector<JS::Handle<DOM::Document>> EventLoop::documents_in_this_event_loop() const
{
    Vector<JS::Handle<DOM::Document>> documents;
    for (auto& document : m_documents) {
        VERIFY(document);
        documents.append(JS::make_handle(*document));
    }
    return documents;
}

void EventLoop::register_document(Badge<DOM::Document>, DOM::Document& document)
{
    m_documents.append(&document);
}

void EventLoop::unregister_document(Badge<DOM::Document>, DOM::Document& document)
{
    bool did_remove = m_documents.remove_first_matching([&](auto& entry) { return entry.ptr() == &document; });
    VERIFY(did_remove);
}

void EventLoop::push_onto_backup_incumbent_settings_object_stack(Badge<EnvironmentSettingsObject>, EnvironmentSettingsObject& environment_settings_object)
{
    m_backup_incumbent_settings_object_stack.append(environment_settings_object);
}

void EventLoop::pop_backup_incumbent_settings_object_stack(Badge<EnvironmentSettingsObject>)
{
    m_backup_incumbent_settings_object_stack.take_last();
}

EnvironmentSettingsObject& EventLoop::top_of_backup_incumbent_settings_object_stack()
{
    return m_backup_incumbent_settings_object_stack.last();
}

void EventLoop::register_environment_settings_object(Badge<EnvironmentSettingsObject>, EnvironmentSettingsObject& environment_settings_object)
{
    m_related_environment_settings_objects.append(&environment_settings_object);
}

void EventLoop::unregister_environment_settings_object(Badge<EnvironmentSettingsObject>, EnvironmentSettingsObject& environment_settings_object)
{
    bool did_remove = m_related_environment_settings_objects.remove_first_matching([&](auto& entry) { return entry == &environment_settings_object; });
    VERIFY(did_remove);
}

// https://html.spec.whatwg.org/multipage/webappapis.html#same-loop-windows
Vector<JS::Handle<HTML::Window>> EventLoop::same_loop_windows() const
{
    Vector<JS::Handle<HTML::Window>> windows;
    for (auto& document : documents_in_this_event_loop()) {
        if (document->is_fully_active())
            windows.append(JS::make_handle(document->window()));
    }
    return windows;
}

// https://html.spec.whatwg.org/multipage/webappapis.html#event-loop-processing-model:last-idle-period-start-time
double EventLoop::compute_deadline() const
{
    // 1. Let deadline be this event loop's last idle period start time plus 50.
    auto deadline = m_last_idle_period_start_time + 50;
    // 2. Let hasPendingRenders be false.
    auto has_pending_renders = false;
    // 3. For each windowInSameLoop of the same-loop windows for this event loop:
    for (auto& window : same_loop_windows()) {
        // 1. If windowInSameLoop's map of animation frame callbacks is not empty,
        //    or if the user agent believes that the windowInSameLoop might have pending rendering updates,
        //    set hasPendingRenders to true.
        if (window->has_animation_frame_callbacks())
            has_pending_renders = true;
        // FIXME: 2. Let timerCallbackEstimates be the result of getting the values of windowInSameLoop's map of active timers.
        // FIXME: 3. For each timeoutDeadline of timerCallbackEstimates, if timeoutDeadline is less than deadline, set deadline to timeoutDeadline.
    }
    // 4. If hasPendingRenders is true, then:
    if (has_pending_renders) {
        // 1. Let nextRenderDeadline be this event loop's last render opportunity time plus (1000 divided by the current refresh rate).
        // FIXME: Hardcoded to 60Hz
        auto next_render_deadline = m_last_render_opportunity_time + (1000.0 / 60.0);
        // 2. If nextRenderDeadline is less than deadline, then return nextRenderDeadline.
        if (next_render_deadline < deadline)
            return next_render_deadline;
    }
    // 5. Return deadline.
    return deadline;
}

}
