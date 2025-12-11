/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
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

GC_DEFINE_ALLOCATOR(Screen);

GC::Ref<Screen> Screen::create(HTML::Window& window)
{
    return window.realm().create<Screen>(window);
}

Screen::Screen(HTML::Window& window)
    : DOM::EventTarget(window.realm())
    , m_window(window)
{
}

void Screen::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Screen);
    Base::initialize(realm);
}

void Screen::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window);
    visitor.visit(m_orientation);
}

// https://drafts.csswg.org/cssom-view-1/#dom-screen-width
i32 Screen::width() const
{
    // The width attribute must return the width of the Web-exposed screen area.
    return window().page().web_exposed_screen_area().width().to_int();
}

// https://drafts.csswg.org/cssom-view-1/#dom-screen-height
i32 Screen::height() const
{
    // The height attribute must return the height of the Web-exposed screen area.
    return window().page().web_exposed_screen_area().height().to_int();
}

// https://drafts.csswg.org/cssom-view-1/#dom-screen-availwidth
i32 Screen::avail_width() const
{
    // The availWidth attribute must return the width of the Web-exposed available screen area.
    return window().page().web_exposed_available_screen_area().width().to_int();
}

// https://drafts.csswg.org/cssom-view-1/#dom-screen-availheight
i32 Screen::avail_height() const
{
    // The availHeight attribute must return the height of the Web-exposed available screen area.
    return window().page().web_exposed_available_screen_area().height().to_int();
}

// https://drafts.csswg.org/cssom-view-1/#dom-screen-colordepth
u32 Screen::color_depth() const
{
    // The colorDepth and pixelDepth attributes should return the number of bits allocated to colors for a pixel in
    // the output device, excluding the alpha channel.
    return 24;
}

// https://drafts.csswg.org/cssom-view-1/#dom-screen-pixeldepth
u32 Screen::pixel_depth() const
{
    // The colorDepth and pixelDepth attributes should return the number of bits allocated to colors for a pixel in
    // the output device, excluding the alpha channel.
    return 24;
}

GC::Ref<ScreenOrientation> Screen::orientation()
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
void Screen::set_onchange(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(HTML::EventNames::change, event_handler);
}

// https://w3c.github.io/window-management/#dom-screen-onchange
GC::Ptr<WebIDL::CallbackType> Screen::onchange()
{
    return event_handler_attribute(HTML::EventNames::change);
}

}
