/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/PaintStyle.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/Canvas/CanvasDrawImage.h>

namespace Web::HTML {

class CanvasPattern final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CanvasPattern, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CanvasPattern);

public:
    static WebIDL::ExceptionOr<GC::Ptr<CanvasPattern>> create(JS::Realm&, CanvasImageSource const& image, StringView repetition);

    ~CanvasPattern();

    NonnullRefPtr<Gfx::PaintStyle> to_gfx_paint_style() { return m_pattern; }
    WebIDL::ExceptionOr<void> set_transform(Geometry::DOMMatrix2DInit& transform);

private:
    CanvasPattern(JS::Realm&, Gfx::CanvasPatternPaintStyle&);

    virtual void initialize(JS::Realm&) override;

    NonnullRefPtr<Gfx::CanvasPatternPaintStyle> m_pattern;
};

}
