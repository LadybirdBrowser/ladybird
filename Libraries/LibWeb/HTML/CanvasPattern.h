/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/PaintStyle.h>
#include <LibWeb/Bindings/CanvasPattern.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/Canvas/CanvasDrawImage.h>

namespace Web::HTML {

class CanvasPattern final : public Bindings::Wrappable {
    WEB_WRAPPABLE(CanvasPattern, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(CanvasPattern);

public:
    static WebIDL::ExceptionOr<GC::Ptr<CanvasPattern>> create(JS::Realm&, CanvasImageSource const& image, StringView repetition);

    ~CanvasPattern();

    NonnullRefPtr<Gfx::PaintStyle> to_gfx_paint_style() { return m_pattern; }
    WebIDL::ExceptionOr<void> set_transform(Bindings::DOMMatrix2DInit& transform);

private:
    CanvasPattern(JS::Realm&, Gfx::CanvasPatternPaintStyle&);

    NonnullRefPtr<Gfx::CanvasPatternPaintStyle> m_pattern;
};

}
