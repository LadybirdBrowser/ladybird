/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/EnumBits.h>
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
#include <LibWeb/PixelUnits.h>

namespace Web {

struct ChromeMetrics;

}

namespace Web::Painting {

enum class MouseAction : u8 {
    None = 0,
    CaptureInput = 1 << 0,
    SwallowEvent = 1 << 1,
};

AK_ENUM_BITWISE_OPERATORS(MouseAction);

enum class ChromeWidgetKind : u8 {
    None,
    ResizeHandle,
    HorizontalScrollbar,
    VerticalScrollbar,
};

struct ScrollbarData {
    CSSPixelRect gutter_rect;
    CSSPixelRect thumb_rect;
    CSSPixelRect track_rect;
    CSSPixelFraction thumb_travel_to_scroll_ratio { 0 };
};

enum class ScrollbarSizing {
    Regular,
    Enlarged,
};

struct PhysicalResizeAxes {
    bool horizontal;
    bool vertical;
};

CSS::ScrollbarColorData scrollbar_colors_for_paint(Layout::NodeWithStyle const&);
Optional<ScrollbarData> compute_scrollbar_data(Layout::Node const&, ScrollDirection, ChromeMetrics const&, ScrollStateSnapshot const* = nullptr, ScrollbarSizing = ScrollbarSizing::Regular);
Optional<CSSPixelRect> absolute_scrollbar_rect(Layout::Node const&, ScrollDirection, bool with_gutter, ChromeMetrics const&);
bool resizer_contains(Layout::Node const&, CSSPixelPoint, ChromeMetrics const&);
Optional<CSSPixelRect> absolute_resizer_rect(Layout::Node const&, ChromeMetrics const&);
bool has_resizer(Layout::Node const&);
bool is_chrome_mirrored(Layout::Node const&);
PhysicalResizeAxes physical_resize_axes(Layout::Node const&);

class Scrollbar;
class ResizeHandle;

class ChromeWidgetRegistry : public RefCounted<ChromeWidgetRegistry> {
public:
    ChromeWidgetRegistry();
    ~ChromeWidgetRegistry();

    RefPtr<Scrollbar> scrollbar(Layout::RustFFI::PaintableSlotId, ScrollDirection) const;
    NonnullRefPtr<Scrollbar> get_or_create_scrollbar(Layout::NodeArena&, Layout::RustFFI::PaintableSlotId, ScrollDirection);
    RefPtr<ResizeHandle> resize_handle(Layout::RustFFI::PaintableSlotId) const;
    NonnullRefPtr<ResizeHandle> get_or_create_resize_handle(Layout::NodeArena&, Layout::RustFFI::PaintableSlotId);
    void drop_widgets_for_slot(Layout::RustFFI::PaintableSlotId);
    void clear();

private:
    struct Entry {
        RefPtr<Scrollbar> horizontal_scrollbar;
        RefPtr<Scrollbar> vertical_scrollbar;
        RefPtr<ResizeHandle> resize_handle;
    };

    HashMap<u32, Entry> m_entries;
};

class ChromeWidget
    : public RefCounted<ChromeWidget>
    , public Weakable<ChromeWidget> {
public:
    virtual ~ChromeWidget() = default;

    virtual bool contains(CSSPixelPoint, ChromeMetrics const&) const = 0;
    virtual MouseAction handle_pointer_event(Utf16FlyString const& type, unsigned button, CSSPixelPoint visual_viewport_position) = 0;
    virtual void mouse_enter() = 0;
    virtual void mouse_leave() = 0;

    virtual Optional<CSS::CursorPredefined> cursor() const { return {}; }

protected:
    ChromeWidget(Layout::NodeArena&, Layout::RustFFI::PaintableSlotId);

    Layout::Node* layout_node() const;

private:
    friend class ChromeWidgetRegistry;

    void detach(Badge<ChromeWidgetRegistry>);
    virtual void did_detach() { }

    NonnullRefPtr<Layout::NodeArena> m_arena;
    Layout::RustFFI::PaintableSlotId m_slot;
};

}
