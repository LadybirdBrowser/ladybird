/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/AbortController.h>
#include <LibWeb/DOM/AbortSignal.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(AbortController);

GC::Ref<AbortController> AbortController::create()
{
    auto signal = AbortSignal::create();
    return GC::Heap::the().allocate<AbortController>(move(signal));
}

// https://dom.spec.whatwg.org/#dom-abortcontroller-abortcontroller
AbortController::AbortController(GC::Ref<AbortSignal> signal)
    : m_signal(move(signal))
{
}

AbortController::~AbortController() = default;

void AbortController::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_signal);
}

// https://dom.spec.whatwg.org/#dom-abortcontroller-abort
void AbortController::abort(JS::Realm& realm, Optional<JS::Value> reason)
{
    // The abort(reason) method steps are to signal abort on this’s signal with reason if it is given.
    auto abort_reason = reason.value_or(JS::js_undefined());
    if (abort_reason.is_undefined())
        abort_reason = throw_completion(realm, WebIDL::AbortError::create("Aborted without reason"_utf16)).value();

    m_signal->signal_abort(abort_reason, realm.global_object());
}

void AbortController::abort(JS::Realm& realm, GC::Ref<WebIDL::DOMException> reason)
{
    abort(realm, throw_completion(realm, reason).value());
}

}
