/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/FontFaceSet.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
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

    // 1. Let oldestTask and taskStartTime be null.
    JS::GCPtr<Task> oldest_task;
    [[maybe_unused]] double task_start_time = 0;

    // 2. If the event loop has a task queue with at least one runnable task, then:
    if (m_task_queue->has_runnable_tasks()) {
        // 1. Let taskQueue be one such task queue, chosen in an implementation-defined manner.
        auto task_queue = m_task_queue;

        // 2. Set taskStartTime to the unsafe shared current time.
        task_start_time = HighResolutionTime::unsafe_shared_current_time();

        // 3. Set oldestTask to the first runnable task in taskQueue, and remove it from taskQueue.
        oldest_task = task_queue->take_first_runnable();

        // FIXME: 4. If oldestTask's document is not null, then record task start time given taskStartTime and oldestTask's document.

        // 5. Set the event loop's currently running task to oldestTask.
        m_currently_running_task = oldest_task.ptr();

        // 6. Perform oldestTask's steps.
        oldest_task->execute();

        // 7. Set the event loop's currently running task back to null.
        m_currently_running_task = nullptr;

        // 8. Perform a microtask checkpoint.
        perform_a_microtask_checkpoint();
    }

    // 3. Let taskEndTime be the unsafe shared current time. [HRT]
    [[maybe_unused]] auto task_end_time = HighResolutionTime::unsafe_shared_current_time();

    // 4. If oldestTask is not null, then:
    if (oldest_task) {
        // FIXME: 1. Let top-level browsing contexts be an empty set.
        // FIXME: 2. For each environment settings object settings of oldestTask's script evaluation environment settings object set:
        // FIXME: 2.1. Let global be settings's global object.
        // FIXME: 2.2. If global is not a Window object, then continue.
        // FIXME: 2.3. If global's browsing context is null, then continue.
        // FIXME: 2.4. Let tlbc be global's browsing context's top-level browsing context.
        // FIXME: 2.5. If tlbc is not null, then append it to top-level browsing contexts.
        // FIXME: 3. Report long tasks, passing in taskStartTime, taskEndTime, top-level browsing contexts, and oldestTask.
        // FIXME: 4. If oldestTask's document is not null, then record task end time given taskEndTime and oldestTask's document.
    }

    // 5. If this is a window event loop that has no runnable task in this event loop's task queues, then:
    if (m_type == Type::Window && !m_task_queue->has_runnable_tasks()) {
        // 1. Set this event loop's last idle period start time to the unsafe shared current time.
        m_last_idle_period_start_time = HighResolutionTime::unsafe_shared_current_time();

        // 2. Let computeDeadline be the following steps:
        // Implemented in EventLoop::compute_deadline()

        // 3. For each win of the same-loop windows for this event loop, perform the start an idle period algorithm for win with the following step: return the result of calling computeDeadline, coarsened given win's relevant settings object's cross-origin isolated capability. [REQUESTIDLECALLBACK]
        for (auto& win : same_loop_windows()) {
            win->start_an_idle_period();
        }
    }

    // If there are eligible tasks in the queue, schedule a new round of processing. :^)
    if (m_task_queue->has_runnable_tasks() || (!m_microtask_queue->is_empty() && !m_performing_a_microtask_checkpoint)) {
        schedule();
    }
}

