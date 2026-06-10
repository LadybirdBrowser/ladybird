/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Geometry/DOMRectReadOnly.h>

namespace Web::Bindings {

struct DOMRectInit;

}

namespace Web::Geometry {

// https://drafts.fxtf.org/geometry/#DOMRect
class DOMRect final : public DOMRectReadOnly {
    WEB_WRAPPABLE(DOMRect, DOMRectReadOnly);
    GC_DECLARE_ALLOCATOR(DOMRect);

public:
    [[nodiscard]] static GC::Ref<DOMRect> create(double x, double y, double width, double height);
    [[nodiscard]] static GC::Ref<DOMRect> create(Gfx::FloatRect const&);
    [[nodiscard]] static GC::Ref<DOMRect> create();
    [[nodiscard]] static GC::Ref<DOMRect> dom_rect_from_rect(Bindings::DOMRectInit const&);

    virtual ~DOMRect() override;

    double x() const { return m_rect.x(); }
    double y() const { return m_rect.y(); }
    double width() const { return m_rect.width(); }
    double height() const { return m_rect.height(); }

    void set_x(double x) { m_rect.set_x(x); }
    void set_y(double y) { m_rect.set_y(y); }
    void set_width(double width) { m_rect.set_width(width); }
    void set_height(double height) { m_rect.set_height(height); }

private:
    DOMRect(double x, double y, double width, double height);
    DOMRect();
};

}
