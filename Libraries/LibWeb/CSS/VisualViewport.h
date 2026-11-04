/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Bindings/VisualViewport.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web::CSS {

// https://drafts.csswg.org/cssom-view/#visualviewport
class WEB_API VisualViewport final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(VisualViewport, visual_viewport)

public:
    [[nodiscard]] static GC::Ref<VisualViewport> create(DOM::Document&);

    // The compositor zooms and pans the visual viewport too, and hears of every change made to it here.
    static void did_change_offset_or_scale(DOM::Document&);

    [[nodiscard]] GC::Ref<DOM::EventTarget> event_target() const;

    CSSPixelPoint offset() const;

    [[nodiscard]] double offset_left() const;
    [[nodiscard]] double offset_top() const;

    [[nodiscard]] double page_left() const;
    [[nodiscard]] double page_top() const;

    [[nodiscard]] double width() const;
    [[nodiscard]] double height() const;

    [[nodiscard]] double scale() const;

    void scroll_by(CSSPixelPoint delta);

    void zoom(CSSPixelPoint position, double scale_delta);
    void set_scale_zoomed_by_compositor(double scale);
    // The scale, if this thread changed it since the compositor last heard of it.
    Optional<double> take_scale_for_compositor();
    CSSPixelPoint map_to_layout_viewport(CSSPixelPoint) const;
    CSSPixelPoint map_from_layout_viewport(CSSPixelPoint) const;
    CSSPixelRect map_from_layout_viewport(CSSPixelRect const&) const;
    void reset();
};

}
