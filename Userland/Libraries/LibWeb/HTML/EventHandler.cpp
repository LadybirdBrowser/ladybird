/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/DOMEventListener.h>
#include <LibWeb/HTML/EventHandler.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(EventHandler);

EventHandler::EventHandler(ByteString s)
    : value(move(s))
{
}

EventHandler::EventHandler(WebIDL::CallbackType& c)
    : value(&c)
{
}

void EventHandler::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(listener);

    if (auto* callback = value.get_pointer<JS::GCPtr<WebIDL::CallbackType>>())
        visitor.visit(*callback);
}

}
