/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class ResizeHandle final : public ChromeWidget {
    GC_CELL(ResizeHandle, ChromeWidget);
    GC_DECLARE_ALLOCATOR(ResizeHandle);

public:
    static GC::Ref<ResizeHandle> create(GC::Heap&, PaintableBox&);

    bool contains(CSSPixelPoint position, ChromeMetrics const&) const;

    virtual MouseAction handle_pointer_event(FlyString const& type, unsigned button, CSSPixelPoint visual_viewport_position) override;
    virtual void mouse_enter() override { }
    virtual void mouse_leave() override { }

    virtual Optional<CSS::CursorPredefined> cursor() const override;

private:
    ResizeHandle(PaintableBox&);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<PaintableBox> m_paintable_box;
    GC::Ref<DOM::Element> m_element;
    OwnPtr<ElementResizeAction> m_resize_action;
};

}
