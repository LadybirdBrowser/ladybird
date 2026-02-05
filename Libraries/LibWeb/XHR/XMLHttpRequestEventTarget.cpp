/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/XMLHttpRequestEventTargetPrototype.h>
#include <LibWeb/HTML/EventHandler.h>
#include <LibWeb/XHR/EventNames.h>
#include <LibWeb/XHR/XMLHttpRequestEventTarget.h>

namespace Web::XHR {

#undef __ENUMERATE
#define __ENUMERATE(attribute_name, event_name)                                       \
    void XMLHttpRequestEventTarget::set_##attribute_name(WebIDL::CallbackType* value) \
    {                                                                                 \
        set_event_handler_attribute(event_name, value);                               \
    }                                                                                 \
    WebIDL::CallbackType* XMLHttpRequestEventTarget::attribute_name()                 \
    {                                                                                 \
        return event_handler_attribute(event_name);                                   \
    }
ENUMERATE_XML_HTTP_REQUEST_EVENT_TARGET_EVENT_HANDLERS(__ENUMERATE)
#undef __ENUMERATE

}
