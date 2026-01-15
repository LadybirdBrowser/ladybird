/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <LibGfx/FontCascadeList.h>
#include <LibWeb/HTML/Canvas/AbstractCanvasRenderingContext2DBase.h>
#include <LibWeb/HTML/TextMetrics.h>

namespace Web::HTML {

struct PreparedText {
    Vector<NonnullRefPtr<Gfx::GlyphRun>> glyph_runs;
    Gfx::TextAlignment physical_alignment;
    Gfx::FloatRect bounding_box;
};

// https://html.spec.whatwg.org/multipage/canvas.html#canvastext
class CanvasText : virtual public AbstractCanvasRenderingContext2DBase {
public:
    void fill_text(Utf16String const&, float x, float y, Optional<double> max_width);
    void stroke_text(Utf16String const&, float x, float y, Optional<double> max_width);
    GC::Ref<TextMetrics> measure_text(Utf16String const&);

private:
    PreparedText prepare_text(Utf16String const& text, float max_width = INFINITY);

    [[nodiscard]] Gfx::Path text_path(Utf16String const& text, float x, float y, Optional<double> max_width);

    RefPtr<Gfx::FontCascadeList const> font_cascade_list();
};

}
