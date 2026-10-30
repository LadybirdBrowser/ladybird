/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/AbortSignal.h>
#include <LibWeb/DOM/DOMEventListener.h>
#include <LibWeb/DOM/IDLEventListener.h>

namespace Web::DOM {

#if ARCH(X86_64) && !defined(AK_OS_WINDOWS)
static_assert(sizeof(DOMEventListener) == 32);
#endif

GC_DEFINE_ALLOCATOR(DOMEventListener);

DOMEventListener::DOMEventListener() = default;
DOMEventListener::~DOMEventListener() = default;

void DOMEventListener::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(callback);
    visitor.visit(signal);
}

}
