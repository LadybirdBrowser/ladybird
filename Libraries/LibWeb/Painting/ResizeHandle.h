/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Weak.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class ResizeHandle final : public ChromeWidget {
public:
    static NonnullRefPtr<ResizeHandle> create(PaintableBox&);

    bool contains(CSSPixelPoint position, ChromeMetrics const&) const;

    virtual MouseAction handle_pointer_event(FlyString const& type, unsigned button, CSSPixelPoint visual_viewport_position) override;
    virtual void mouse_enter() override { }
    virtual void mouse_leave() override { }

    virtual Optional<CSS::CursorPredefined> cursor() const override;

private:
    ResizeHandle(PaintableBox&);

    WeakPtr<PaintableBox> m_paintable_box;
    GC::Weak<DOM::Element> m_element;
    OwnPtr<ElementResizeAction> m_resize_action;
};

}
