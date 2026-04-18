/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/Canvas/AbstractCanvasMixin.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvasstate
class CanvasState : protected virtual AbstractCanvasMixin {
public:
    virtual ~CanvasState() = default;

    void save();
    void restore();
    void reset();
    bool is_context_lost();

    DrawingState& drawing_state() override { return m_drawing_state; }
    DrawingState const& drawing_state() const override { return m_drawing_state; }

    CSS::ComputationContext computation_context_for_drawing_state() const override;

    void clear_drawing_state_stack() { m_drawing_state_stack.clear(); }
    void reset_drawing_state() { m_drawing_state = {}; }

    virtual void reset_to_default_state() = 0;

    void visit_edges(GC::Cell::Visitor& visitor)
    {
        m_drawing_state.visit_edges(visitor);
        for (auto& state : m_drawing_state_stack) {
            state.visit_edges(visitor);
        }
    }

protected:
    CanvasState() = default;

private:
    DrawingState m_drawing_state;
    Vector<DrawingState> m_drawing_state_stack;

    // https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-context-lost
    bool m_context_lost { false };
};

}
