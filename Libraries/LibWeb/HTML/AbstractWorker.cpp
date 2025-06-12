/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/AbstractWorker.h>
#include <LibWeb/HTML/EventNames.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/workers.html#handler-abstractworker-onerror
WebIDL::CallbackType* AbstractWorker::onerror()
{
    return this_event_target().event_handler_attribute(HTML::EventNames::error);
}

// https://html.spec.whatwg.org/multipage/workers.html#handler-abstractworker-onerror
void AbstractWorker::set_onerror(WebIDL::CallbackType* event_handler)
{
    this_event_target().set_event_handler_attribute(HTML::EventNames::error, event_handler);
}

}
