/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/PixelUnits.h>

namespace Web::CSS {

// https://drafts.csswg.org/cssom-view/#visualviewport
class VisualViewport final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(VisualViewport, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(VisualViewport);

public:
    [[nodiscard]] static GC::Ref<VisualViewport> create(DOM::Document&);

    virtual ~VisualViewport() override = default;

    CSSPixelPoint offset() const { return m_offset; }

    [[nodiscard]] double offset_left() const;
    [[nodiscard]] double offset_top() const;

    [[nodiscard]] double page_left() const;
    [[nodiscard]] double page_top() const;

    [[nodiscard]] double width() const;
    [[nodiscard]] double height() const;

    [[nodiscard]] double scale() const;

    void set_onresize(WebIDL::CallbackType*);
    WebIDL::CallbackType* onresize();
    void set_onscroll(WebIDL::CallbackType*);
    WebIDL::CallbackType* onscroll();
    void set_onscrollend(WebIDL::CallbackType*);
    WebIDL::CallbackType* onscrollend();

    void scroll_by(CSSPixelPoint delta) { m_offset += delta; }

    [[nodiscard]] Gfx::AffineTransform transform() const;
    void zoom(CSSPixelPoint position, double scale_delta);
    CSSPixelPoint map_to_layout_viewport(CSSPixelPoint) const;
    void reset();

private:
    explicit VisualViewport(DOM::Document&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<DOM::Document> m_document;
    CSSPixelPoint m_offset;
    double m_scale { 1.0 };
};

}
