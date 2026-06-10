/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/HTML/Canvas/CanvasState.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-save
void CanvasState::save()
{
    // The save() method steps are to push a copy of the current drawing state onto the drawing state stack.
    m_drawing_state_stack.append(m_drawing_state);

    if (auto* canvas_command_list = this->canvas_command_list())
        canvas_command_list->append(Gfx::CanvasCommands::Save {});
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-restore
void CanvasState::restore()
{
    // The restore() method steps are to pop the top entry in the drawing state stack, and reset the drawing state it describes. If there is no saved state, then the method must do nothing.
    if (m_drawing_state_stack.is_empty())
        return;
    m_drawing_state = m_drawing_state_stack.take_last();

    if (auto* canvas_command_list = this->canvas_command_list())
        canvas_command_list->append(Gfx::CanvasCommands::Restore {});
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

CSS::ComputationContext CanvasState::computation_context_for_drawing_state() const
{
    auto font_metrics = [&]() {
        // https://html.spec.whatwg.org/multipage/canvas.html#text-styles
        // NB: We lazily initialize the font, so if it hasn't been set yet we generate the font metrics based on the default font
        if (!m_drawing_state.font_style_value) {
            // When the object implementing the CanvasTextDrawingStyles interface is created, the font of the context must be set to 10px sans-serif.
            return CSS::Length::FontMetrics { 10, Platform::FontPlugin::the().default_font(8)->pixel_metrics(), CSS::InitialValues::line_height() };
        }

        VERIFY(m_drawing_state.current_font_cascade_list);
        auto const& first_font = m_drawing_state.current_font_cascade_list->font_for_code_point(' ');
        auto const& font_size = m_drawing_state.font_style_value->as_shorthand().longhand(CSS::PropertyID::FontSize)->as_length().length().absolute_length_to_px();

        return CSS::Length::FontMetrics { font_size, first_font.pixel_metrics(), CSS::InitialValues::line_height() };
    }();

    auto viewport_rect = canvas_element().visit(
        [&](GC::Ref<HTMLCanvasElement> const& canvas_element) {
            if (auto navigable = canvas_element->navigable())
                return navigable->viewport_rect();
            return CSSPixelRect { 0, 0, 0, 0 };
        },
        [&](GC::Ref<OffscreenCanvas> const&) {
            return CSSPixelRect { 0, 0, 0, 0 };
        });

    return {
        .length_resolution_context = {
            .viewport_rect = viewport_rect,
            .font_metrics = font_metrics,
            .root_font_metrics = font_metrics },

        // NB: We don't require an abstract element because tree counting and random() functions aren't allowed in
        //     non-font canvas context values
        .abstract_element = {},

        // FIXME: The spec doesn't specify what color scheme should be used here but other browsers always use light so
        //        we do too for compatibility. See https://github.com/whatwg/html/issues/12505
        .color_scheme = CSS::PreferredColorScheme::Light
    };
}

}
