/*
 * Copyright (c) 2025, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/CommandEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/CommandEvent.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CommandEvent);

GC::Ref<CommandEvent> CommandEvent::create(JS::Realm& realm, FlyString const& event_name, CommandEventInit event_init)
{
    return realm.create<CommandEvent>(realm, event_name, move(event_init));
}

WebIDL::ExceptionOr<GC::Ref<CommandEvent>> CommandEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, CommandEventInit event_init)
{
    return create(realm, event_name, move(event_init));
}

CommandEvent::CommandEvent(JS::Realm& realm, FlyString const& event_name, CommandEventInit event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_source(event_init.source)
    , m_command(move(event_init.command))
{
}

void CommandEvent::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_source);
}

void CommandEvent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CommandEvent);
    Base::initialize(realm);
}

}
