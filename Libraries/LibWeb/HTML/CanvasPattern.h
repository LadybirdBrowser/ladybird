/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/PaintStyle.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/Canvas/CanvasDrawImage.h>

namespace Web::Bindings {

struct DOMMatrix2DInit;

}

namespace Web::Geometry {

class DOMMatrix;

}

namespace Web::HTML {

class CanvasPattern final : public Bindings::Wrappable {
    WEB_WRAPPABLE(CanvasPattern, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(CanvasPattern);

public:
    static WebIDL::ExceptionOr<GC::Ptr<CanvasPattern>> create(CanvasImageSource const& image, StringView repetition);

    ~CanvasPattern();

    NonnullRefPtr<Gfx::PaintStyle> to_gfx_paint_style() { return m_pattern; }
    WebIDL::ExceptionOr<void> set_transform(GC::Ref<Geometry::DOMMatrix> transform);
    WebIDL::ExceptionOr<void> set_transform(Bindings::DOMMatrix2DInit const& transform);

private:
    CanvasPattern(Gfx::CanvasPatternPaintStyle&);

    NonnullRefPtr<Gfx::CanvasPatternPaintStyle> m_pattern;
};

}
