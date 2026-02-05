/*
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/Canvas/CanvasState.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvastextdrawingstyles
template<typename IncludingClass, typename CanvasType>
class CanvasTextDrawingStyles {
public:
    ~CanvasTextDrawingStyles() = default;
    ByteString font() const;
    void set_font(StringView font);

    // https://html.spec.whatwg.org/multipage/canvas.html#font-style-source-object
    Variant<DOM::Document*, HTML::WorkerGlobalScope*> get_font_source_for_font_style_source_object(CanvasType& font_style_source_object);

    Bindings::CanvasTextAlign text_align() const { return my_drawing_state().text_align; }
    void set_text_align(Bindings::CanvasTextAlign text_align) { my_drawing_state().text_align = text_align; }

    Bindings::CanvasTextBaseline text_baseline() const { return my_drawing_state().text_baseline; }
    void set_text_baseline(Bindings::CanvasTextBaseline text_baseline) { my_drawing_state().text_baseline = text_baseline; }

    Bindings::CanvasDirection direction() const { return my_drawing_state().direction; }
    void set_direction(Bindings::CanvasDirection direction) { my_drawing_state().direction = direction; }

protected:
    CanvasTextDrawingStyles() = default;

private:
    CanvasState::DrawingState& my_drawing_state() { return static_cast<IncludingClass&>(*this).drawing_state(); }
    CanvasState::DrawingState const& my_drawing_state() const { return static_cast<IncludingClass const&>(*this).drawing_state(); }
};

}
