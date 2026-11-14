/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/DOM/AbortController.h>

namespace Web::DOM {

extern "C" {
void* web_rust_abort_controller_create(JSVM*);
AbortSignal* web_rust_abort_controller_signal(AbortController const*);
void web_rust_abort_controller_abort(JSVM*, void* realm, AbortController*, JSValue reason);
}

GC::Ref<AbortController> AbortController::create()
{
    return *static_cast<AbortController*>(web_rust_abort_controller_create(JS::HostABI::vm_to_abi(JS::VM::the())));
}

GC::Ref<AbortSignal> AbortController::signal() const
{
    return *web_rust_abort_controller_signal(this);
}

void AbortController::abort(JS::Realm& realm, Optional<JS::Value> reason)
{
    web_rust_abort_controller_abort(JS::HostABI::vm_to_abi(realm.vm()), &realm, this, JS::HostABI::value_to_abi(reason.value_or(JS::js_undefined())));
}

}
