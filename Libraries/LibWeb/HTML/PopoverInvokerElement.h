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

    GC::Ptr<DOM::Element> get_popover_target_element() { return m_popover_target_element; }

    void set_popover_target_element(GC::Ptr<DOM::Element> value) { m_popover_target_element = value; }

protected:
    void visit_edges(JS::Cell::Visitor& visitor)
    {
        visitor.visit(m_popover_target_element);
    }

private:
    GC::Ptr<DOM::Element> m_popover_target_element;
};

}
