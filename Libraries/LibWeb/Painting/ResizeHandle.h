/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Weak.h>
#include <LibWeb/Painting/ChromeWidget.h>

namespace Web::Painting {

class ResizeHandle final : public ChromeWidget {
public:
    static NonnullRefPtr<ResizeHandle> create(Layout::NodeArena&, Layout::RustFFI::NodeSlotId);

    virtual MouseAction handle_pointer_event(Utf16FlyString const& type, unsigned button, CSSPixelPoint visual_viewport_position) override;
    virtual void mouse_enter() override { }
    virtual void mouse_leave() override { }

    virtual Optional<CSS::CursorPredefined> cursor() const override;

private:
    ResizeHandle(Layout::NodeArena&, Layout::RustFFI::NodeSlotId);

    GC::Weak<DOM::Element> m_element;
    OwnPtr<ElementResizeAction> m_resize_action;
};

}
