/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/WindowEventHandlers.h>

namespace Web::HTML {

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                                 \
    void WindowEventHandlers::set_##attribute_name(WebIDL::CallbackType* value) \
    {                                                                           \
        if (auto event_target = window_event_handlers_to_event_target())        \
            event_target->set_event_handler_attribute(event_name, value);       \
    }                                                                           \
    WebIDL::CallbackType* WindowEventHandlers::attribute_name()                 \
    {                                                                           \
        if (auto event_target = window_event_handlers_to_event_target())        \
            return event_target->event_handler_attribute(event_name);           \
        return nullptr;                                                         \
    }
ENUMERATE_WINDOW_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

WindowEventHandlers::~WindowEventHandlers() = default;

}
