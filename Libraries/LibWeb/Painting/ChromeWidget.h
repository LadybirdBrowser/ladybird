/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/HashMap.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Utf16FlyString.h>
#include <AK/Weakable.h>
#include <LibGC/Cell.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Painting/Scrolling.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web::Painting {

enum class MouseAction : u8 {
    None,
    CaptureInput,
};

enum class ChromeWidgetKind : u8 {
    None,
    ResizeHandle,
    HorizontalScrollbar,
    VerticalScrollbar,
};

struct PhysicalResizeAxes {
    bool horizontal;
    bool vertical;
};

PhysicalResizeAxes physical_resize_axes(Layout::Node const&);

class ResizeHandle;

// The compositor takes the pointer input of scrollbars, so the resize handle is the only chrome widget the main thread
// keeps.
class ChromeWidgetRegistry : public RefCounted<ChromeWidgetRegistry> {
public:
    ChromeWidgetRegistry();
    ~ChromeWidgetRegistry();

    RefPtr<ResizeHandle> resize_handle(Compositing::RustFFI::NodeSlotId) const;
    NonnullRefPtr<ResizeHandle> get_or_create_resize_handle(Layout::NodeArena&, Compositing::RustFFI::NodeSlotId);
    void drop_widgets_for_slot(Compositing::RustFFI::NodeSlotId);
    void clear();

private:
    HashMap<u32, NonnullRefPtr<ResizeHandle>> m_resize_handles;
};

class ChromeWidget
    : public RefCounted<ChromeWidget>
    , public Weakable<ChromeWidget> {
public:
    virtual ~ChromeWidget() = default;

    virtual MouseAction handle_pointer_event(Utf16FlyString const& type, unsigned button, CSSPixelPoint visual_viewport_position) = 0;

    virtual Optional<CSS::CursorPredefined> cursor() const { return {}; }

protected:
    ChromeWidget(Layout::NodeArena&, Compositing::RustFFI::NodeSlotId);

    Layout::Node* layout_node() const;

private:
    friend class ChromeWidgetRegistry;

    void detach(Badge<ChromeWidgetRegistry>);
    virtual void did_detach() { }

    NonnullRefPtr<Layout::NodeArena> m_arena;
    Compositing::RustFFI::NodeSlotId m_slot;
};

}
