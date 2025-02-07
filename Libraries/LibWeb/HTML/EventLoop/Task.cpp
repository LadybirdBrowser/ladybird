/*
 * Copyright (c) 2021-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IDAllocator.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventLoop/Task.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Task);

static IDAllocator s_unique_task_source_allocator { static_cast<int>(Task::Source::UniqueTaskSourceStart) };

[[nodiscard]] static TaskID allocate_task_id()
{
    static u64 next_task_id = 1;
    return next_task_id++;
}

GC::Ref<Task> Task::create(JS::VM& vm, Source source, GC::Ptr<DOM::Document const> document, GC::Ref<GC::Function<void()>> steps)
{
    return vm.heap().allocate<Task>(source, document, move(steps));
}

Task::Task(Source source, GC::Ptr<DOM::Document const> document, GC::Ref<GC::Function<void()>> steps)
    : m_id(allocate_task_id())
    , m_source(source)
    , m_steps(steps)
    , m_document(document)
{
}

Task::~Task() = default;

void Task::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_steps);
    visitor.visit(m_document);
}

void Task::execute()
{
    m_steps->function()();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-task-runnable
bool Task::is_runnable() const
{
    // A task is runnable if its document is either null or fully active.
    if (!m_document)
        return true;

    // AD-HOC: If the document has been destroyed, we'll consider the task runnable.
    //         Otherwise it would get stuck here forever, since a destroyed document never becomes fully active again.
    if (m_document->has_been_destroyed())
        return true;

    return m_document->is_fully_active();
}

DOM::Document const* Task::document() const
{
    return m_document.ptr();
}

UniqueTaskSource::UniqueTaskSource()
    : source(static_cast<Task::Source>(s_unique_task_source_allocator.allocate()))
{
}

UniqueTaskSource::~UniqueTaskSource()
{
    s_unique_task_source_allocator.deallocate(static_cast<int>(source));
}

}
