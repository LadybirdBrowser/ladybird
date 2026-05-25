/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Painter.h>
#include <LibWeb/HTML/Canvas/CanvasState.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-save
void CanvasState::save()
{
    // The save() method steps are to push a copy of the current drawing state onto the drawing state stack.
    m_drawing_state_stack.append(m_drawing_state);

    if (auto* painter = painter_for_canvas_state())
        painter->save();
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-restore
void CanvasState::restore()
{
    // The restore() method steps are to pop the top entry in the drawing state stack, and reset the drawing state it describes. If there is no saved state, then the method must do nothing.
    if (m_drawing_state_stack.is_empty())
        return;
    m_drawing_state = m_drawing_state_stack.take_last();

    if (auto* painter = painter_for_canvas_state())
        painter->restore();
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-reset
void CanvasState::reset()
{
    // The reset() method steps are to reset the rendering context to its default state.
    reset_to_default_state();
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-iscontextlost
bool CanvasState::is_context_lost()
{
    // The isContextLost() method steps are to return this's context lost.
    return m_context_lost;
}

}
