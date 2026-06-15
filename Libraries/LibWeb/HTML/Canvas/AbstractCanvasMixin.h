/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/CanvasCommandList.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Canvas/DrawingState.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/OffscreenCanvas.h>

#pragma once

namespace Web::HTML {

class AbstractCanvasMixin {
protected:
    virtual Variant<GC::Ref<HTMLCanvasElement>, GC::Ref<OffscreenCanvas>> canvas_element() = 0;
    virtual Variant<GC::Ref<HTMLCanvasElement>, GC::Ref<OffscreenCanvas>> canvas_element() const = 0;
    virtual DrawingState& drawing_state() = 0;
    virtual DrawingState const& drawing_state() const = 0;
    virtual JS::Realm& my_realm() = 0;
    virtual Gfx::Path& mutable_path() = 0;
    virtual Gfx::CanvasCommandList* canvas_command_list() = 0;
    virtual CSS::ComputationContext computation_context_for_drawing_state() const = 0;
    Optional<Color> parse_a_css_color_value(StringView const& value) const;
};

}
