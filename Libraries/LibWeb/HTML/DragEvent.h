/*
 * Copyright (c) 2024, Maciej <sppmacd@pm.me>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/UIEvents/MouseEvent.h>

namespace Web::HTML {

GC::Ref<DOM::Event> create_drag_event(Utf16FlyString const&, UIEvents::MouseEventOptions const&, double page_x, double page_y, double offset_x, double offset_y, HighResolutionTime::DOMHighResTimeStamp, DragDataStore const&, Utf16FlyString const& current_drag_operation);
GC::Ref<DOM::Event> create_drag_event_for_create_event(HighResolutionTime::DOMHighResTimeStamp);
void finish_drag_event(DOM::Event const&);
Utf16FlyString drag_event_drop_effect(DOM::Event const&);
Utf16FlyString drag_event_operation(DOM::Event const&);

}
