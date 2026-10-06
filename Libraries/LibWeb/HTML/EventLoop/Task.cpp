/*
 * Copyright (c) 2021-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IDAllocator.h>
#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Task);

static IDAllocator& unique_task_source_allocator()
{
    static NeverDestroyed<IDAllocator> allocator { static_cast<int>(Task::Source::UniqueTaskSourceStart) };
    return *allocator;
}

[[nodiscard]] static TaskID allocate_task_id()
{
    static u64 next_task_id = 1;
    return next_task_id++;
}

GC::Ref<Task> Task::create(Source source, GC::Ptr<DOM::Document const> document, GC::Ref<GC::Function<void()>> steps, Priority priority, GC::Ptr<GC::Function<void()>> discard_steps)
{
    return GC::Heap::the().allocate<Task>(source, document, move(steps), priority, discard_steps);
}

Task::Task(Source source, GC::Ptr<DOM::Document const> document, GC::Ref<GC::Function<void()>> steps, Priority priority, GC::Ptr<GC::Function<void()>> discard_steps)
    : m_id(allocate_task_id())
    , m_source(source)
    , m_priority(priority)
    , m_steps(steps)
    , m_discard_steps(discard_steps)
    , m_document(document)
{
}

Task::~Task() = default;

void Task::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_steps);
    visitor.visit(m_discard_steps);
    visitor.visit(m_document);
}

void Task::execute()
{
    auto scope_guard = SVG::ScopedSVGImageDocument::create_if_needed(m_document, SVG::ScopedSVGImageDocument::FrameRequests::RouteToCurrentImage);

    m_steps->function()();
}

void Task::discard()
{
    if (auto discard_steps = m_discard_steps) {
        m_discard_steps = nullptr;
        discard_steps->function()();
    }
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-task-runnable
bool Task::is_runnable() const
{
    // A task is runnable if its document is either null or fully active.
    // AD-HOC: Decoded SVG documents are not generally active but are made active while executing their tasks.
    return !m_document || m_document->is_fully_active() || (m_document->is_decoded_svg() && !m_document->has_been_destroyed());
}

bool Task::is_permanently_unrunnable() const
{
    return m_document && m_document->has_been_destroyed();
}

DOM::Document const* Task::document() const
{
    return m_document.ptr();
}

UniqueTaskSource::UniqueTaskSource()
    : source(static_cast<Task::Source>(unique_task_source_allocator().allocate()))
{
}

UniqueTaskSource::~UniqueTaskSource()
{
    unique_task_source_allocator().deallocate(static_cast<int>(source));
}

NonnullRefPtr<ParallelQueue> ParallelQueue::create()
{
    return adopt_ref(*new (nothrow) ParallelQueue);
}

TaskID ParallelQueue::enqueue(GC::Ref<GC::Function<void()>> algorithm)
{
    auto& event_loop = HTML::main_thread_event_loop();
    auto task = HTML::Task::create(m_task_source.source, nullptr, algorithm);
    event_loop.task_queue().add(task);
    return task->id();
}

}
