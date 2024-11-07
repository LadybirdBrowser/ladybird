/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
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
    static bool dispatch(GC::Ref<EventTarget>, Event&, bool legacy_target_override, bool& legacy_output_did_listeners_throw);

private:
    static void invoke(Event::PathEntry&, Event&, Event::Phase, bool& legacy_output_did_listeners_throw);
    static bool inner_invoke(Event&, Vector<GC::Root<DOM::DOMEventListener>>&, Event::Phase, bool, bool&);
};

}
