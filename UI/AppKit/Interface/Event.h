/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibURL/Forward.h>
#include <LibWeb/Page/InputEvent.h>

#import <Cocoa/Cocoa.h>

namespace Ladybird {

Web::MouseEvent ns_event_to_mouse_event(Web::MouseEvent::Type, NSEvent*, NSView*, Web::UIEvents::MouseButton);

Web::DragEvent ns_event_to_drag_event(Web::DragEvent::Type, id<NSDraggingInfo>, NSView*);
Vector<URL::URL> drag_event_url_list(Web::DragEvent const&);

Web::KeyEvent ns_event_to_key_event(Web::KeyEvent::Type, NSEvent*);
NSEvent* key_event_to_ns_event(Web::KeyEvent const&);

NSEvent* create_context_menu_mouse_event(NSView*, Gfx::IntPoint);
NSEvent* create_context_menu_mouse_event(NSView*, NSPoint);

}
