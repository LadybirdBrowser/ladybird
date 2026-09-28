/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibCompositing/InputEvent.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/Page/DragEvent.h>
#include <LibWebCommon/Page/PageId.h>
#include <LibWebCommon/UIEvents/KeyCode.h>
#include <LibWebCommon/UIEvents/MouseButton.h>

namespace Web {

using InputEvent = Variant<Compositing::KeyEvent, Compositing::MouseEvent, DragEvent, Compositing::PinchEvent>;

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
