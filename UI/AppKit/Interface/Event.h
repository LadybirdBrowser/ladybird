/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibCompositing/InputEvent.h>
#include <LibURL/Forward.h>
#include <LibWeb/Page/DragEvent.h>

#import <Cocoa/Cocoa.h>

namespace Ladybird {

Compositing::KeyModifier ns_modifiers_to_key_modifiers(NSEventModifierFlags);
Compositing::MouseEvent ns_event_to_mouse_event(Compositing::MouseEvent::Type, NSEvent*, NSView*, Compositing::MouseButton);

Web::DragEvent ns_event_to_drag_event(Web::DragEvent::Type, id<NSDraggingInfo>, NSView*);
Vector<URL::URL> drag_event_url_list(Web::DragEvent const&);

Compositing::KeyEvent ns_event_to_key_event(Compositing::KeyEvent::Type, NSEvent*, bool should_insert_text = false);
NSEvent* key_event_to_ns_event(Compositing::KeyEvent const&);

NSEvent* create_context_menu_mouse_event(NSView*, Gfx::IntPoint);
NSEvent* create_context_menu_mouse_event(NSView*, NSPoint);

}
