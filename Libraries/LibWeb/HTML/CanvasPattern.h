/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/PaintStyle.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/HTML/Canvas/CanvasDrawImage.h>

namespace Web::HTML {

class CanvasPatternPaintStyle final : public Gfx::PaintStyle {
public:
    enum class Repetition {
        Repeat,
        RepeatX,
        RepeatY,
        NoRepeat
    };

    static ErrorOr<NonnullRefPtr<CanvasPatternPaintStyle>> create(CanvasImageSource image, Repetition repetition)
    {
        return adopt_nonnull_ref_or_enomem(new (nothrow) CanvasPatternPaintStyle(move(image), repetition));
    }

    virtual void paint(Gfx::IntRect physical_bounding_box, PaintFunction paint) const override;

private:
    CanvasPatternPaintStyle(CanvasImageSource image, Repetition repetition)
        : m_image(move(image))
        , m_repetition(repetition)
    {
    }

    CanvasImageSource m_image;
    Repetition m_repetition { Repetition::Repeat };
};

class CanvasPattern final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CanvasPattern, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(CanvasPattern);

public:
    static WebIDL::ExceptionOr<GC::Ptr<CanvasPattern>> create(JS::Realm&, CanvasImageSource const& image, StringView repetition);

    ~CanvasPattern();

    NonnullRefPtr<Gfx::PaintStyle> to_gfx_paint_style() { return m_pattern; }

private:
    CanvasPattern(JS::Realm&, CanvasPatternPaintStyle&);

    virtual void initialize(JS::Realm&) override;

    NonnullRefPtr<CanvasPatternPaintStyle> m_pattern;
};

}