// https://html.spec.whatwg.org/multipage/webappapis.html#event-loop-processing-model
void EventLoop::queue_task_to_update_the_rendering()
{
    // FIXME: 1. Wait until at least one navigable whose active document's relevant agent's event loop is eventLoop might have a rendering opportunity.

    // 2. Set eventLoop's last render opportunity time to the unsafe shared current time.
    m_last_render_opportunity_time = HighResolutionTime::unsafe_shared_current_time();

    // OPTIMIZATION: If there are already rendering tasks in the queue, we don't need to queue another one.
    if (m_task_queue->has_rendering_tasks()) {
        return;
    }

    // 3. For each navigable that has a rendering opportunity, queue a global task on the rendering task source given navigable's active window to update the rendering:
    for (auto& navigable : all_navigables()) {
        if (!navigable->is_traversable())
            continue;
        if (!navigable->has_a_rendering_opportunity())
            continue;

        auto document = navigable->active_document();
        if (!document)
            continue;
        if (document->is_decoded_svg())
            continue;

        queue_global_task(Task::Source::Rendering, *navigable->active_window(), JS::create_heap_function(navigable->heap(), [this] mutable {
            VERIFY(!m_is_running_rendering_task);
            m_is_running_rendering_task = true;
            ScopeGuard const guard = [this] {
                m_is_running_rendering_task = false;
            };

            // FIXME: 1. Let frameTimestamp be eventLoop's last render opportunity time.

            // FIXME: 2. Let docs be all fully active Document objects whose relevant agent's event loop is eventLoop, sorted arbitrarily except that the following conditions must be met:
            auto docs = documents_in_this_event_loop();
            docs.remove_all_matching([&](auto& document) {
                return !document->is_fully_active();
            });

            // 3. Filter non-renderable documents: Remove from docs any Document object doc for which any of the following are true:
            docs.remove_all_matching([&](auto const& document) {
                auto navigable = document->navigable();
                if (!navigable)
                    return true;

                // FIXME: doc is render-blocked;

                // doc's visibility state is "hidden";
                if (document->visibility_state() == "hidden"sv)
                    return true;

                // FIXME: doc's rendering is suppressed for view transitions; or

                // doc's node navigable doesn't currently have a rendering opportunity.
                if (!navigable->has_a_rendering_opportunity())
                    return true;

                return false;
            });

            // FIXME: 4. Unnecessary rendering: Remove from docs any Document object doc for which all of the following are true:

            // FIXME: 5. Remove from docs all Document objects for which the user agent believes that it's preferable to skip updating the rendering for other reasons.

            // FIXME: 6. For each doc of docs, reveal doc.

            // FIXME: 7. For each doc of docs, flush autofocus candidates for doc if its node navigable is a top-level traversable.

            // 8. For each doc of docs, run the resize steps for doc. [CSSOMVIEW]
            for (auto& document : docs) {
                document->run_the_resize_steps();
            }

            // 9. For each doc of docs, run the scroll steps for doc. [CSSOMVIEW]
            for (auto& document : docs) {
                document->run_the_scroll_steps();
            }

            // 10. For each doc of docs, evaluate media queries and report changes for doc. [CSSOMVIEW]
            for (auto& document : docs) {
                document->evaluate_media_queries_and_report_changes();
            }

            // 11. For each doc of docs, update animations and send events for doc, passing in relative high resolution time given frameTimestamp and doc's relevant global object as the timestamp [WEBANIMATIONS]
            for (auto& document : docs) {
                document->update_animations_and_send_events(document->window()->performance()->now());
            };

            // FIXME: 12. For each doc of docs, run the fullscreen steps for doc. [FULLSCREEN]

            // FIXME: 13. For each doc of docs, if the user agent detects that the backing storage associated with a CanvasRenderingContext2D or an OffscreenCanvasRenderingContext2D, context, has been lost, then it must run the context lost steps for each such context:

            // 14. For each doc of docs, run the animation frame callbacks for doc, passing in the relative high resolution time given frameTimestamp and doc's relevant global object as the timestamp.
            auto now = HighResolutionTime::unsafe_shared_current_time();
            for (auto& document : docs) {
                run_animation_frame_callbacks(*document, now);
            }

            // FIXME: 15. Let unsafeStyleAndLayoutStartTime be the unsafe shared current time.

            // 16. For each doc of docs:
            for (auto& document : docs) {
                // 1. Let resizeObserverDepth be 0.
                size_t resize_observer_depth = 0;

                // 2. While true:
                while (true) {
                    // 1. Recalculate styles and update layout for doc.
                    // NOTE: Recalculation of styles is handled by update_layout()
                    document->update_layout();

                    // FIXME: 2. Let hadInitialVisibleContentVisibilityDetermination be false.
                    // FIXME: 3. For each element element with 'auto' used value of 'content-visibility':
                    // FIXME: 4. If hadInitialVisibleContentVisibilityDetermination is true, then continue.

                    // 5. Gather active resize observations at depth resizeObserverDepth for doc.
                    document->gather_active_observations_at_depth(resize_observer_depth);

                    // 6. If doc has active resize observations:
                    if (document->has_active_resize_observations()) {
                        // 1. Set resizeObserverDepth to the result of broadcasting active resize observations given doc.
                        resize_observer_depth = document->broadcast_active_resize_observations();

                        // 2. Continue.
                        continue;
                    }

                    // 7. Otherwise, break.
                    break;
                }

                // 3. If doc has skipped resize observations, then deliver resize loop error given doc.
                if (document->has_skipped_resize_observations()) {
                    // FIXME: Deliver resize loop error.
                }
            }

            // FIXME: 17. For each doc of docs, if the focused area of doc is not a focusable area, then run the focusing steps for doc's viewport, and set doc's relevant global object's navigation API's focus changed during ongoing navigation to false.

            // FIXME: 18. For each doc of docs, perform pending transition operations for doc. [CSSVIEWTRANSITIONS]

            // 19. For each doc of docs, run the update intersection observations steps for doc, passing in the relative high resolution time given now and doc's relevant global object as the timestamp. [INTERSECTIONOBSERVER]
            for (auto& document : docs) {
                document->run_the_update_intersection_observations_steps(now);
            }

            // FIXME: 20. For each doc of docs, record rendering time for doc given unsafeStyleAndLayoutStartTime.

            // FIXME: 21. For each doc of docs, mark paint timing for doc.

            // 22. For each doc of docs, update the rendering or user interface of doc and its node navigable to reflect the current state.
            for (auto& document : docs) {
                document->page().client().process_screenshot_requests();
                auto navigable = document->navigable();
                if (navigable && document->needs_repaint()) {
                    auto* browsing_context = document->browsing_context();
                    auto& page = browsing_context->page();
                    if (navigable->is_traversable()) {
                        VERIFY(page.client().is_ready_to_paint());
                        page.client().paint_next_frame();
                    }
                }
            }

            // 23. For each doc of docs, process top layer removals given doc.
            for (auto& document : docs) {
                document->process_top_layer_removals();
            }

            for (auto& document : docs) {
                if (document->readiness() == HTML::DocumentReadyState::Complete && document->style_computer().number_of_css_font_faces_with_loading_in_progress() == 0) {
                    HTML::TemporaryExecutionContext context(HTML::relevant_settings_object(*document), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
                    document->fonts()->resolve_ready_promise();
                }
            }
        }));
    }
}

// https://html.spec.whatwg.org/multipage/webappapis.html#queue-a-task
TaskID queue_a_task(HTML::Task::Source source, JS::GCPtr<EventLoop> event_loop, JS::GCPtr<DOM::Document> document, JS::NonnullGCPtr<JS::HeapFunction<void()>> steps)
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
    auto& queue = source == HTML::Task::Source::Microtask ? event_loop->microtask_queue() : event_loop->task_queue();

    // 9. Append task to queue.
    queue.add(task);

    return queue.last_added_task()->id();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#queue-a-global-task
TaskID queue_global_task(HTML::Task::Source source, JS::Object& global_object, JS::NonnullGCPtr<JS::HeapFunction<void()>> steps)
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
        if (document->is_decoded_svg())
            continue;
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
