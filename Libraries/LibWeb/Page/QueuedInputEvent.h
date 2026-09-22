/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/Page/DragEvent.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Page/PageId.h>

namespace Web {

using InputEvent = Variant<KeyEvent, MouseEvent, DragEvent, PinchEvent>;

inline u64 input_event_id(InputEvent const& event)
{
    return event.visit([](auto const& event) { return event.id; });
}

inline void set_input_event_id(InputEvent& event, u64 id)
{
    event.visit([&](auto& event) { event.id = id; });
}

struct QueuedInputEvent {
    Web::PageId page_id { 0 };
    InputEvent event;
    // The events coalesced into this one, which finish when it does.
    Vector<u64> coalesced_event_ids;
    // The local root the event targets when it is not the page's traversable: a navigable whose parent's document
    // another process hosts, which the UI process addresses by id.
    Optional<HTML::CrossProcessId> navigable_id;
};

}
