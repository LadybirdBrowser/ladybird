/*
 * Copyright (c) 2024, Nathan van der Kamp <nbvdkamp@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class PopoverInvokerElement {

public:
    PopoverInvokerElement() { }

    GC::Ptr<DOM::Element> popover_target_element() { return m_popover_target_element; }

    void set_popover_target_element(GC::Ptr<DOM::Element> value) { m_popover_target_element = value; }

    static void popover_target_activation_behaviour(GC::Ref<DOM::Node> node, GC::Ref<DOM::Node> event_target);

protected:
    void visit_edges(JS::Cell::Visitor&);
    void associated_attribute_changed(FlyString const& name, Optional<String> const& value, Optional<FlyString> const& namespace_);

private:
    GC::Ptr<DOM::Element> m_popover_target_element;

    static GC::Ptr<HTMLElement> get_the_popover_target_element(GC::Ref<DOM::Node> node);
};

}
