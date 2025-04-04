/*
 * Copyright (c) 2025, Glenn Skrzypczak <glenn.skrzypczak@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/Utils.h>

namespace Web::HTML {

struct CommandEventInit : public DOM::EventInit {
    GC::Ptr<DOM::Element> source;
    String command;
};

class CommandEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(CommandEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(CommandEvent);

public:
    [[nodiscard]] static GC::Ref<CommandEvent> create(JS::Realm&, FlyString const& event_name, CommandEventInit = {});
    static WebIDL::ExceptionOr<GC::Ref<CommandEvent>> construct_impl(JS::Realm&, FlyString const& event_name, CommandEventInit);

    // https://html.spec.whatwg.org/multipage/interaction.html#dom-commandevent-command
    String const& command() const { return m_command; }

    // https://html.spec.whatwg.org/multipage/interaction.html#dom-commandevent-source
    GC::Ptr<DOM::Element> source() const { return as<DOM::Element>(retarget(m_source, current_target())); }

private:
    void visit_edges(Visitor&) override;

    CommandEvent(JS::Realm&, FlyString const& event_name, CommandEventInit event_init);

    void initialize(JS::Realm&) override;

    GC::Ptr<DOM::Element> m_source;
    String m_command;
};

}
