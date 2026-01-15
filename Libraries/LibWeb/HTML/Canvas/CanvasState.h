/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/Color.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/Filter.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/FontCascadeList.h>
#include <LibGfx/PaintStyle.h>
#include <LibWeb/Bindings/CanvasRenderingContext2DPrototype.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/HTML/Canvas/AbstractCanvasRenderingContext2DBase.h>
#include <LibWeb/HTML/Canvas/CanvasDrawImage.h>
#include <LibWeb/HTML/Canvas/DrawingState.h>
#include <LibWeb/HTML/CanvasGradient.h>
#include <LibWeb/HTML/CanvasPattern.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasstate
class CanvasState : virtual public AbstractCanvasRenderingContext2DBase {
public:
    virtual Gfx::Painter* painter_for_canvas_state();
    virtual Gfx::Path& path_for_canvas_state() = 0;

    void save();
    void restore();
    void reset();
    bool is_context_lost();

    virtual DrawingState& drawing_state() override { return m_drawing_state; }
    virtual DrawingState const& drawing_state() const override { return m_drawing_state; }

    void clear_drawing_state_stack() { m_drawing_state_stack.clear(); }
    void reset_drawing_state() { m_drawing_state = {}; }

    virtual void reset_to_default_state() = 0;

    void visit_edges(GC::Cell::Visitor& visitor);

private:
    DrawingState m_drawing_state;
    Vector<DrawingState> m_drawing_state_stack;

    // https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-context-lost
    bool m_context_lost { false };
};

}
