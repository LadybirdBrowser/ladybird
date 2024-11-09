/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Rect.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ScreenPrototype.h>
#include <LibWeb/CSS/Screen.h>
#include <LibWeb/CSS/ScreenOrientation.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Page/Page.h>

namespace Web::CSS {

JS_DEFINE_ALLOCATOR(Screen);

JS::NonnullGCPtr<Screen> Screen::create(HTML::Window& window)
{
    return window.heap().allocate<Screen>(window.realm(), window);
}

Screen::Screen(HTML::Window& window)
    : DOM::EventTarget(window.realm())
    , m_window(window)
{
}

void Screen::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Screen);
}

void Screen::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window);
    visitor.visit(m_orientation);
}

Gfx::IntRect Screen::screen_rect() const
{
    auto screen_rect_in_css_pixels = window().page().web_exposed_screen_area();
    return {
        screen_rect_in_css_pixels.x().to_int(),
        screen_rect_in_css_pixels.y().to_int(),
        screen_rect_in_css_pixels.width().to_int(),
        screen_rect_in_css_pixels.height().to_int()
    };
}

JS::NonnullGCPtr<ScreenOrientation> Screen::orientation()
{
    if (!m_orientation)
        m_orientation = ScreenOrientation::create(realm());
    return *m_orientation;
}

// https://w3c.github.io/window-management/#dom-screen-isextended
bool Screen::is_extended() const
{
    dbgln("FIXME: Unimplemented Screen::is_extended");
    return false;
}

// https://w3c.github.io/window-management/#dom-screen-onchange
void Screen::set_onchange(JS::GCPtr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::change, event_handler);
}

// https://w3c.github.io/window-management/#dom-screen-onchange
JS::GCPtr<WebIDL::CallbackType> Screen::onchange()
{
    return event_handler_attribute(HTML::EventNames::change);
}

}
