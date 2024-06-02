/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Page/InputEvent.h>

#import <System/Cocoa.h>

namespace Ladybird {

Web::MouseEvent ns_event_to_mouse_event(Web::MouseEvent::Type, NSEvent*, NSView*, NSScrollView*, Web::UIEvents::MouseButton);
Web::KeyEvent ns_event_to_key_event(Web::KeyEvent::Type, NSEvent*);
NSEvent* key_event_to_ns_event(Web::KeyEvent const&);

NSEvent* create_context_menu_mouse_event(NSView*, Gfx::IntPoint);
NSEvent* create_context_menu_mouse_event(NSView*, NSPoint);

}
