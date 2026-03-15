/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class Scrollbar final : public ChromeWidget {
    GC_CELL(Scrollbar, ChromeWidget);
    GC_DECLARE_ALLOCATOR(Scrollbar);

public:
    static GC::Ref<Scrollbar> create(GC::Heap&, PaintableBox&, PaintableBox::ScrollDirection);

    PaintableBox::ScrollDirection direction() const { return m_direction; }
    bool is_enlarged() const { return m_hovered || m_thumb_grab_position.has_value(); }

    bool contains(CSSPixelPoint position, ChromeMetrics const&) const;

    virtual MouseAction handle_pointer_event(FlyString const& type, unsigned button, CSSPixelPoint visual_viewport_position) override;
    virtual void mouse_enter() override;
    virtual void mouse_leave() override;

private:
    Scrollbar(PaintableBox&, PaintableBox::ScrollDirection);

    virtual void visit_edges(Cell::Visitor&) override;

    MouseAction mouse_down(CSSPixelPoint, unsigned button);
    MouseAction mouse_move(CSSPixelPoint);
    MouseAction mouse_up(CSSPixelPoint, unsigned button);
    void scroll_to_mouse_position(CSSPixelPoint);

    GC::Ref<PaintableBox> m_paintable_box;
    PaintableBox::ScrollDirection m_direction;
    bool m_hovered { false };
    Optional<CSSPixels> m_thumb_grab_position;
};

}
