/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>

namespace Web::Fetch::Infrastructure {

TaskDestination::TaskDestination(GC::Ref<JS::Object> global_object)
    : Variant(global_object)
{
    // AD-HOC: SVG images share a Window. Track it's document so that it can be made active when executing the task.
    if (auto* window = HTML::window_from_global_object(*global_object); window && window->associated_document().is_decoded_svg())
        document_override = window->associated_document();
}

void TaskDestination::visit_edges(GC::Cell::Visitor& visitor) const
{
    visitor.visit(static_cast<Variant const&>(*this));
    visitor.visit(document_override);
}

// https://fetch.spec.whatwg.org/#queue-a-fetch-task
HTML::TaskID queue_fetch_task(TaskDestination task_destination, GC::Ref<GC::Function<void()>> algorithm)
{
    VERIFY(!task_destination.has<Empty>());

    // 1. If taskDestination is a parallel queue, then enqueue algorithm to taskDestination.
    if (auto* parallel_queue = task_destination.get_pointer<NonnullRefPtr<HTML::ParallelQueue>>())
        return (*parallel_queue)->enqueue(algorithm);

    // 2. Otherwise, queue a global task on the networking task source with taskDestination and algorithm.
    auto scope_guard = SVG::ScopedSVGImageDocument::create_if_needed(task_destination.document_override, SVG::ScopedSVGImageDocument::FrameRequests::RouteToCurrentImage);

    return HTML::queue_global_task(HTML::Task::Source::Networking, task_destination.get<GC::Ref<JS::Object>>(), algorithm);
}

// AD-HOC: This overload allows tracking the queued task within the fetch controller so that we may cancel queued tasks
//         when the spec indicates that we must stop an ongoing fetch.
HTML::TaskID queue_fetch_task(GC::Ref<FetchController> fetch_controller, TaskDestination task_destination, GC::Ref<GC::Function<void()>> algorithm)
{
    auto fetch_task_id = fetch_controller->next_fetch_task_id();

    auto html_task_id = queue_fetch_task(task_destination, GC::create_function(GC::Heap::the(), [fetch_controller, fetch_task_id, algorithm]() {
        fetch_controller->fetch_task_complete(fetch_task_id);
        algorithm->function()();
    }));

    fetch_controller->fetch_task_queued(fetch_task_id, html_task_id);
    return html_task_id;
}

}
