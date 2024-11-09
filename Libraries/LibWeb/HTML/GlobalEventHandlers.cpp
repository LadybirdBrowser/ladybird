/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/GlobalEventHandlers.h>
#include <LibWeb/UIEvents/EventNames.h>

namespace Web::HTML {

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                                    \
    void GlobalEventHandlers::set_##attribute_name(WebIDL::CallbackType* value)    \
    {                                                                              \
        if (auto event_target = global_event_handlers_to_event_target(event_name)) \
            event_target->set_event_handler_attribute(event_name, value);          \
    }                                                                              \
    WebIDL::CallbackType* GlobalEventHandlers::attribute_name()                    \
    {                                                                              \
        if (auto event_target = global_event_handlers_to_event_target(event_name)) \
            return event_target->event_handler_attribute(event_name);              \
        return nullptr;                                                            \
    }
ENUMERATE_GLOBAL_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

GlobalEventHandlers::~GlobalEventHandlers() = default;
}
