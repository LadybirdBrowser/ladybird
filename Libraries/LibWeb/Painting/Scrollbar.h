/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/ChromeWidget.h>

namespace Web::Painting {

class Scrollbar final : public ChromeWidget {
public:
    static NonnullRefPtr<Scrollbar> create(Layout::NodeArena&, Compositing::RustFFI::NodeSlotId, ScrollDirection);

    bool is_enlarged() const { return m_hovered || m_drag_is_driven_by_compositor; }

    // The compositor scrolls for a drag it drives, and this scrollbar only keeps looking dragged until the release.
    void begin_drag_driven_by_compositor();

    virtual MouseAction handle_pointer_event(Utf16FlyString const& type, unsigned button, CSSPixelPoint visual_viewport_position) override;
    virtual void mouse_enter() override;
    virtual void mouse_leave() override;

private:
    Scrollbar(Layout::NodeArena&, Compositing::RustFFI::NodeSlotId, ScrollDirection);

    void release_drag();
    void push_enlarged_state();
    virtual void did_detach() override;

    ScrollDirection m_direction;
    bool m_hovered { false };
    bool m_drag_is_driven_by_compositor { false };
};

}
