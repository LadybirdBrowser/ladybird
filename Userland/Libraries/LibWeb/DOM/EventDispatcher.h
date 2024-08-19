/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class EventDispatcher {
public:
    static bool dispatch(GC::Ref<EventTarget>, Event&, bool legacy_target_override = false);

private:
    static void invoke(Event::PathEntry&, Event&, Event::Phase);
    static bool inner_invoke(Event&, Vector<GC::Handle<DOM::DOMEventListener>>&, Event::Phase, bool);
};

}
