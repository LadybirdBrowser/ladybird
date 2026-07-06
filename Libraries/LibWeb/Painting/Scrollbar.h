/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Painting {

class Scrollbar final : public ChromeWidget {
public:
    static NonnullRefPtr<Scrollbar> create(Paintable&, Paintable::ScrollDirection);

    Paintable::ScrollDirection direction() const { return m_direction; }
    bool is_enlarged() const { return m_hovered || m_thumb_grab_position.has_value(); }

    virtual bool contains(CSSPixelPoint position, ChromeMetrics const&) const override;

    virtual MouseAction handle_pointer_event(FlyString const& type, unsigned button, CSSPixelPoint visual_viewport_position) override;
    virtual void mouse_enter() override;
    virtual void mouse_leave() override;

private:
    Scrollbar(Paintable&, Paintable::ScrollDirection);

    MouseAction mouse_down(CSSPixelPoint, unsigned button);
    MouseAction mouse_move(CSSPixelPoint);
    MouseAction mouse_up(CSSPixelPoint, unsigned button);
    bool scroll_to_mouse_position(CSSPixelPoint);

    WeakPtr<Paintable> m_paintable_box;
    Paintable::ScrollDirection m_direction;
    bool m_hovered { false };
    Optional<CSSPixels> m_thumb_grab_position;
};

}
