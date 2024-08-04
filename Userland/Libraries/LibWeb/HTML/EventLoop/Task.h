/*
 * Copyright (c) 2021-2024, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/CellAllocator.h>
#include <LibJS/SafeFunction.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

struct UniqueTaskSource;

AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(u64, TaskID, Comparison);

class Task final : public JS::Cell {
    JS_CELL(Task, JS::Cell);
    JS_DECLARE_ALLOCATOR(Task);

public:
    // https://html.spec.whatwg.org/multipage/webappapis.html#generic-task-sources
    enum class Source {
        Unspecified,
        DOMManipulation,
        UserInteraction,
        Networking,
        HistoryTraversal,
        IdleTask,
        PostedMessage,
        Microtask,
        TimerTask,
        JavaScriptEngine,

        // https://html.spec.whatwg.org/multipage/webappapis.html#navigation-and-traversal-task-source
        NavigationAndTraversal,

        // https://w3c.github.io/FileAPI/#fileReadingTaskSource
        FileReading,

        // https://www.w3.org/TR/intersection-observer/#intersectionobserver-task-source
        IntersectionObserver,

        // https://w3c.github.io/performance-timeline/#dfn-performance-timeline-task-source
        PerformanceTimeline,

        // https://html.spec.whatwg.org/multipage/canvas.html#canvas-blob-serialisation-task-source
        CanvasBlobSerializationTask,

        // https://w3c.github.io/clipboard-apis/#clipboard-task-source
        Clipboard,

        // https://w3c.github.io/permissions/#permissions-task-source
        Permissions,

        // https://drafts.csswg.org/css-font-loading/#task-source
        FontLoading,

        // https://html.spec.whatwg.org/multipage/server-sent-events.html#remote-event-task-source
        RemoteEvent,

        // !!! IMPORTANT: Keep this field last!
        // This serves as the base value of all unique task sources.
        // Some elements, such as the HTMLMediaElement, must have a unique task source per instance.
        UniqueTaskSourceStart
    };

    static JS::NonnullGCPtr<Task> create(JS::VM&, Source, JS::GCPtr<DOM::Document const>, JS::NonnullGCPtr<JS::HeapFunction<void()>> steps);

    virtual ~Task() override;

    [[nodiscard]] TaskID id() const { return m_id; }
    Source source() const { return m_source; }
    void execute();

    DOM::Document const* document() const;

    bool is_runnable() const;

private:
    Task(Source, JS::GCPtr<DOM::Document const>, JS::NonnullGCPtr<JS::HeapFunction<void()>> steps);

    virtual void visit_edges(Visitor&) override;

    TaskID m_id {};
    Source m_source { Source::Unspecified };
    JS::NonnullGCPtr<JS::HeapFunction<void()>> m_steps;
    JS::GCPtr<DOM::Document const> m_document;
};

struct UniqueTaskSource {
    UniqueTaskSource();
    ~UniqueTaskSource();

    Task::Source const source;
};

}
