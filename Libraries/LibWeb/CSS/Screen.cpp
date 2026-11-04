/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Rect.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/CSS/Screen.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>

namespace Web::CSS {

extern "C" {

// Defined in Bindings/Rust/src/css/exports.rs.
void* web_screen_create(JSVM*, void* realm, void* window);

// Declared in Bindings/Rust/src/css/screen.rs.
void web_css_screen_area(void* window, bool available, i32* width, i32* height);
bool web_css_screen_is_extended(void* window);
}

static HTML::Window& window_from_abi(void* window)
{
    return *static_cast<HTML::Window*>(static_cast<GC::Cell*>(window));
}

void web_css_screen_area(void* window, bool available, i32* width, i32* height)
{
    auto& page = window_from_abi(window).page();
    auto area = available ? page.web_exposed_available_screen_area() : page.web_exposed_screen_area();
    *width = area.width().to_int();
    *height = area.height().to_int();
}

// https://w3c.github.io/window-management/#dom-screen-isextended
bool web_css_screen_is_extended(void* window_pointer)
{
    auto& window = window_from_abi(window_pointer);

    // 1. If this's relevant global object's associated Document is not allowed to use
    //    the policy-controlled feature named "window-management", return false.
    if (!window.associated_document().is_allowed_to_use_feature(DOM::PolicyControlledFeature::WindowManagement))
        return false;

    // 2. Return true if the device has more than one screen, and false otherwise.
    return window.page().client().screen_count() > 1;
}

GC::Ref<Screen> Screen::create(HTML::Window& window)
{
    auto* screen = web_screen_create(JS::HostABI::vm_to_abi(JS::VM::the()), &HTML::relevant_realm(window), static_cast<GC::Cell*>(&window));
    return GC::Ref { *static_cast<Screen*>(screen) };
}

}
