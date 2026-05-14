/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Bindings/Window.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web {

class SmoothScrollTask : public GC::Cell {
    GC_CELL(SmoothScrollTask, GC::Cell);
    GC_DECLARE_ALLOCATOR(SmoothScrollTask);

public:
    SmoothScrollTask(GC::Ref<WebIDL::Promise>, CSSPixelPoint source_offset, CSSPixelPoint target_offset, double timestamp);
    SmoothScrollTask(AK::RefPtr<Painting::PaintableBox> const&, GC::Ref<WebIDL::Promise>, CSSPixelPoint source_offset, CSSPixelPoint target_offset, double timestamp);

    void set_source_offset(CSSPixelPoint value) { m_source_offset = value; }
    void set_target_offset(CSSPixelPoint value) { m_target_offset = value; }
    void set_start_time(double value) { m_start_time = value; }
    void set_is_ongoing(bool value) { m_is_ongoing = value; }

    GC::Ref<WebIDL::Promise> scroll_promise() { return m_scroll_promise; }
    AK::RefPtr<Painting::PaintableBox> box();
    CSSPixelPoint source_offset() { return m_source_offset; }
    CSSPixelPoint target_offset() { return m_target_offset; }
    double start_time() const { return m_start_time; }
    void update_duration();
    double duration() const { return m_duration; }
    bool is_ongoing() const { return m_is_ongoing; }

    virtual void visit_edges(Cell::Visitor&) override;

private:
    GC::Ref<WebIDL::Promise> m_scroll_promise;
    AK::WeakPtr<Painting::PaintableBox> m_box;
    CSSPixelPoint m_source_offset;
    CSSPixelPoint m_target_offset;
    double m_start_time;
    double m_duration;
    bool m_is_ongoing { false };
};

class SmoothScrollHandler : public GC::Cell {
    GC_CELL(SmoothScrollHandler, GC::Cell);
    GC_DECLARE_ALLOCATOR(SmoothScrollHandler);

public:
    [[nodiscard]] static GC::Ref<SmoothScrollHandler> create(GC::Heap&, GC::Ref<DOM::Document>);
    SmoothScrollHandler(GC::Ref<DOM::Document> document);

    void process();
    void start_viewport_scroll(GC::Ref<WebIDL::Promise> promise, CSSPixelPoint target_offset);
    void start_visual_viewport_scroll(GC::Ref<WebIDL::Promise> promise, CSSPixelPoint target_offset);
    void start_box_scroll(GC::Ref<WebIDL::Promise> promise, CSSPixelPoint target_offset, AK::RefPtr<Painting::PaintableBox> const& box);

    GC::Ptr<SmoothScrollTask> ongoing_viewport_scroll() { return m_ongoing_viewport_scroll; }
    GC::Ptr<SmoothScrollTask> ongoing_visual_viewport_scroll() { return m_ongoing_visual_viewport_scroll; }
    GC::Ptr<SmoothScrollTask> ongoing_scroll_of_box(Painting::PaintableBox& box);

    void abort_any_ongoing_viewport_scroll();
    void abort_any_ongoing_visual_viewport_scroll();
    void abort_any_ongoing_scroll_of_box(Painting::PaintableBox& box);

    static Bindings::ScrollBehavior resolve_scroll_behavior(DOM::Element const&, Bindings::ScrollBehavior);
    static Bindings::ScrollBehavior resolve_scroll_behavior(HTML::Navigable const&, Bindings::ScrollBehavior);
    static Bindings::ScrollBehavior resolve_scroll_behavior(CSS::VisualViewport&, Bindings::ScrollBehavior);
    static Bindings::ScrollBehavior resolve_scroll_behavior(Painting::PaintableBox&, Bindings::ScrollBehavior);

    virtual void visit_edges(Cell::Visitor&) override;

private:
    GC::Ref<DOM::Document> m_document;

    GC::Ptr<SmoothScrollTask> m_ongoing_viewport_scroll;
    GC::Ptr<SmoothScrollTask> m_ongoing_visual_viewport_scroll;
    Vector<GC::Ref<SmoothScrollTask>> m_ongoing_box_scrolls;

    void finish(SmoothScrollTask&);

    static CSSPixelPoint current_scroll_offset(SmoothScrollTask& task, double progress);
    static double task_progress(SmoothScrollTask& task, double timestamp);
    static double ease(double);
};

}
