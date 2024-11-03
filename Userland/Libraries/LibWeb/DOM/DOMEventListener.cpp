/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/DOM/DOMEventListener.h>
#include <LibWeb/DOM/IDLEventListener.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(DOMEventListener);

DOMEventListener::DOMEventListener() = default;
DOMEventListener::~DOMEventListener() = default;

void DOMEventListener::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(callback);
    visitor.visit(signal);
}

}
