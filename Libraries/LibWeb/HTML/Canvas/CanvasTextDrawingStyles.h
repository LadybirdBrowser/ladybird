/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibWeb/HTML/Canvas/AbstractCanvasMixin.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvastextdrawingstyles
template<typename CanvasType>
class CanvasTextDrawingStyles : protected virtual AbstractCanvasMixin {
public:
    ~CanvasTextDrawingStyles() = default;
    Utf16String font() const;
    void set_font(Utf16View font) override;

    // https://html.spec.whatwg.org/multipage/canvas.html#font-style-source-object
    Variant<DOM::Document*, HTML::WorkerGlobalScope*> get_font_source_for_font_style_source_object(CanvasType& font_style_source_object);

    Bindings::CanvasTextAlign text_align() const { return drawing_state().text_align; }
    void set_text_align(Bindings::CanvasTextAlign text_align) { drawing_state().text_align = text_align; }

    Bindings::CanvasTextBaseline text_baseline() const { return drawing_state().text_baseline; }
    void set_text_baseline(Bindings::CanvasTextBaseline text_baseline) { drawing_state().text_baseline = text_baseline; }

    Bindings::CanvasDirection direction() const { return drawing_state().direction; }
    void set_direction(Bindings::CanvasDirection direction) { drawing_state().direction = direction; }

    Utf16String letter_spacing() const;
    void set_letter_spacing(Utf16View);
    float resolved_letter_spacing() const override;

protected:
    CanvasTextDrawingStyles() = default;
};

}
